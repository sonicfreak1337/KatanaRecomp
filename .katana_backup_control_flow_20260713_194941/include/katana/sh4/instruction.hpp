#pragma once

#include <cstdint>
#include <string>

namespace katana::sh4 {

enum class InstructionKind {
    Unknown,
    Nop,
    Rts,
    MovImmediate,
    AddImmediate,
    MovRegister,
    AddRegister
};

struct DecodedInstruction {
    std::uint16_t opcode = 0;
    InstructionKind kind = InstructionKind::Unknown;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::int32_t immediate = 0;

    std::string text;

    [[nodiscard]] bool is_known() const noexcept {
        return kind != InstructionKind::Unknown;
    }
};

}
