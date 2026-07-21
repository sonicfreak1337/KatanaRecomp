#include "katana/runtime/block_dispatch.hpp"

#include <stdexcept>

namespace katana::runtime {

CanonicalBlockDispatcher::CanonicalBlockDispatcher(RuntimeBlockTable& table,
                                                   DispatchDiagnosticRecorder* diagnostics,
                                                   DemandBlockMaterializer* materializer,
                                                   IndirectDispatchMetrics* metrics)
    : table_(table), diagnostics_(diagnostics), materializer_(materializer), metrics_(metrics) {}

BlockDispatchOutcome
CanonicalBlockDispatcher::dispatch(CpuState& cpu,
                                   BlockExecutionContext& context,
                                   const BlockVariantKey& variant,
                                   const BlockEndDefinition& end,
                                   const bool condition,
                                   const std::optional<std::uint32_t> dynamic_target) {
    std::optional<BlockAddress> target;
    std::optional<RuntimeBlockHandle> target_block;
    bool direct = false;
    switch (end.kind) {
    case BlockEndKind::Fallthrough:
        if (!end.fallthrough) {
            throw std::invalid_argument("Fallthrough braucht ein getrenntes Folgeziel.");
        }
        target = end.fallthrough;
        direct = true;
        break;
    case BlockEndKind::StaticBranch:
        if (end.direct_successors.size() != 1u) {
            throw std::invalid_argument("Statischer Sprung braucht genau ein Ziel.");
        }
        target = end.direct_successors[0];
        direct = true;
        break;
    case BlockEndKind::ConditionalBranch:
        if (end.direct_successors.size() != 1u || !end.fallthrough) {
            throw std::invalid_argument(
                "Bedingter Block braucht Sprungziel und getrennten Fallthrough.");
        }
        target =
            condition ? std::optional<BlockAddress>{end.direct_successors[0]} : end.fallthrough;
        direct = true;
        break;
    case BlockEndKind::DynamicBranch:
    case BlockEndKind::Call:
    case BlockEndKind::Return:
    case BlockEndKind::Exception:
    case BlockEndKind::ExceptionReturn:
    case BlockEndKind::InterruptSafepoint: {
        if (end.kind != BlockEndKind::Return && end.kind != BlockEndKind::ExceptionReturn &&
            end.kind != BlockEndKind::Exception && end.kind != BlockEndKind::InterruptSafepoint &&
            !dynamic_target) {
            throw std::invalid_argument("Dynamischer Block braucht ein Ziel.");
        }
        const auto kind = end.kind == BlockEndKind::Call     ? IndirectDispatchKind::Call
                          : end.kind == BlockEndKind::Return ? IndirectDispatchKind::Return
                                                             : IndirectDispatchKind::TailJump;
        const auto callsite = end.callsite.value_or(end.source.virtual_address);
        const auto return_address = end.kind == BlockEndKind::Call ? callsite + 4u : cpu.pr;
        const auto result = dispatch_indirect(cpu,
                                              table_,
                                              {kind,
                                               callsite,
                                               end.kind == BlockEndKind::ExceptionReturn ||
                                                       end.kind == BlockEndKind::Exception ||
                                                       end.kind == BlockEndKind::InterruptSafepoint
                                                   ? cpu.pc
                                                   : dynamic_target.value_or(0u),
                                               return_address,
                                               end.source,
                                               variant,
                                               DispatchResolutionOrigin::TableLookup,
                                               diagnostics_,
                                               RuntimeDispatchClass::GuardedFallback,
                                               metrics_,
                                               materializer_});
        target = BlockAddress{result.diagnostic_target, result.physical_target};
        target_block = result.block;
        break;
    }
    case BlockEndKind::Sleep:
        break;
    }
    if (direct && target) {
        const auto selected = dispatch_indirect(cpu,
                                                table_,
                                                {IndirectDispatchKind::TailJump,
                                                 end.source.virtual_address,
                                                 target->virtual_address,
                                                 cpu.pr,
                                                 end.source,
                                                 variant,
                                                 DispatchResolutionOrigin::StaticProof,
                                                 diagnostics_,
                                                 RuntimeDispatchClass::GuardedFallback,
                                                 metrics_,
                                                 materializer_});
        target_block = selected.block;
    }
    return {make_block_exit(cpu, context, end.kind, end.source, target), target_block, direct};
}

void CanonicalBlockDispatcher::link(std::string source_identity, std::string target_identity) {
    if (source_identity.empty() || target_identity.empty()) {
        throw std::invalid_argument("Link braucht Quell- und Zielidentitaet.");
    }
    incoming_[std::move(target_identity)].insert(std::move(source_identity));
}

std::vector<std::string>
CanonicalBlockDispatcher::unlink_target(const std::string& target_identity) {
    const auto found = incoming_.find(target_identity);
    if (found == incoming_.end()) {
        return {};
    }
    std::vector<std::string> result(found->second.begin(), found->second.end());
    incoming_.erase(found);
    return result;
}

std::size_t
CanonicalBlockDispatcher::incoming_link_count(const std::string& target_identity) const noexcept {
    const auto found = incoming_.find(target_identity);
    return found == incoming_.end() ? 0u : found->second.size();
}

} // namespace katana::runtime
