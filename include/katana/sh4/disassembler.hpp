#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace katana::io {
class ExecutableImage;
}

namespace katana::sh4 {

struct DisassemblyLine {
    std::uint32_t address = 0;
    std::uint16_t opcode = 0;
    DecodedInstruction instruction;

    bool is_delay_slot = false;
    std::optional<std::uint32_t> target_address;
};

[[nodiscard]] std::vector<DisassemblyLine> disassemble(
    std::span<const std::uint8_t> bytes,
    std::uint32_t base_address = 0
);

[[nodiscard]] std::vector<DisassemblyLine> disassemble(
    const katana::io::ExecutableImage& image
);

}
