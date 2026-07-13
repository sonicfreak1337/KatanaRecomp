#include "katana/sh4/decoder.hpp"

#include <array>
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

struct SpecialRegisterEncoding {
    std::uint16_t code;
    SpecialRegister special_register;
    const char* name;
    bool privileged;
};

template <std::size_t Size>
bool decode_special_register_family(
    DecodedInstruction& instruction,
    const std::uint16_t opcode,
    const std::array<SpecialRegisterEncoding, Size>& encodings,
    const InstructionKind kind,
    const char* mnemonic,
    const bool memory_form,
    const bool register_is_source
) {
    for (const auto& encoding : encodings) {
        if ((opcode & 0xF0FFu) != encoding.code) {
            continue;
        }

        const auto general_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.kind = kind;
        instruction.special_register = encoding.special_register;
        instruction.is_privileged = encoding.privileged;

        if (register_is_source) {
            instruction.source_register = general_register;
        } else {
            instruction.destination_register = general_register;
        }

        if (memory_form && register_is_source) {
            instruction.text =
                std::string(mnemonic) + " @" +
                register_name(general_register) + "+, " +
                encoding.name;
        } else if (memory_form) {
            instruction.text =
                std::string(mnemonic) + " " + encoding.name +
                ", @-" + register_name(general_register);
        } else if (register_is_source) {
            instruction.text =
                std::string(mnemonic) + " " +
                register_name(general_register) + ", " +
                encoding.name;
        } else {
            instruction.text =
                std::string(mnemonic) + " " + encoding.name +
                ", " + register_name(general_register);
        }
        return true;
    }
    return false;
}

bool decode_special_register_transfer(
    DecodedInstruction& instruction,
    const std::uint16_t opcode
) {
    constexpr std::array system_direct_store = {
        SpecialRegisterEncoding{0x000Au, SpecialRegister::Mach, "mach", false},
        SpecialRegisterEncoding{0x001Au, SpecialRegister::Macl, "macl", false},
        SpecialRegisterEncoding{0x002Au, SpecialRegister::Pr, "pr", false},
        SpecialRegisterEncoding{0x005Au, SpecialRegister::Fpul, "fpul", false},
        SpecialRegisterEncoding{0x006Au, SpecialRegister::Fpscr, "fpscr", false}
    };
    constexpr std::array system_memory_store = {
        SpecialRegisterEncoding{0x4002u, SpecialRegister::Mach, "mach", false},
        SpecialRegisterEncoding{0x4012u, SpecialRegister::Macl, "macl", false},
        SpecialRegisterEncoding{0x4022u, SpecialRegister::Pr, "pr", false},
        SpecialRegisterEncoding{0x4052u, SpecialRegister::Fpul, "fpul", false},
        SpecialRegisterEncoding{0x4062u, SpecialRegister::Fpscr, "fpscr", false}
    };
    constexpr std::array system_direct_load = {
        SpecialRegisterEncoding{0x400Au, SpecialRegister::Mach, "mach", false},
        SpecialRegisterEncoding{0x401Au, SpecialRegister::Macl, "macl", false},
        SpecialRegisterEncoding{0x402Au, SpecialRegister::Pr, "pr", false},
        SpecialRegisterEncoding{0x405Au, SpecialRegister::Fpul, "fpul", false},
        SpecialRegisterEncoding{0x406Au, SpecialRegister::Fpscr, "fpscr", false}
    };
    constexpr std::array system_memory_load = {
        SpecialRegisterEncoding{0x4006u, SpecialRegister::Mach, "mach", false},
        SpecialRegisterEncoding{0x4016u, SpecialRegister::Macl, "macl", false},
        SpecialRegisterEncoding{0x4026u, SpecialRegister::Pr, "pr", false},
        SpecialRegisterEncoding{0x4056u, SpecialRegister::Fpul, "fpul", false},
        SpecialRegisterEncoding{0x4066u, SpecialRegister::Fpscr, "fpscr", false}
    };
    constexpr std::array control_direct_store = {
        SpecialRegisterEncoding{0x0002u, SpecialRegister::Sr, "sr", true},
        SpecialRegisterEncoding{0x0012u, SpecialRegister::Gbr, "gbr", false},
        SpecialRegisterEncoding{0x0022u, SpecialRegister::Vbr, "vbr", true},
        SpecialRegisterEncoding{0x0032u, SpecialRegister::Ssr, "ssr", true},
        SpecialRegisterEncoding{0x0042u, SpecialRegister::Spc, "spc", true},
        SpecialRegisterEncoding{0x003Au, SpecialRegister::Sgr, "sgr", true},
        SpecialRegisterEncoding{0x00FAu, SpecialRegister::Dbr, "dbr", true},
        SpecialRegisterEncoding{0x0082u, SpecialRegister::Bank0, "r0_bank", true},
        SpecialRegisterEncoding{0x0092u, SpecialRegister::Bank1, "r1_bank", true},
        SpecialRegisterEncoding{0x00A2u, SpecialRegister::Bank2, "r2_bank", true},
        SpecialRegisterEncoding{0x00B2u, SpecialRegister::Bank3, "r3_bank", true},
        SpecialRegisterEncoding{0x00C2u, SpecialRegister::Bank4, "r4_bank", true},
        SpecialRegisterEncoding{0x00D2u, SpecialRegister::Bank5, "r5_bank", true},
        SpecialRegisterEncoding{0x00E2u, SpecialRegister::Bank6, "r6_bank", true},
        SpecialRegisterEncoding{0x00F2u, SpecialRegister::Bank7, "r7_bank", true}
    };
    constexpr std::array control_memory_store = {
        SpecialRegisterEncoding{0x4003u, SpecialRegister::Sr, "sr", true},
        SpecialRegisterEncoding{0x4013u, SpecialRegister::Gbr, "gbr", false},
        SpecialRegisterEncoding{0x4023u, SpecialRegister::Vbr, "vbr", true},
        SpecialRegisterEncoding{0x4033u, SpecialRegister::Ssr, "ssr", true},
        SpecialRegisterEncoding{0x4043u, SpecialRegister::Spc, "spc", true},
        SpecialRegisterEncoding{0x4032u, SpecialRegister::Sgr, "sgr", true},
        SpecialRegisterEncoding{0x40F2u, SpecialRegister::Dbr, "dbr", true},
        SpecialRegisterEncoding{0x4083u, SpecialRegister::Bank0, "r0_bank", true},
        SpecialRegisterEncoding{0x4093u, SpecialRegister::Bank1, "r1_bank", true},
        SpecialRegisterEncoding{0x40A3u, SpecialRegister::Bank2, "r2_bank", true},
        SpecialRegisterEncoding{0x40B3u, SpecialRegister::Bank3, "r3_bank", true},
        SpecialRegisterEncoding{0x40C3u, SpecialRegister::Bank4, "r4_bank", true},
        SpecialRegisterEncoding{0x40D3u, SpecialRegister::Bank5, "r5_bank", true},
        SpecialRegisterEncoding{0x40E3u, SpecialRegister::Bank6, "r6_bank", true},
        SpecialRegisterEncoding{0x40F3u, SpecialRegister::Bank7, "r7_bank", true}
    };
    constexpr std::array control_direct_load = {
        SpecialRegisterEncoding{0x400Eu, SpecialRegister::Sr, "sr", true},
        SpecialRegisterEncoding{0x401Eu, SpecialRegister::Gbr, "gbr", false},
        SpecialRegisterEncoding{0x402Eu, SpecialRegister::Vbr, "vbr", true},
        SpecialRegisterEncoding{0x403Eu, SpecialRegister::Ssr, "ssr", true},
        SpecialRegisterEncoding{0x404Eu, SpecialRegister::Spc, "spc", true},
        SpecialRegisterEncoding{0x40FAu, SpecialRegister::Dbr, "dbr", true},
        SpecialRegisterEncoding{0x408Eu, SpecialRegister::Bank0, "r0_bank", true},
        SpecialRegisterEncoding{0x409Eu, SpecialRegister::Bank1, "r1_bank", true},
        SpecialRegisterEncoding{0x40AEu, SpecialRegister::Bank2, "r2_bank", true},
        SpecialRegisterEncoding{0x40BEu, SpecialRegister::Bank3, "r3_bank", true},
        SpecialRegisterEncoding{0x40CEu, SpecialRegister::Bank4, "r4_bank", true},
        SpecialRegisterEncoding{0x40DEu, SpecialRegister::Bank5, "r5_bank", true},
        SpecialRegisterEncoding{0x40EEu, SpecialRegister::Bank6, "r6_bank", true},
        SpecialRegisterEncoding{0x40FEu, SpecialRegister::Bank7, "r7_bank", true}
    };
    constexpr std::array control_memory_load = {
        SpecialRegisterEncoding{0x4007u, SpecialRegister::Sr, "sr", true},
        SpecialRegisterEncoding{0x4017u, SpecialRegister::Gbr, "gbr", false},
        SpecialRegisterEncoding{0x4027u, SpecialRegister::Vbr, "vbr", true},
        SpecialRegisterEncoding{0x4037u, SpecialRegister::Ssr, "ssr", true},
        SpecialRegisterEncoding{0x4047u, SpecialRegister::Spc, "spc", true},
        SpecialRegisterEncoding{0x40F6u, SpecialRegister::Dbr, "dbr", true},
        SpecialRegisterEncoding{0x4087u, SpecialRegister::Bank0, "r0_bank", true},
        SpecialRegisterEncoding{0x4097u, SpecialRegister::Bank1, "r1_bank", true},
        SpecialRegisterEncoding{0x40A7u, SpecialRegister::Bank2, "r2_bank", true},
        SpecialRegisterEncoding{0x40B7u, SpecialRegister::Bank3, "r3_bank", true},
        SpecialRegisterEncoding{0x40C7u, SpecialRegister::Bank4, "r4_bank", true},
        SpecialRegisterEncoding{0x40D7u, SpecialRegister::Bank5, "r5_bank", true},
        SpecialRegisterEncoding{0x40E7u, SpecialRegister::Bank6, "r6_bank", true},
        SpecialRegisterEncoding{0x40F7u, SpecialRegister::Bank7, "r7_bank", true}
    };

    return
        decode_special_register_family(instruction, opcode, system_direct_store, InstructionKind::StoreSpecialRegister, "sts", false, false) ||
        decode_special_register_family(instruction, opcode, system_memory_store, InstructionKind::StoreSpecialRegisterPreDecrement, "sts.l", true, false) ||
        decode_special_register_family(instruction, opcode, system_direct_load, InstructionKind::LoadSpecialRegister, "lds", false, true) ||
        decode_special_register_family(instruction, opcode, system_memory_load, InstructionKind::LoadSpecialRegisterPostIncrement, "lds.l", true, true) ||
        decode_special_register_family(instruction, opcode, control_direct_store, InstructionKind::StoreSpecialRegister, "stc", false, false) ||
        decode_special_register_family(instruction, opcode, control_memory_store, InstructionKind::StoreSpecialRegisterPreDecrement, "stc.l", true, false) ||
        decode_special_register_family(instruction, opcode, control_direct_load, InstructionKind::LoadSpecialRegister, "ldc", false, true) ||
        decode_special_register_family(instruction, opcode, control_memory_load, InstructionKind::LoadSpecialRegisterPostIncrement, "ldc.l", true, true);
}

}

DecodedInstruction decode(const std::uint16_t opcode) {
    DecodedInstruction instruction;
    instruction.opcode = opcode;

    if (decode_special_register_transfer(instruction, opcode)) {
        return instruction;
    }

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
    if ((opcode & 0xF0FFu) == 0x4000u) {
        instruction.kind = InstructionKind::ShiftLogicalLeftOne;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shll " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4001u) {
        instruction.kind = InstructionKind::ShiftLogicalRightOne;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shlr " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4020u) {
        instruction.kind = InstructionKind::ShiftArithmeticLeftOne;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shal " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4021u) {
        instruction.kind = InstructionKind::ShiftArithmeticRightOne;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shar " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF0FFu) == 0x4008u) {
        instruction.kind = InstructionKind::ShiftLogicalLeftTwo;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shll2 " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4018u) {
        instruction.kind = InstructionKind::ShiftLogicalLeftEight;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shll8 " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4028u) {
        instruction.kind = InstructionKind::ShiftLogicalLeftSixteen;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shll16 " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4009u) {
        instruction.kind = InstructionKind::ShiftLogicalRightTwo;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shlr2 " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4019u) {
        instruction.kind = InstructionKind::ShiftLogicalRightEight;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shlr8 " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4029u) {
        instruction.kind = InstructionKind::ShiftLogicalRightSixteen;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "shlr16 " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF0FFu) == 0x4004u) {
        instruction.kind = InstructionKind::RotateLeft;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "rotl " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4005u) {
        instruction.kind = InstructionKind::RotateRight;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "rotr " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4024u) {
        instruction.kind = InstructionKind::RotateLeftThroughT;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "rotcl " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x4025u) {
        instruction.kind = InstructionKind::RotateRightThroughT;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text =
            "rotcr " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x400Cu) {
        instruction.kind = InstructionKind::ShiftArithmeticDynamic;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "shad " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x400Du) {
        instruction.kind = InstructionKind::ShiftLogicalDynamic;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "shld " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if (opcode == 0x0019u) {
        instruction.kind = InstructionKind::DivideInitializeUnsigned;
        instruction.text = "div0u";
        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2007u) {
        instruction.kind = InstructionKind::DivideInitializeSigned;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "div0s " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3004u) {
        instruction.kind = InstructionKind::DivideStep;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "div1 " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x400Fu) {
        instruction.kind = InstructionKind::MultiplyAccumulateWord;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mac.w @" +
            register_name(instruction.source_register) +
            "+, @" +
            register_name(instruction.destination_register) +
            "+";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x000Fu) {
        instruction.kind = InstructionKind::MultiplyAccumulateLong;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mac.l @" +
            register_name(instruction.source_register) +
            "+, @" +
            register_name(instruction.destination_register) +
            "+";

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x300Du) {
        instruction.kind = InstructionKind::DoubleMultiplySignedLong;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "dmuls.l " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x3005u) {
        instruction.kind = InstructionKind::DoubleMultiplyUnsignedLong;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "dmulu.l " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }
    if ((opcode & 0xF00Fu) == 0x0007u) {
        instruction.kind = InstructionKind::MultiplyLong;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mul.l " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Fu) {
        instruction.kind = InstructionKind::MultiplySignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "muls.w " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x200Eu) {
        instruction.kind = InstructionKind::MultiplyUnsignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mulu.w " +
            register_name(instruction.source_register) +
            ", " +
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
    if (opcode == 0x0048u) {
        instruction.kind = InstructionKind::ClearS;
        instruction.text = "clrs";
        return instruction;
    }

    if (opcode == 0x0058u) {
        instruction.kind = InstructionKind::SetS;
        instruction.text = "sets";
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
    if ((opcode & 0xF00Fu) == 0x2004u) {
        instruction.kind =
            InstructionKind::MovByteStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.b " +
            register_name(instruction.source_register) +
            ", @-" +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2005u) {
        instruction.kind =
            InstructionKind::MovWordStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.w " +
            register_name(instruction.source_register) +
            ", @-" +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x2006u) {
        instruction.kind =
            InstructionKind::MovLongStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.l " +
            register_name(instruction.source_register) +
            ", @-" +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6004u) {
        instruction.kind =
            InstructionKind::MovByteLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.b @" +
            register_name(instruction.source_register) +
            "+, " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6005u) {
        instruction.kind =
            InstructionKind::MovWordLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.w @" +
            register_name(instruction.source_register) +
            "+, " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6006u) {
        instruction.kind =
            InstructionKind::MovLongLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.l @" +
            register_name(instruction.source_register) +
            "+, " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8000u) {
        instruction.kind =
            InstructionKind::MovByteStoreDisplacement;
        instruction.source_register = 0u;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>(opcode & 0x000Fu);

        instruction.text =
            "mov.b r0, @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8100u) {
        instruction.kind =
            InstructionKind::MovWordStoreDisplacement;
        instruction.source_register = 0u;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x000Fu) * 2u);

        instruction.text =
            "mov.w r0, @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xF000u) == 0x1000u) {
        instruction.kind =
            InstructionKind::MovLongStoreDisplacement;
        decode_memory_registers(instruction, opcode);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x000Fu) * 4u);

        instruction.text =
            "mov.l " +
            register_name(instruction.source_register) +
            ", @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8400u) {
        instruction.kind =
            InstructionKind::MovByteLoadDisplacement;
        instruction.destination_register = 0u;
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>(opcode & 0x000Fu);

        instruction.text =
            "mov.b @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.source_register) +
            "), r0";

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8500u) {
        instruction.kind =
            InstructionKind::MovWordLoadDisplacement;
        instruction.destination_register = 0u;
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x000Fu) * 2u);

        instruction.text =
            "mov.w @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.source_register) +
            "), r0";

        return instruction;
    }

    if ((opcode & 0xF000u) == 0x5000u) {
        instruction.kind =
            InstructionKind::MovLongLoadDisplacement;
        decode_memory_registers(instruction, opcode);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x000Fu) * 4u);

        instruction.text =
            "mov.l @(" +
            std::to_string(instruction.displacement) +
            ", " +
            register_name(instruction.source_register) +
            "), " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x0004u) {
        instruction.kind =
            InstructionKind::MovByteStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.b " +
            register_name(instruction.source_register) +
            ", @(r0, " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x0005u) {
        instruction.kind =
            InstructionKind::MovWordStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.w " +
            register_name(instruction.source_register) +
            ", @(r0, " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x0006u) {
        instruction.kind =
            InstructionKind::MovLongStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.l " +
            register_name(instruction.source_register) +
            ", @(r0, " +
            register_name(instruction.destination_register) +
            ")";

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x000Cu) {
        instruction.kind =
            InstructionKind::MovByteLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.b @(r0, " +
            register_name(instruction.source_register) +
            "), " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x000Du) {
        instruction.kind =
            InstructionKind::MovWordLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.w @(r0, " +
            register_name(instruction.source_register) +
            "), " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x000Eu) {
        instruction.kind =
            InstructionKind::MovLongLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text =
            "mov.l @(r0, " +
            register_name(instruction.source_register) +
            "), " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC000u) {
        instruction.kind =
            InstructionKind::MovByteStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>(opcode & 0x00FFu);
        instruction.text =
            "mov.b r0, @(" +
            std::to_string(instruction.displacement) +
            ", gbr)";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC100u) {
        instruction.kind =
            InstructionKind::MovWordStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text =
            "mov.w r0, @(" +
            std::to_string(instruction.displacement) +
            ", gbr)";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC200u) {
        instruction.kind =
            InstructionKind::MovLongStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text =
            "mov.l r0, @(" +
            std::to_string(instruction.displacement) +
            ", gbr)";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC400u) {
        instruction.kind =
            InstructionKind::MovByteLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>(opcode & 0x00FFu);
        instruction.text =
            "mov.b @(" +
            std::to_string(instruction.displacement) +
            ", gbr), r0";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC500u) {
        instruction.kind =
            InstructionKind::MovWordLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text =
            "mov.w @(" +
            std::to_string(instruction.displacement) +
            ", gbr), r0";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC600u) {
        instruction.kind =
            InstructionKind::MovLongLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text =
            "mov.l @(" +
            std::to_string(instruction.displacement) +
            ", gbr), r0";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0x9000u) {
        instruction.kind =
            InstructionKind::MovWordLoadPcRelative;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text =
            "mov.w @(" +
            std::to_string(instruction.displacement) +
            ", pc), " +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xD000u) {
        instruction.kind =
            InstructionKind::MovLongLoadPcRelative;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text =
            "mov.l @(" +
            std::to_string(instruction.displacement) +
            ", pc), " +
            register_name(instruction.destination_register);
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0xC700u) {
        instruction.kind =
            InstructionKind::MoveAddressPcRelative;
        instruction.destination_register = 0u;
        instruction.displacement =
            static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text =
            "mova @(" +
            std::to_string(instruction.displacement) +
            ", pc), r0";
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
