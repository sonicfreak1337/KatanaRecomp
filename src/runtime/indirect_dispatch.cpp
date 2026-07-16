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
        throw IndirectDispatchError(request.kind, request.callsite, target, request.source);
    }

    if (request.kind == IndirectDispatchKind::Call) { cpu.pr = request.return_address; }
    cpu.pc = target;
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
