#include "katana/runtime/interpreter_boundary.hpp"

#include "katana/runtime/block_table.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

DispatchFallbackReason diagnostic_reason(const InterpreterRequest& request) noexcept {
    if (!request.manifest_allowed) return DispatchFallbackReason::ManifestDenied;
    if (request.dynamic_code) return DispatchFallbackReason::DynamicCode;
    if (request.reason == "unknown-opcode") return DispatchFallbackReason::UnknownOpcode;
    return DispatchFallbackReason::UnresolvedControlFlow;
}

void diagnose(DispatchDiagnosticRecorder* recorder,
              const InterpreterRequest& request,
              const CpuState& cpu,
              const DispatchDiagnosticError error,
              const std::uint32_t exit_pc,
              const bool exception) noexcept {
    if (recorder == nullptr) return;
    static_cast<void>(
        recorder->try_record({request.guest_pc,
                              request.guest_pc,
                              canonical_physical_address(request.guest_pc),
                              std::nullopt,
                              std::nullopt,
                              cpu.pr,
                              exception ? BlockEndKind::Exception : BlockEndKind::DynamicBranch,
                              DispatchResolutionOrigin::Fallback,
                              DispatchAliasOrigin::None,
                              diagnostic_reason(request),
                              request.manifest_allowed ? DispatchFallbackAction::Interpreter
                                                       : DispatchFallbackAction::Abort,
                              1u,
                              exit_pc,
                              error}));
}

} // namespace

PreciseInterpreterBoundary::PreciseInterpreterBoundary(SchedulerSafepoints& safepoints,
                                                       InterpreterStep step,
                                                       ExecutableCodeTracker* code_tracker,
                                                       DispatchDiagnosticRecorder* diagnostics)
    : safepoints_(safepoints), step_(std::move(step)), code_tracker_(code_tracker),
      diagnostics_(diagnostics) {
    if (!step_) {
        throw std::invalid_argument(
            "Interpretergrenze braucht einen expliziten Instruktionsschritt.");
    }
}

InterpreterResult PreciseInterpreterBoundary::execute(CpuState& cpu,
                                                      const InterpreterRequest& request) {
    if (request.reason.empty() || request.exit_boundary == 0u || request.guest_cycles == 0u) {
        throw std::invalid_argument(
            "Interpretereintritt braucht Grund, Austrittsgrenze und Gastzyklen.");
    }
    if (!request.manifest_allowed) {
        diagnose(diagnostics_,
                 request,
                 cpu,
                 DispatchDiagnosticError::FirmwareDenied,
                 request.guest_pc,
                 false);
        throw std::runtime_error("Manifest verbietet Fallback: " + request.reason);
    }
    ++counts_[request.reason];
    cpu.pc = request.guest_pc;
    GuestInstructionAttempt instruction_attempt(cpu, request.guest_pc, request.guest_cycles);
    if (request.dynamic_code && request.dynamic_size != 0u)
        cpu.active_instruction_physical_pc = request.dynamic_physical_address;
    const auto exception_generation_on_entry = cpu.exception_generation;
    bool supported = false;
    auto diagnostic_error = DispatchDiagnosticError::None;
    try {
        supported = step_(cpu, request);
    } catch (const MemoryAccessError& error) {
        diagnostic_error = error.reason() == MemoryAccessErrorReason::Misaligned
                               ? DispatchDiagnosticError::Misaligned
                               : DispatchDiagnosticError::UnmappedMemory;
        enter_memory_exception(cpu, error, request.guest_pc, request.delay_slot_owner);
    }
    if (!supported && cpu.exception_generation == exception_generation_on_entry) {
        diagnostic_error = DispatchDiagnosticError::UnknownCode;
        raise_illegal_instruction(cpu, request.guest_pc, request.delay_slot_owner);
    }

    const bool exception = cpu.exception_generation != exception_generation_on_entry;
    if (!exception) instruction_attempt.complete();
    const auto kind =
        request.delay_slot_owner ? SafepointKind::AfterDelaySlot : SafepointKind::BlockEnd;
    const auto pending_guest_cycles = cpu.pending_guest_cycles;
    const auto scheduler_cycle_before = safepoints_.current_cycle();
    const auto commit_delivered_cycles = [&]() noexcept {
        const auto scheduler_cycle_after = safepoints_.current_cycle();
        const auto delivered =
            scheduler_cycle_after >= scheduler_cycle_before
                ? std::min(pending_guest_cycles,
                           scheduler_cycle_after - scheduler_cycle_before)
                : 0u;
        const auto accountable = std::min(delivered, cpu.pending_guest_cycles);
        cpu.pending_guest_cycles -= accountable;
        cpu.total_guest_cycles += accountable;
    };
    SafepointReport safepoint;
    try {
        safepoint =
            safepoints_.consume(pending_guest_cycles, kind, ExecutionOrigin::Fallback);
    } catch (...) {
        commit_delivered_cycles();
        throw;
    }
    commit_delivered_cycles();
    if (!exception && cpu.pc != request.exit_boundary) {
        diagnose(
            diagnostics_, request, cpu, DispatchDiagnosticError::InvalidBoundary, cpu.pc, false);
        throw std::runtime_error(
            "Interpreter verliess die synchronisierte Blockgrenze nicht exakt.");
    }
    if (request.dynamic_code) {
        if (code_tracker_ == nullptr || request.dynamic_size == 0u || request.provenance.empty()) {
            throw std::runtime_error(
                "Dynamischer Interpretercode braucht Invalidierung und Provenienz.");
        }
        static_cast<void>(code_tracker_->register_block({"fallback-" + request.provenance,
                                                         request.dynamic_physical_address,
                                                         request.dynamic_size,
                                                         request.provenance,
                                                         {},
                                                         ExecutableBlockOrigin::FallbackDecode}));
    }
    diagnose(diagnostics_, request, cpu, diagnostic_error, cpu.pc, exception);
    return {!exception,
            exception,
            cpu.pc,
            exception ? cpu.last_exception_cause : ExceptionCause::None,
            safepoint};
}

std::uint64_t PreciseInterpreterBoundary::count(const std::string& reason) const noexcept {
    const auto found = counts_.find(reason);
    return found == counts_.end() ? 0u : found->second;
}
const std::map<std::string, std::uint64_t>& PreciseInterpreterBoundary::counts() const noexcept {
    return counts_;
}

} // namespace katana::runtime
