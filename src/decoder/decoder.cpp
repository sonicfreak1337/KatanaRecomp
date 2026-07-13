#include "katana/sh4/decoder.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace katana::sh4 {
namespace {

std::string register_name(const std::uint8_t index) {
    return "r" + std::to_string(index);
}

std::int32_t sign_extend_8(const std::uint16_t value) {
    const auto raw = static_cast<std::uint8_t>(value & 0x00FFu);

    if ((raw & 0x80u) == 0u) {
        return static_cast<std::int32_t>(raw);
    }

    return static_cast<std::int32_t>(raw) - 0x100;
}

std::int32_t sign_extend_12(const std::uint16_t value) {
    const auto raw = static_cast<std::uint16_t>(value & 0x0FFFu);

    if ((raw & 0x0800u) == 0u) {
        return static_cast<std::int32_t>(raw);
    }

    return static_cast<std::int32_t>(raw) - 0x1000;
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
        instruction.control_flow = ControlFlowKind::Return;
        instruction.has_delay_slot = true;
        instruction.text = "rts";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xA000u) {
        instruction.kind = InstructionKind::Bra;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::UnconditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bra";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xB000u) {
        instruction.kind = InstructionKind::Bsr;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::Call;
        instruction.has_delay_slot = true;
        instruction.text = "bsr";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8900u) {
        instruction.kind = InstructionKind::Bt;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bt";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8B00u) {
        instruction.kind = InstructionKind::Bf;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bf";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8D00u) {
        instruction.kind = InstructionKind::BtS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bt/s";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8F00u) {
        instruction.kind = InstructionKind::BfS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bf/s";
        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x402Bu) {
        instruction.kind = InstructionKind::Jmp;
        instruction.branch_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectBranch;
        instruction.has_delay_slot = true;
        instruction.text =
            "jmp @" + register_name(instruction.branch_register);
        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x400Bu) {
        instruction.kind = InstructionKind::Jsr;
        instruction.branch_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectCall;
        instruction.has_delay_slot = true;
        instruction.text =
            "jsr @" + register_name(instruction.branch_register);
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

std::optional<std::uint32_t> calculate_direct_branch_target(
    const DecodedInstruction& instruction,
    const std::uint32_t instruction_address
) {
    switch (instruction.kind) {
        case InstructionKind::Bra:
        case InstructionKind::Bsr:
        case InstructionKind::Bt:
        case InstructionKind::Bf:
        case InstructionKind::BtS:
        case InstructionKind::BfS:
            break;

        default:
            return std::nullopt;
    }

    const auto expanded_target =
        static_cast<std::int64_t>(instruction_address) +
        4 +
        static_cast<std::int64_t>(instruction.displacement);

    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(expanded_target) & 0xFFFFFFFFull
    );
}

}
