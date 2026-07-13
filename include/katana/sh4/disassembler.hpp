#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace katana::sh4 {

struct DisassemblyLine {
    std::uint32_t address = 0;
    std::uint16_t opcode = 0;
    DecodedInstruction instruction;
};

[[nodiscard]] std::vector<DisassemblyLine> disassemble(
    std::span<const std::uint8_t> bytes,
    std::uint32_t base_address = 0
);

}
