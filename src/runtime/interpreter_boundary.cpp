#include "katana/runtime/interpreter_boundary.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

PreciseInterpreterBoundary::PreciseInterpreterBoundary(
    SchedulerSafepoints& safepoints,
    InterpreterStep step,
    ExecutableCodeTracker* code_tracker
) : safepoints_(safepoints), step_(std::move(step)), code_tracker_(code_tracker) {
    if (!step_) { throw std::invalid_argument("Interpretergrenze braucht einen expliziten Instruktionsschritt."); }
}

InterpreterResult PreciseInterpreterBoundary::execute(
    CpuState& cpu,
    const InterpreterRequest& request
) {
    if (request.reason.empty() || request.exit_boundary == 0u || request.guest_cycles == 0u) {
        throw std::invalid_argument("Interpretereintritt braucht Grund, Austrittsgrenze und Gastzyklen.");
    }
    if (!request.manifest_allowed) {
        throw std::runtime_error("Manifest verbietet Fallback: " + request.reason);
    }
    ++counts_[request.reason];
    cpu.pc = request.guest_pc;
    bool supported = false;
    try {
        supported = step_(cpu, request);
    } catch (const MemoryAccessError& error) {
        enter_memory_exception(cpu, error, request.guest_pc, request.delay_slot_owner);
    }
    if (!supported && cpu.last_exception_cause == ExceptionCause::None) {
        raise_illegal_instruction(cpu, request.guest_pc, request.delay_slot_owner);
    }

    const auto kind = request.delay_slot_owner ? SafepointKind::AfterDelaySlot : SafepointKind::BlockEnd;
    const auto safepoint = safepoints_.consume(request.guest_cycles, kind, ExecutionOrigin::Fallback);
    const bool exception = cpu.last_exception_cause != ExceptionCause::None;
    if (!exception && cpu.pc != request.exit_boundary) {
        throw std::runtime_error("Interpreter verliess die synchronisierte Blockgrenze nicht exakt.");
    }
    if (request.dynamic_code) {
        if (code_tracker_ == nullptr || request.dynamic_size == 0u || request.provenance.empty()) {
            throw std::runtime_error("Dynamischer Interpretercode braucht Invalidierung und Provenienz.");
        }
        code_tracker_->register_block({
            "fallback-" + request.provenance, request.dynamic_physical_address,
            request.dynamic_size, request.provenance, {}
        });
    }
    return {!exception, exception, cpu.pc, cpu.last_exception_cause, safepoint};
}

std::uint64_t PreciseInterpreterBoundary::count(const std::string& reason) const noexcept {
    const auto found = counts_.find(reason);
    return found == counts_.end() ? 0u : found->second;
}
const std::map<std::string, std::uint64_t>& PreciseInterpreterBoundary::counts() const noexcept { return counts_; }

} // namespace katana::runtime
