#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <optional>

namespace katana::sh4 {

[[nodiscard]] DecodedInstruction decode(std::uint16_t opcode);

[[nodiscard]] std::optional<std::uint32_t>
calculate_direct_branch_target(
    const DecodedInstruction& instruction,
    std::uint32_t instruction_address
);

}
