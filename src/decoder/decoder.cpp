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

void decode_memory_registers(
    DecodedInstruction& instruction,
    const std::uint16_t opcode
) {
    instruction.destination_register =
        static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

    instruction.source_register =
        static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
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

    if ((opcode & 0xF00Fu) == 0x3008u) {
        instruction.kind = InstructionKind::SubRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "sub " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x600Bu) {
        instruction.kind = InstructionKind::NegateRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "neg " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6007u) {
        instruction.kind = InstructionKind::NotRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "not " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x300Eu) {
        instruction.kind = InstructionKind::AddWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "addc " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Fu) {
        instruction.kind = InstructionKind::AddWithOverflow;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "addv " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Au) {
        instruction.kind = InstructionKind::SubWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "subc " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Bu) {
        instruction.kind = InstructionKind::SubWithOverflow;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "subv " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x600Au) {
        instruction.kind = InstructionKind::NegateWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "negc " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x600Cu) {
        instruction.kind = InstructionKind::ExtendUnsignedByte;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "extu.b " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x600Du) {
        instruction.kind = InstructionKind::ExtendUnsignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "extu.w " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x600Eu) {
        instruction.kind = InstructionKind::ExtendSignedByte;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "exts.b " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x600Fu) {
        instruction.kind = InstructionKind::ExtendSignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "exts.w " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6008u) {
        instruction.kind = InstructionKind::SwapBytes;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "swap.b " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6009u) {
        instruction.kind = InstructionKind::SwapWords;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "swap.w " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Du) {
        instruction.kind = InstructionKind::ExtractMiddle;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "xtrct " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF0FFu) == 0x4010u) {
        instruction.kind = InstructionKind::DecrementAndTest;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "dt " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x0029u) {
        instruction.kind = InstructionKind::MoveT;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "movt " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x2009u) {
        instruction.kind = InstructionKind::AndRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "and " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Au) {
        instruction.kind = InstructionKind::XorRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "xor " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Bu) {
        instruction.kind = InstructionKind::OrRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "or " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC900u) {
        instruction.kind = InstructionKind::AndImmediate;
        instruction.destination_register = 0;
        instruction.immediate =
            static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text =
            "and #" +
            std::to_string(instruction.immediate) +
            ", r0";

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xCA00u) {
        instruction.kind = InstructionKind::XorImmediate;
        instruction.destination_register = 0;
        instruction.immediate =
            static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text =
            "xor #" +
            std::to_string(instruction.immediate) +
            ", r0";

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xCB00u) {
        instruction.kind = InstructionKind::OrImmediate;
        instruction.destination_register = 0;
        instruction.immediate =
            static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text =
            "or #" +
            std::to_string(instruction.immediate) +
            ", r0";

        return instruction;
    }
    if (opcode == 0x0008u) {
        instruction.kind = InstructionKind::ClearT;
        instruction.text = "clrt";
        return instruction;
    }

    if (opcode == 0x0018u) {
        instruction.kind = InstructionKind::SetT;
        instruction.text = "sett";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8800u) {
        instruction.kind = InstructionKind::CompareEqualImmediate;
        instruction.destination_register = 0;
        instruction.immediate = sign_extend_8(opcode);

        instruction.text =
            "cmp/eq #" +
            std::to_string(instruction.immediate) +
            ", r0";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3000u) {
        instruction.kind = InstructionKind::CompareEqualRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/eq " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3002u) {
        instruction.kind = InstructionKind::CompareHigherOrSame;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/hs " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3003u) {
        instruction.kind = InstructionKind::CompareGreaterOrEqual;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/ge " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3006u) {
        instruction.kind = InstructionKind::CompareHigher;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/hi " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3007u) {
        instruction.kind = InstructionKind::CompareGreaterThan;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/gt " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4011u) {
        instruction.kind = InstructionKind::ComparePositiveOrZero;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "cmp/pz " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4015u) {
        instruction.kind = InstructionKind::ComparePositive;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "cmp/pl " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Cu) {
        instruction.kind = InstructionKind::CompareString;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "cmp/str " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xFF00u) == 0xC800u) {
        instruction.kind = InstructionKind::TestImmediate;
        instruction.destination_register = 0;
        instruction.immediate =
            static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text =
            "tst #" +
            std::to_string(instruction.immediate) +
            ", r0";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2008u) {
        instruction.kind = InstructionKind::TestRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "tst " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x2000u) {
        instruction.kind = InstructionKind::MovByteStore;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.b " +
            register_name(instruction.source_register) +
            ", @" +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2001u) {
        instruction.kind = InstructionKind::MovWordStore;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.w " +
            register_name(instruction.source_register) +
            ", @" +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2002u) {
        instruction.kind = InstructionKind::MovLongStore;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.l " +
            register_name(instruction.source_register) +
            ", @" +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6000u) {
        instruction.kind = InstructionKind::MovByteLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.b @" +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6001u) {
        instruction.kind = InstructionKind::MovWordLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.w @" +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6002u) {
        instruction.kind = InstructionKind::MovLongLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text =
            "mov.l @" +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);
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
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Cu) {
        instruction.kind = InstructionKind::AddRegister;
        decode_memory_registers(instruction, opcode);

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
