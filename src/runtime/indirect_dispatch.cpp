#include "katana/runtime/indirect_dispatch.hpp"

#include <iomanip>
#include <sstream>

namespace katana::runtime {
namespace {

const char* kind_name(const IndirectDispatchKind kind) noexcept {
    switch (kind) {
    case IndirectDispatchKind::Call: return "call";
    case IndirectDispatchKind::TailJump: return "tail-jump";
    case IndirectDispatchKind::Return: return "return";
    }
    return "unknown";
}

std::string describe(const IndirectDispatchRequest& request, const std::uint32_t target) {
    std::ostringstream out;
    out << kind_name(request.kind) << " callsite=0x" << std::hex << std::setw(8)
        << std::setfill('0') << request.callsite << " target=0x" << std::setw(8) << target
        << " pr=0x" << std::setw(8) << request.return_address
        << " source=" << stable_block_identity(request.source);
    return out.str();
}

BlockEndKind block_end(const IndirectDispatchKind kind) noexcept {
    switch (kind) {
        case IndirectDispatchKind::Call: return BlockEndKind::Call;
        case IndirectDispatchKind::TailJump: return BlockEndKind::DynamicBranch;
        case IndirectDispatchKind::Return: return BlockEndKind::Return;
    }
    return BlockEndKind::DynamicBranch;
}

void diagnose(
    const IndirectDispatchRequest& request,
    const std::uint32_t target,
    const std::uint32_t pr,
    const bool alias_lookup,
    const bool resolved
) noexcept {
    if (request.diagnostics == nullptr) return;
    static_cast<void>(request.diagnostics->try_record({
        request.callsite,
        request.source.virtual_address,
        canonical_physical_address(request.source.physical_address),
        target,
        canonical_physical_address(target),
        pr,
        block_end(request.kind),
        request.resolution_origin,
        resolved
            ? (alias_lookup
                ? DispatchAliasOrigin::CanonicalPhysical
                : DispatchAliasOrigin::ExactVirtual)
            : DispatchAliasOrigin::None,
        DispatchFallbackReason::None,
        DispatchFallbackAction::None,
        0u,
        resolved ? target : request.callsite,
        resolved ? DispatchDiagnosticError::None : DispatchDiagnosticError::UnknownTarget
    }));
}

} // namespace

IndirectDispatchError::IndirectDispatchError(
    const IndirectDispatchKind kind,
    const std::uint32_t callsite,
    const std::uint32_t target,
    const BlockAddress source
) : std::runtime_error([&] {
        IndirectDispatchRequest request;
        request.kind = kind;
        request.callsite = callsite;
        request.return_address = 0u;
        request.source = source;
        return "Unbekanntes indirektes Ziel: " + describe(request, target);
    }()) {}

IndirectDispatchResult dispatch_indirect(
    CpuState& cpu,
    const RuntimeBlockTable& table,
    const IndirectDispatchRequest& request
) {
    const auto target = request.kind == IndirectDispatchKind::Return ? cpu.pr : request.target;
    const auto physical = canonical_physical_address(target);
    const RuntimeBlock* block = table.lookup(target, request.variant);
    bool alias_lookup = false;
    if (block == nullptr) {
        block = table.lookup_physical(physical, request.variant);
        alias_lookup = block != nullptr;
    }
    if (block == nullptr) {
        diagnose(request, target, cpu.pr, false, false);
        throw IndirectDispatchError(request.kind, request.callsite, target, request.source);
    }

    if (request.kind == IndirectDispatchKind::Call) { cpu.pr = request.return_address; }
    cpu.pc = target;
    diagnose(request, target, cpu.pr, alias_lookup, true);
    return {
        block,
        target,
        physical,
        cpu.pc,
        cpu.pr,
        alias_lookup,
        describe(request, target) + (alias_lookup ? " alias=physical" : " alias=exact")
    };
}

} // namespace katana::runtime
