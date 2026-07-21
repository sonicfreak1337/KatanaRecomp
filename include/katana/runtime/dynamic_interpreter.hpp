#pragma once

#include "katana/runtime/block_abi.hpp"
#include "katana/runtime/platform_services.hpp"

#include <cstdint>

namespace katana::runtime {

struct DynamicInterpreterResult {
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    std::uint32_t start_pc = 0u;
    std::uint32_t byte_size = 0u;
    std::uint64_t instructions = 0u;
    std::uint64_t guest_cycles = 0u;
};

[[nodiscard]] DynamicInterpreterResult
execute_dynamic_sh4_block(CpuState& cpu,
                          PlatformServices& services,
                          std::uint64_t maximum_instructions = 64u);

} // namespace katana::runtime
