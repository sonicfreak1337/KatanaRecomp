#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/dispatch_diagnostics.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace katana::runtime {

enum class IndirectDispatchKind : std::uint8_t { Call, TailJump, Return };

struct IndirectDispatchRequest {
    IndirectDispatchKind kind = IndirectDispatchKind::TailJump;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t return_address = 0u;
    BlockAddress source;
    BlockVariantKey variant;
    DispatchResolutionOrigin resolution_origin = DispatchResolutionOrigin::TableLookup;
    DispatchDiagnosticRecorder* diagnostics = nullptr;
};

struct IndirectDispatchResult {
    const RuntimeBlock* block = nullptr;
    std::uint32_t diagnostic_target = 0u;
    std::uint32_t physical_target = 0u;
    std::uint32_t resulting_pc = 0u;
    std::uint32_t resulting_pr = 0u;
    bool alias_lookup = false;
    std::string diagnostic;
};

class IndirectDispatchError final : public std::runtime_error {
public:
    IndirectDispatchError(
        IndirectDispatchKind kind,
        std::uint32_t callsite,
        std::uint32_t target,
        BlockAddress source
    );
};

[[nodiscard]] IndirectDispatchResult dispatch_indirect(
    CpuState& cpu,
    const RuntimeBlockTable& table,
    const IndirectDispatchRequest& request
);

} // namespace katana::runtime
