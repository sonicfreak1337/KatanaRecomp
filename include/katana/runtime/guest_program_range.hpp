#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstdint>

namespace katana::runtime {

// Physical executable backing expressed through any linear P0/P1/P2 guest alias.
// A matching instruction must fit completely inside the half-open byte range.
struct GuestProgramRange {
    std::uint32_t guest_start = 0u;
    std::uint32_t byte_size = 0u;

    [[nodiscard]] bool operator==(const GuestProgramRange&) const noexcept = default;
};

[[nodiscard]] bool valid_guest_program_range(GuestProgramRange range) noexcept;

// Translates the candidate as an instruction through the active guest address space and compares
// the resulting physical address with range. Translation failures are diagnostic misses: this
// function never enters a guest exception or mutates CpuState.
[[nodiscard]] bool guest_program_range_contains_instruction(
    const CpuState& cpu,
    std::uint32_t instruction_address,
    GuestProgramRange range) noexcept;

} // namespace katana::runtime
