#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction_metadata.hpp"

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

    output << ".word 0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
           << opcode;

    return output.str();
}

bool matches_metadata(const std::uint16_t opcode, const InstructionKind kind) {
    const auto* metadata = metadata_for_kind(kind);
    return metadata != nullptr && metadata->matches(opcode);
}

void decode_memory_registers(DecodedInstruction& instruction, const std::uint16_t opcode) {
    instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

    instruction.source_register = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
}

bool decode_special_register_transfer(DecodedInstruction& instruction, const std::uint16_t opcode) {
    for (const auto& encoding : special_register_encoding_metadata()) {
        if (!encoding.matches(opcode)) {
            continue;
        }

        const auto general_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.kind = encoding.kind;
        instruction.special_register = encoding.special_register;
        instruction.is_privileged = encoding.is_privileged;

        if (encoding.register_is_source) {
            instruction.source_register = general_register;
        } else {
            instruction.destination_register = general_register;
        }

        if (encoding.memory_form && encoding.register_is_source) {
            instruction.text = std::string(encoding.mnemonic) + " @" +
                               register_name(general_register) + "+, " +
                               std::string(encoding.special_register_name);
        } else if (encoding.memory_form) {
            instruction.text = std::string(encoding.mnemonic) + " " +
                               std::string(encoding.special_register_name) + ", @-" +
                               register_name(general_register);
        } else if (encoding.register_is_source) {
            instruction.text = std::string(encoding.mnemonic) + " " +
                               register_name(general_register) + ", " +
                               std::string(encoding.special_register_name);
        } else {
            instruction.text = std::string(encoding.mnemonic) + " " +
                               std::string(encoding.special_register_name) + ", " +
                               register_name(general_register);
        }
        return true;
    }
    return false;
}

bool decode_fpu_instruction(DecodedInstruction& instruction, const std::uint16_t opcode) {
    constexpr std::array kinds = {InstructionKind::FmovRegister,
                                  InstructionKind::FmovLoad,
                                  InstructionKind::FmovLoadPostIncrement,
                                  InstructionKind::FmovLoadR0Indexed,
                                  InstructionKind::FmovStore,
                                  InstructionKind::FmovStorePreDecrement,
                                  InstructionKind::FmovStoreR0Indexed,
                                  InstructionKind::Fldi0,
                                  InstructionKind::Fldi1,
                                  InstructionKind::Flds,
                                  InstructionKind::Fsts,
                                  InstructionKind::Fabs,
                                  InstructionKind::Fadd,
                                  InstructionKind::FcmpEqual,
                                  InstructionKind::FcmpGreater,
                                  InstructionKind::Fdiv,
                                  InstructionKind::FloatFromFpul,
                                  InstructionKind::Fmac,
                                  InstructionKind::Fmul,
                                  InstructionKind::Fneg,
                                  InstructionKind::Fsqrt,
                                  InstructionKind::Fsrra,
                                  InstructionKind::Fsca,
                                  InstructionKind::Fipr,
                                  InstructionKind::Ftrv,
                                  InstructionKind::Fsub,
                                  InstructionKind::Ftrc,
                                  InstructionKind::FcnvDoubleToSingle,
                                  InstructionKind::FcnvSingleToDouble,
                                  InstructionKind::Frchg,
                                  InstructionKind::Fschg};
    for (const auto kind : kinds) {
        const auto* metadata = metadata_for_kind(kind);
        if (metadata == nullptr || !metadata->matches(opcode)) {
            continue;
        }
        instruction.kind = kind;
        const auto n = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        const auto m = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.destination_register = n;
        instruction.source_register = m;
        switch (kind) {
        case InstructionKind::Flds:
        case InstructionKind::Ftrc:
        case InstructionKind::FcnvDoubleToSingle:
            instruction.destination_register = 0u;
            instruction.source_register = n;
            break;
        case InstructionKind::Fldi0:
        case InstructionKind::Fldi1:
        case InstructionKind::Fsts:
        case InstructionKind::Fabs:
        case InstructionKind::FloatFromFpul:
        case InstructionKind::Fneg:
        case InstructionKind::Fsqrt:
        case InstructionKind::FcnvSingleToDouble:
            instruction.source_register = 0u;
            break;
        case InstructionKind::Fsca:
            instruction.destination_register = static_cast<std::uint8_t>(n & 0x0Eu);
            instruction.source_register = 0u;
            break;
        case InstructionKind::Fipr:
            instruction.destination_register = static_cast<std::uint8_t>(((n >> 2u) & 3u) * 4u);
            instruction.source_register = static_cast<std::uint8_t>((n & 3u) * 4u);
            break;
        case InstructionKind::Ftrv:
            instruction.destination_register = static_cast<std::uint8_t>(n & 0x0Cu);
            instruction.source_register = 0u;
            break;
        case InstructionKind::Frchg:
        case InstructionKind::Fschg:
            instruction.destination_register = 0u;
            instruction.source_register = 0u;
            break;
        default:
            break;
        }
        const auto fr = [](const std::uint8_t index) { return "fr" + std::to_string(index); };
        switch (kind) {
        case InstructionKind::FmovRegister:
            instruction.text = "fmov " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::FmovLoad:
            instruction.text = "fmov.s @r" + std::to_string(m) + ", " + fr(n);
            break;
        case InstructionKind::FmovLoadPostIncrement:
            instruction.text = "fmov.s @r" + std::to_string(m) + "+, " + fr(n);
            break;
        case InstructionKind::FmovLoadR0Indexed:
            instruction.text = "fmov.s @(r0,r" + std::to_string(m) + "), " + fr(n);
            break;
        case InstructionKind::FmovStore:
            instruction.text = "fmov.s " + fr(m) + ", @r" + std::to_string(n);
            break;
        case InstructionKind::FmovStorePreDecrement:
            instruction.text = "fmov.s " + fr(m) + ", @-r" + std::to_string(n);
            break;
        case InstructionKind::FmovStoreR0Indexed:
            instruction.text = "fmov.s " + fr(m) + ", @(r0,r" + std::to_string(n) + ")";
            break;
        case InstructionKind::Fldi0:
            instruction.text = "fldi0 " + fr(n);
            break;
        case InstructionKind::Fldi1:
            instruction.text = "fldi1 " + fr(n);
            break;
        case InstructionKind::Flds:
            instruction.text = "flds " + fr(n) + ", fpul";
            break;
        case InstructionKind::Fsts:
            instruction.text = "fsts fpul, " + fr(n);
            break;
        case InstructionKind::Fabs:
            instruction.text = "fabs " + fr(n);
            break;
        case InstructionKind::Fadd:
            instruction.text = "fadd " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::FcmpEqual:
            instruction.text = "fcmp/eq " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::FcmpGreater:
            instruction.text = "fcmp/gt " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::Fdiv:
            instruction.text = "fdiv " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::FloatFromFpul:
            instruction.text = "float fpul, " + fr(n);
            break;
        case InstructionKind::Fmac:
            instruction.text = "fmac fr0, " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::Fmul:
            instruction.text = "fmul " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::Fneg:
            instruction.text = "fneg " + fr(n);
            break;
        case InstructionKind::Fsqrt:
            instruction.text = "fsqrt " + fr(n);
            break;
        case InstructionKind::Fsrra:
            instruction.text = "fsrra " + fr(n);
            break;
        case InstructionKind::Fsca:
            instruction.text = "fsca fpul, dr" + std::to_string((n & 0x0Eu) >> 1u);
            break;
        case InstructionKind::Fipr:
            instruction.text = "fipr fv" + std::to_string((n & 3u) * 4u) + ", fv" +
                               std::to_string(((n >> 2u) & 3u) * 4u);
            break;
        case InstructionKind::Ftrv:
            instruction.text = "ftrv xmtrx, fv" + std::to_string(n & 0x0Cu);
            break;
        case InstructionKind::Fsub:
            instruction.text = "fsub " + fr(m) + ", " + fr(n);
            break;
        case InstructionKind::Ftrc:
            instruction.text = "ftrc " + fr(n) + ", fpul";
            break;
        case InstructionKind::FcnvDoubleToSingle:
            instruction.text = "fcnvds dr" + std::to_string(n >> 1u) + ", fpul";
            break;
        case InstructionKind::FcnvSingleToDouble:
            instruction.text = "fcnvsd fpul, dr" + std::to_string(n >> 1u);
            break;
        case InstructionKind::Frchg:
            instruction.text = "frchg";
            break;
        case InstructionKind::Fschg:
            instruction.text = "fschg";
            break;
        default:
            break;
        }
        return true;
    }
    return false;
}

} // namespace

DecodedInstruction decode(const std::uint16_t opcode) {
    DecodedInstruction instruction;
    instruction.opcode = opcode;

    if (decode_special_register_transfer(instruction, opcode)) {
        return instruction;
    }

    if (decode_fpu_instruction(instruction, opcode)) {
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Nop)) {
        instruction.kind = InstructionKind::Nop;
        instruction.text = "nop";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ClearMac)) {
        instruction.kind = InstructionKind::ClearMac;
        instruction.text = "clrmac";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Rts)) {
        instruction.kind = InstructionKind::Rts;
        instruction.control_flow = ControlFlowKind::Return;
        instruction.has_delay_slot = true;
        instruction.text = "rts";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ReturnFromException)) {
        instruction.kind = InstructionKind::ReturnFromException;
        instruction.control_flow = ControlFlowKind::ExceptionReturn;
        instruction.has_delay_slot = true;
        instruction.is_privileged = true;
        instruction.text = "rte";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Sleep)) {
        instruction.kind = InstructionKind::Sleep;
        instruction.control_flow = ControlFlowKind::Halt;
        instruction.is_privileged = true;
        instruction.text = "sleep";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::LoadTlb)) {
        instruction.kind = InstructionKind::LoadTlb;
        instruction.is_privileged = true;
        instruction.text = "ldtlb";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Prefetch)) {
        instruction.kind = InstructionKind::Prefetch;
        instruction.source_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.text = "pref @" + register_name(instruction.source_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Ocbi) ||
        matches_metadata(opcode, InstructionKind::Ocbp) ||
        matches_metadata(opcode, InstructionKind::Ocbwb)) {
        instruction.kind = matches_metadata(opcode, InstructionKind::Ocbi) ? InstructionKind::Ocbi
                           : matches_metadata(opcode, InstructionKind::Ocbp)
                               ? InstructionKind::Ocbp
                               : InstructionKind::Ocbwb;
        instruction.source_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        const auto mnemonic = instruction.kind == InstructionKind::Ocbi   ? "ocbi @"
                              : instruction.kind == InstructionKind::Ocbp ? "ocbp @"
                                                                          : "ocbwb @";
        instruction.text = std::string(mnemonic) + register_name(instruction.source_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovcaLong)) {
        instruction.kind = InstructionKind::MovcaLong;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.source_register = 0u;
        instruction.text = "movca.l r0,@" + register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::TestAndSetByte)) {
        instruction.kind = InstructionKind::TestAndSetByte;
        instruction.source_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.text = "tas.b @" + register_name(instruction.source_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::TrapAlways)) {
        instruction.kind = InstructionKind::TrapAlways;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);
        instruction.control_flow = ControlFlowKind::Trap;
        instruction.text = "trapa #" + std::to_string(instruction.immediate);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Bra)) {
        instruction.kind = InstructionKind::Bra;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::UnconditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bra";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Bsr)) {
        instruction.kind = InstructionKind::Bsr;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::Call;
        instruction.has_delay_slot = true;
        instruction.text = "bsr";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Braf)) {
        instruction.kind = InstructionKind::Braf;
        instruction.branch_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectBranch;
        instruction.has_delay_slot = true;
        instruction.text = "braf " + register_name(instruction.branch_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Bsrf)) {
        instruction.kind = InstructionKind::Bsrf;
        instruction.branch_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectCall;
        instruction.has_delay_slot = true;
        instruction.text = "bsrf " + register_name(instruction.branch_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Bt)) {
        instruction.kind = InstructionKind::Bt;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bt";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Bf)) {
        instruction.kind = InstructionKind::Bf;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bf";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::BtS)) {
        instruction.kind = InstructionKind::BtS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bt/s";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::BfS)) {
        instruction.kind = InstructionKind::BfS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bf/s";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Jmp)) {
        instruction.kind = InstructionKind::Jmp;
        instruction.branch_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectBranch;
        instruction.has_delay_slot = true;
        instruction.text = "jmp @" + register_name(instruction.branch_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::Jsr)) {
        instruction.kind = InstructionKind::Jsr;
        instruction.branch_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectCall;
        instruction.has_delay_slot = true;
        instruction.text = "jsr @" + register_name(instruction.branch_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SubRegister)) {
        instruction.kind = InstructionKind::SubRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "sub " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::NegateRegister)) {
        instruction.kind = InstructionKind::NegateRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "neg " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::NotRegister)) {
        instruction.kind = InstructionKind::NotRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "not " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::AddWithCarry)) {
        instruction.kind = InstructionKind::AddWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text = "addc " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::AddWithOverflow)) {
        instruction.kind = InstructionKind::AddWithOverflow;
        decode_memory_registers(instruction, opcode);

        instruction.text = "addv " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SubWithCarry)) {
        instruction.kind = InstructionKind::SubWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text = "subc " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SubWithOverflow)) {
        instruction.kind = InstructionKind::SubWithOverflow;
        decode_memory_registers(instruction, opcode);

        instruction.text = "subv " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::NegateWithCarry)) {
        instruction.kind = InstructionKind::NegateWithCarry;
        decode_memory_registers(instruction, opcode);

        instruction.text = "negc " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ExtendUnsignedByte)) {
        instruction.kind = InstructionKind::ExtendUnsignedByte;
        decode_memory_registers(instruction, opcode);

        instruction.text = "extu.b " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ExtendUnsignedWord)) {
        instruction.kind = InstructionKind::ExtendUnsignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text = "extu.w " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ExtendSignedByte)) {
        instruction.kind = InstructionKind::ExtendSignedByte;
        decode_memory_registers(instruction, opcode);

        instruction.text = "exts.b " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ExtendSignedWord)) {
        instruction.kind = InstructionKind::ExtendSignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text = "exts.w " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SwapBytes)) {
        instruction.kind = InstructionKind::SwapBytes;
        decode_memory_registers(instruction, opcode);

        instruction.text = "swap.b " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SwapWords)) {
        instruction.kind = InstructionKind::SwapWords;
        decode_memory_registers(instruction, opcode);

        instruction.text = "swap.w " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ExtractMiddle)) {
        instruction.kind = InstructionKind::ExtractMiddle;
        decode_memory_registers(instruction, opcode);

        instruction.text = "xtrct " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::DecrementAndTest)) {
        instruction.kind = InstructionKind::DecrementAndTest;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "dt " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MoveT)) {
        instruction.kind = InstructionKind::MoveT;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "movt " + register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ShiftLogicalLeftOne)) {
        instruction.kind = InstructionKind::ShiftLogicalLeftOne;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shll " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalRightOne)) {
        instruction.kind = InstructionKind::ShiftLogicalRightOne;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shlr " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftArithmeticLeftOne)) {
        instruction.kind = InstructionKind::ShiftArithmeticLeftOne;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shal " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftArithmeticRightOne)) {
        instruction.kind = InstructionKind::ShiftArithmeticRightOne;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shar " + register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ShiftLogicalLeftTwo)) {
        instruction.kind = InstructionKind::ShiftLogicalLeftTwo;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shll2 " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalLeftEight)) {
        instruction.kind = InstructionKind::ShiftLogicalLeftEight;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shll8 " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalLeftSixteen)) {
        instruction.kind = InstructionKind::ShiftLogicalLeftSixteen;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shll16 " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalRightTwo)) {
        instruction.kind = InstructionKind::ShiftLogicalRightTwo;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shlr2 " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalRightEight)) {
        instruction.kind = InstructionKind::ShiftLogicalRightEight;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shlr8 " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalRightSixteen)) {
        instruction.kind = InstructionKind::ShiftLogicalRightSixteen;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "shlr16 " + register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::RotateLeft)) {
        instruction.kind = InstructionKind::RotateLeft;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "rotl " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::RotateRight)) {
        instruction.kind = InstructionKind::RotateRight;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "rotr " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::RotateLeftThroughT)) {
        instruction.kind = InstructionKind::RotateLeftThroughT;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "rotcl " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::RotateRightThroughT)) {
        instruction.kind = InstructionKind::RotateRightThroughT;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "rotcr " + register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ShiftArithmeticDynamic)) {
        instruction.kind = InstructionKind::ShiftArithmeticDynamic;
        decode_memory_registers(instruction, opcode);

        instruction.text = "shad " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ShiftLogicalDynamic)) {
        instruction.kind = InstructionKind::ShiftLogicalDynamic;
        decode_memory_registers(instruction, opcode);

        instruction.text = "shld " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::DivideInitializeUnsigned)) {
        instruction.kind = InstructionKind::DivideInitializeUnsigned;
        instruction.text = "div0u";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::DivideInitializeSigned)) {
        instruction.kind = InstructionKind::DivideInitializeSigned;
        decode_memory_registers(instruction, opcode);

        instruction.text = "div0s " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::DivideStep)) {
        instruction.kind = InstructionKind::DivideStep;
        decode_memory_registers(instruction, opcode);

        instruction.text = "div1 " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::MultiplyAccumulateWord)) {
        instruction.kind = InstructionKind::MultiplyAccumulateWord;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mac.w @" + register_name(instruction.source_register) + "+, @" +
                           register_name(instruction.destination_register) + "+";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MultiplyAccumulateLong)) {
        instruction.kind = InstructionKind::MultiplyAccumulateLong;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mac.l @" + register_name(instruction.source_register) + "+, @" +
                           register_name(instruction.destination_register) + "+";

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::DoubleMultiplySignedLong)) {
        instruction.kind = InstructionKind::DoubleMultiplySignedLong;
        decode_memory_registers(instruction, opcode);

        instruction.text = "dmuls.l " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::DoubleMultiplyUnsignedLong)) {
        instruction.kind = InstructionKind::DoubleMultiplyUnsignedLong;
        decode_memory_registers(instruction, opcode);

        instruction.text = "dmulu.l " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::MultiplyLong)) {
        instruction.kind = InstructionKind::MultiplyLong;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mul.l " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MultiplySignedWord)) {
        instruction.kind = InstructionKind::MultiplySignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text = "muls.w " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MultiplyUnsignedWord)) {
        instruction.kind = InstructionKind::MultiplyUnsignedWord;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mulu.w " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::AndRegister)) {
        instruction.kind = InstructionKind::AndRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "and " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::XorRegister)) {
        instruction.kind = InstructionKind::XorRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "xor " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::OrRegister)) {
        instruction.kind = InstructionKind::OrRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "or " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::AndImmediate)) {
        instruction.kind = InstructionKind::AndImmediate;
        instruction.destination_register = 0;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text = "and #" + std::to_string(instruction.immediate) + ", r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::XorImmediate)) {
        instruction.kind = InstructionKind::XorImmediate;
        instruction.destination_register = 0;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text = "xor #" + std::to_string(instruction.immediate) + ", r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::OrImmediate)) {
        instruction.kind = InstructionKind::OrImmediate;
        instruction.destination_register = 0;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text = "or #" + std::to_string(instruction.immediate) + ", r0";

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ClearS)) {
        instruction.kind = InstructionKind::ClearS;
        instruction.text = "clrs";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SetS)) {
        instruction.kind = InstructionKind::SetS;
        instruction.text = "sets";
        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::ClearT)) {
        instruction.kind = InstructionKind::ClearT;
        instruction.text = "clrt";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::SetT)) {
        instruction.kind = InstructionKind::SetT;
        instruction.text = "sett";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareEqualImmediate)) {
        instruction.kind = InstructionKind::CompareEqualImmediate;
        instruction.destination_register = 0;
        instruction.immediate = sign_extend_8(opcode);

        instruction.text = "cmp/eq #" + std::to_string(instruction.immediate) + ", r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareEqualRegister)) {
        instruction.kind = InstructionKind::CompareEqualRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/eq " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareHigherOrSame)) {
        instruction.kind = InstructionKind::CompareHigherOrSame;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/hs " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareGreaterOrEqual)) {
        instruction.kind = InstructionKind::CompareGreaterOrEqual;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/ge " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareHigher)) {
        instruction.kind = InstructionKind::CompareHigher;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/hi " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareGreaterThan)) {
        instruction.kind = InstructionKind::CompareGreaterThan;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/gt " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ComparePositiveOrZero)) {
        instruction.kind = InstructionKind::ComparePositiveOrZero;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "cmp/pz " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::ComparePositive)) {
        instruction.kind = InstructionKind::ComparePositive;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);

        instruction.text = "cmp/pl " + register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::CompareString)) {
        instruction.kind = InstructionKind::CompareString;
        decode_memory_registers(instruction, opcode);

        instruction.text = "cmp/str " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::TestImmediate)) {
        instruction.kind = InstructionKind::TestImmediate;
        instruction.destination_register = 0;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);

        instruction.text = "tst #" + std::to_string(instruction.immediate) + ", r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::TestRegister)) {
        instruction.kind = InstructionKind::TestRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "tst " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }
    for (const auto kind : {InstructionKind::TestByteImmediate,
                            InstructionKind::AndByteImmediate,
                            InstructionKind::XorByteImmediate,
                            InstructionKind::OrByteImmediate}) {
        if (!matches_metadata(opcode, kind)) continue;
        instruction.kind = kind;
        instruction.immediate = static_cast<std::int32_t>(opcode & 0x00FFu);
        const auto mnemonic = kind == InstructionKind::TestByteImmediate  ? "tst.b"
                              : kind == InstructionKind::AndByteImmediate ? "and.b"
                              : kind == InstructionKind::XorByteImmediate ? "xor.b"
                                                                          : "or.b";
        instruction.text =
            std::string(mnemonic) + " #" + std::to_string(instruction.immediate) + ", @(r0,gbr)";
        return instruction;
    }
    if (matches_metadata(opcode, InstructionKind::MovByteStorePreDecrement)) {
        instruction.kind = InstructionKind::MovByteStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.b " + register_name(instruction.source_register) + ", @-" +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordStorePreDecrement)) {
        instruction.kind = InstructionKind::MovWordStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.w " + register_name(instruction.source_register) + ", @-" +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongStorePreDecrement)) {
        instruction.kind = InstructionKind::MovLongStorePreDecrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.l " + register_name(instruction.source_register) + ", @-" +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteLoadPostIncrement)) {
        instruction.kind = InstructionKind::MovByteLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.b @" + register_name(instruction.source_register) + "+, " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoadPostIncrement)) {
        instruction.kind = InstructionKind::MovWordLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.w @" + register_name(instruction.source_register) + "+, " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoadPostIncrement)) {
        instruction.kind = InstructionKind::MovLongLoadPostIncrement;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.l @" + register_name(instruction.source_register) + "+, " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteStoreDisplacement)) {
        instruction.kind = InstructionKind::MovByteStoreDisplacement;
        instruction.source_register = 0u;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>(opcode & 0x000Fu);

        instruction.text = "mov.b r0, @(" + std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordStoreDisplacement)) {
        instruction.kind = InstructionKind::MovWordStoreDisplacement;
        instruction.source_register = 0u;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x000Fu) * 2u);

        instruction.text = "mov.w r0, @(" + std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongStoreDisplacement)) {
        instruction.kind = InstructionKind::MovLongStoreDisplacement;
        decode_memory_registers(instruction, opcode);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x000Fu) * 4u);

        instruction.text = "mov.l " + register_name(instruction.source_register) + ", @(" +
                           std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteLoadDisplacement)) {
        instruction.kind = InstructionKind::MovByteLoadDisplacement;
        instruction.destination_register = 0u;
        instruction.source_register = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>(opcode & 0x000Fu);

        instruction.text = "mov.b @(" + std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.source_register) + "), r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoadDisplacement)) {
        instruction.kind = InstructionKind::MovWordLoadDisplacement;
        instruction.destination_register = 0u;
        instruction.source_register = static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x000Fu) * 2u);

        instruction.text = "mov.w @(" + std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.source_register) + "), r0";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoadDisplacement)) {
        instruction.kind = InstructionKind::MovLongLoadDisplacement;
        decode_memory_registers(instruction, opcode);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x000Fu) * 4u);

        instruction.text = "mov.l @(" + std::to_string(instruction.displacement) + ", " +
                           register_name(instruction.source_register) + "), " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteStoreR0Indexed)) {
        instruction.kind = InstructionKind::MovByteStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.b " + register_name(instruction.source_register) + ", @(r0, " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordStoreR0Indexed)) {
        instruction.kind = InstructionKind::MovWordStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.w " + register_name(instruction.source_register) + ", @(r0, " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongStoreR0Indexed)) {
        instruction.kind = InstructionKind::MovLongStoreR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.l " + register_name(instruction.source_register) + ", @(r0, " +
                           register_name(instruction.destination_register) + ")";

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteLoadR0Indexed)) {
        instruction.kind = InstructionKind::MovByteLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.b @(r0, " + register_name(instruction.source_register) + "), " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoadR0Indexed)) {
        instruction.kind = InstructionKind::MovWordLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.w @(r0, " + register_name(instruction.source_register) + "), " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoadR0Indexed)) {
        instruction.kind = InstructionKind::MovLongLoadR0Indexed;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov.l @(r0, " + register_name(instruction.source_register) + "), " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteStoreGbrDisplacement)) {
        instruction.kind = InstructionKind::MovByteStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement = static_cast<std::int32_t>(opcode & 0x00FFu);
        instruction.text = "mov.b r0, @(" + std::to_string(instruction.displacement) + ", gbr)";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordStoreGbrDisplacement)) {
        instruction.kind = InstructionKind::MovWordStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text = "mov.w r0, @(" + std::to_string(instruction.displacement) + ", gbr)";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongStoreGbrDisplacement)) {
        instruction.kind = InstructionKind::MovLongStoreGbrDisplacement;
        instruction.source_register = 0u;
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text = "mov.l r0, @(" + std::to_string(instruction.displacement) + ", gbr)";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteLoadGbrDisplacement)) {
        instruction.kind = InstructionKind::MovByteLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement = static_cast<std::int32_t>(opcode & 0x00FFu);
        instruction.text = "mov.b @(" + std::to_string(instruction.displacement) + ", gbr), r0";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoadGbrDisplacement)) {
        instruction.kind = InstructionKind::MovWordLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text = "mov.w @(" + std::to_string(instruction.displacement) + ", gbr), r0";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoadGbrDisplacement)) {
        instruction.kind = InstructionKind::MovLongLoadGbrDisplacement;
        instruction.destination_register = 0u;
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text = "mov.l @(" + std::to_string(instruction.displacement) + ", gbr), r0";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoadPcRelative)) {
        instruction.kind = InstructionKind::MovWordLoadPcRelative;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 2u);
        instruction.text = "mov.w @(" + std::to_string(instruction.displacement) + ", pc), " +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoadPcRelative)) {
        instruction.kind = InstructionKind::MovLongLoadPcRelative;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text = "mov.l @(" + std::to_string(instruction.displacement) + ", pc), " +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MoveAddressPcRelative)) {
        instruction.kind = InstructionKind::MoveAddressPcRelative;
        instruction.destination_register = 0u;
        instruction.displacement = static_cast<std::int32_t>((opcode & 0x00FFu) * 4u);
        instruction.text = "mova @(" + std::to_string(instruction.displacement) + ", pc), r0";
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteStore)) {
        instruction.kind = InstructionKind::MovByteStore;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.b " + register_name(instruction.source_register) + ", @" +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordStore)) {
        instruction.kind = InstructionKind::MovWordStore;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.w " + register_name(instruction.source_register) + ", @" +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongStore)) {
        instruction.kind = InstructionKind::MovLongStore;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.l " + register_name(instruction.source_register) + ", @" +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovByteLoad)) {
        instruction.kind = InstructionKind::MovByteLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.b @" + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovWordLoad)) {
        instruction.kind = InstructionKind::MovWordLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.w @" + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovLongLoad)) {
        instruction.kind = InstructionKind::MovLongLoad;
        decode_memory_registers(instruction, opcode);
        instruction.text = "mov.l @" + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);
        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovImmediate)) {
        instruction.kind = InstructionKind::MovImmediate;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text = "mov #" + std::to_string(instruction.immediate) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::AddImmediate)) {
        instruction.kind = InstructionKind::AddImmediate;
        instruction.destination_register = static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text = "add #" + std::to_string(instruction.immediate) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::MovRegister)) {
        instruction.kind = InstructionKind::MovRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "mov " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    if (matches_metadata(opcode, InstructionKind::AddRegister)) {
        instruction.kind = InstructionKind::AddRegister;
        decode_memory_registers(instruction, opcode);

        instruction.text = "add " + register_name(instruction.source_register) + ", " +
                           register_name(instruction.destination_register);

        return instruction;
    }

    instruction.text = unknown_instruction_text(opcode);
    return instruction;
}

std::optional<std::uint32_t>
calculate_direct_branch_target(const DecodedInstruction& instruction,
                               const std::uint32_t instruction_address) {
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

    const auto expanded_target = static_cast<std::int64_t>(instruction_address) + 4 +
                                 static_cast<std::int64_t>(instruction.displacement);

    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(expanded_target) & 0xFFFFFFFFull);
}

} // namespace katana::sh4
