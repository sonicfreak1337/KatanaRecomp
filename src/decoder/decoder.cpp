#include "katana/sh4/decoder.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace katana::sh4 {
namespace {

std::string register_name(const std::uint8_t index) {
    return "r" + std::to_string(index);
}

std::int32_t sign_extend_8(const std::uint16_t value) {
    const auto raw = static_cast<std::uint8_t>(value & 0x00FFu);

    if ((raw & 0x80u) == 0) {
        return raw;
    }

    return static_cast<std::int32_t>(raw) - 0x100;
}

std::string unknown_instruction_text(const std::uint16_t opcode) {
    std::ostringstream output;

    output
        << ".word 0x"
        << std::hex
        << std::uppercase
        << std::setw(4)
        << std::setfill('0')
        << opcode;

    return output.str();
}

}

DecodedInstruction decode(const std::uint16_t opcode) {
    DecodedInstruction instruction;
    instruction.opcode = opcode;

    if (opcode == 0x0009u) {
        instruction.kind = InstructionKind::Nop;
        instruction.text = "nop";
        return instruction;
    }

    if (opcode == 0x000Bu) {
        instruction.kind = InstructionKind::Rts;
        instruction.text = "rts";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xE000u) {
        instruction.kind = InstructionKind::MovImmediate;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text =
            "mov #" +
            std::to_string(instruction.immediate) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF000u) == 0x7000u) {
        instruction.kind = InstructionKind::AddImmediate;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text =
            "add #" +
            std::to_string(instruction.immediate) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6003u) {
        instruction.kind = InstructionKind::MovRegister;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);

        instruction.text =
            "mov " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Cu) {
        instruction.kind = InstructionKind::AddRegister;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);

        instruction.text =
            "add " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    instruction.text = unknown_instruction_text(opcode);
    return instruction;
}

}
