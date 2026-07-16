#include "katana/runtime/controlled_fallback.hpp"

#include "katana/runtime/block_table.hpp"

#include <iomanip>
#include <numeric>
#include <sstream>
#include <utility>

namespace katana::runtime {
namespace {
const char* reason_name(const FallbackReason reason) noexcept {
    switch (reason) {
    case FallbackReason::UnknownOpcode: return "unknown-opcode";
    case FallbackReason::UnresolvedControlFlow: return "unresolved-control-flow";
    case FallbackReason::DynamicCode: return "dynamic-code";
    }
    return "invalid-reason";
}

DispatchFallbackReason diagnostic_reason(const FallbackReason reason) noexcept {
    switch (reason) {
        case FallbackReason::UnknownOpcode: return DispatchFallbackReason::UnknownOpcode;
        case FallbackReason::UnresolvedControlFlow:
            return DispatchFallbackReason::UnresolvedControlFlow;
        case FallbackReason::DynamicCode: return DispatchFallbackReason::DynamicCode;
    }
    return DispatchFallbackReason::None;
}

DispatchFallbackAction diagnostic_action(const FallbackPolicy policy) noexcept {
    switch (policy) {
        case FallbackPolicy::Abort: return DispatchFallbackAction::Abort;
        case FallbackPolicy::Diagnose: return DispatchFallbackAction::Diagnose;
        case FallbackPolicy::Interpreter: return DispatchFallbackAction::Interpreter;
        case FallbackPolicy::UserHook: return DispatchFallbackAction::UserHook;
    }
    return DispatchFallbackAction::None;
}

void diagnose(
    DispatchDiagnosticRecorder* recorder,
    const ControlledFallbackRequest& request,
    const CpuState& cpu,
    const FallbackPolicy policy,
    const std::uint32_t exit_pc,
    const DispatchDiagnosticError error = DispatchDiagnosticError::None
) noexcept {
    if (recorder == nullptr) return;
    static_cast<void>(recorder->try_record({
        request.guest_pc,
        request.guest_pc,
        canonical_physical_address(request.guest_pc),
        request.target,
        request.target.has_value()
            ? std::optional<std::uint32_t>{canonical_physical_address(*request.target)}
            : std::nullopt,
        cpu.pr,
        BlockEndKind::DynamicBranch,
        DispatchResolutionOrigin::Fallback,
        DispatchAliasOrigin::None,
        diagnostic_reason(request.reason),
        diagnostic_action(policy),
        request.guest_instructions,
        exit_pc,
        error
    }));
}
}

std::string stable_fallback_reason(const ControlledFallbackRequest& request) {
    std::ostringstream out;
    out << reason_name(request.reason) << " pc=0x" << std::hex << std::setw(8)
        << std::setfill('0') << request.guest_pc;
    if (request.opcode) { out << " opcode=0x" << std::setw(4) << *request.opcode; }
    if (request.target) { out << " target=0x" << std::setw(8) << *request.target; }
    out << " boundary=0x" << std::setw(8) << request.block_boundary;
    return out.str();
}

ControlledFallbackError::ControlledFallbackError(const ControlledFallbackRequest& request)
    : std::runtime_error("Kontrollierter Fallback abgebrochen: " + stable_fallback_reason(request)) {}

ControlledFallback::ControlledFallback(
    const FallbackPolicy policy,
    ControlledFallbackHandler handler,
    DispatchDiagnosticRecorder* diagnostics
) : policy_(policy), handler_(std::move(handler)), diagnostics_(diagnostics) {
    if ((policy == FallbackPolicy::Interpreter || policy == FallbackPolicy::UserHook) && !handler_) {
        throw std::invalid_argument("Interpreter- oder Nutzerhook-Fallback benoetigt einen expliziten Handler.");
    }
}

ControlledFallbackResult ControlledFallback::enter(
    CpuState& cpu,
    BlockExecutionContext& context,
    const ControlledFallbackRequest& request
) {
    ++counts_.at(static_cast<std::size_t>(request.reason));
    cpu.pc = request.guest_pc;
    context.sync_point = BlockSyncPoint::FallbackBoundary;
    const auto reason = stable_fallback_reason(request);
    if (policy_ == FallbackPolicy::Abort) {
        diagnose(diagnostics_, request, cpu, policy_, cpu.pc);
        throw ControlledFallbackError(request);
    }
    if (policy_ == FallbackPolicy::Diagnose) {
        diagnose(diagnostics_, request, cpu, policy_, cpu.pc);
        return {false, true, cpu.pc, context.scheduler_cycle, reason};
    }

    const auto next_pc = handler_(cpu, context, request);
    if (next_pc != request.block_boundary) {
        diagnose(
            diagnostics_, request, cpu, policy_, next_pc,
            DispatchDiagnosticError::InvalidBoundary
        );
        throw std::runtime_error(
            "Fallback verliess Interpreter ausserhalb der dokumentierten Blockgrenze: " + reason
        );
    }
    cpu.pc = next_pc;
    diagnose(diagnostics_, request, cpu, policy_, next_pc);
    return {true, true, next_pc, context.scheduler_cycle, reason};
}

std::uint64_t ControlledFallback::count(const FallbackReason reason) const noexcept {
    return counts_[static_cast<std::size_t>(reason)];
}

std::uint64_t ControlledFallback::total_count() const noexcept {
    return std::accumulate(counts_.begin(), counts_.end(), std::uint64_t{0u});
}

} // namespace katana::runtime
