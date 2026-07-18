#include "katana/ir/lower.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::ir {
namespace {

SpecialRegister lower_special_register(const katana::sh4::SpecialRegister source) {
    using Source = katana::sh4::SpecialRegister;
    switch (source) {
    case Source::None:
        return SpecialRegister::None;
    case Source::Mach:
        return SpecialRegister::Mach;
    case Source::Macl:
        return SpecialRegister::Macl;
    case Source::Pr:
        return SpecialRegister::Pr;
    case Source::Fpul:
        return SpecialRegister::Fpul;
    case Source::Fpscr:
        return SpecialRegister::Fpscr;
    case Source::Sr:
        return SpecialRegister::Sr;
    case Source::Gbr:
        return SpecialRegister::Gbr;
    case Source::Vbr:
        return SpecialRegister::Vbr;
    case Source::Ssr:
        return SpecialRegister::Ssr;
    case Source::Spc:
        return SpecialRegister::Spc;
    case Source::Sgr:
        return SpecialRegister::Sgr;
    case Source::Dbr:
        return SpecialRegister::Dbr;
    case Source::Bank0:
        return SpecialRegister::Bank0;
    case Source::Bank1:
        return SpecialRegister::Bank1;
    case Source::Bank2:
        return SpecialRegister::Bank2;
    case Source::Bank3:
        return SpecialRegister::Bank3;
    case Source::Bank4:
        return SpecialRegister::Bank4;
    case Source::Bank5:
        return SpecialRegister::Bank5;
    case Source::Bank6:
        return SpecialRegister::Bank6;
    case Source::Bank7:
        return SpecialRegister::Bank7;
    }
    return SpecialRegister::None;
}

Operation lower_operation(const katana::sh4::InstructionKind kind) {
    using Source = katana::sh4::InstructionKind;

    switch (kind) {
    case Source::Nop:
        return Operation::Nop;

    case Source::MovImmediate:
        return Operation::MovImmediate;

    case Source::AddImmediate:
        return Operation::AddImmediate;

    case Source::MovRegister:
        return Operation::MovRegister;

    case Source::AddRegister:
        return Operation::AddRegister;

    case Source::SubRegister:
        return Operation::SubRegister;

    case Source::NegateRegister:
        return Operation::NegateRegister;

    case Source::NotRegister:
        return Operation::NotRegister;
    case Source::AddWithCarry:
        return Operation::AddWithCarry;

    case Source::AddWithOverflow:
        return Operation::AddWithOverflow;

    case Source::SubWithCarry:
        return Operation::SubWithCarry;

    case Source::SubWithOverflow:
        return Operation::SubWithOverflow;

    case Source::NegateWithCarry:
        return Operation::NegateWithCarry;
    case Source::ExtendUnsignedByte:
        return Operation::ExtendUnsignedByte;

    case Source::ExtendUnsignedWord:
        return Operation::ExtendUnsignedWord;

    case Source::ExtendSignedByte:
        return Operation::ExtendSignedByte;

    case Source::ExtendSignedWord:
        return Operation::ExtendSignedWord;

    case Source::SwapBytes:
        return Operation::SwapBytes;

    case Source::SwapWords:
        return Operation::SwapWords;

    case Source::ExtractMiddle:
        return Operation::ExtractMiddle;
    case Source::DecrementAndTest:
        return Operation::DecrementAndTest;

    case Source::MoveT:
        return Operation::MoveT;
    case Source::ShiftLogicalLeftOne:
        return Operation::ShiftLogicalLeftOne;

    case Source::ShiftLogicalRightOne:
        return Operation::ShiftLogicalRightOne;

    case Source::ShiftArithmeticLeftOne:
        return Operation::ShiftArithmeticLeftOne;

    case Source::ShiftArithmeticRightOne:
        return Operation::ShiftArithmeticRightOne;
    case Source::ShiftLogicalLeftTwo:
        return Operation::ShiftLogicalLeftTwo;

    case Source::ShiftLogicalLeftEight:
        return Operation::ShiftLogicalLeftEight;

    case Source::ShiftLogicalLeftSixteen:
        return Operation::ShiftLogicalLeftSixteen;

    case Source::ShiftLogicalRightTwo:
        return Operation::ShiftLogicalRightTwo;

    case Source::ShiftLogicalRightEight:
        return Operation::ShiftLogicalRightEight;

    case Source::ShiftLogicalRightSixteen:
        return Operation::ShiftLogicalRightSixteen;
    case Source::RotateLeft:
        return Operation::RotateLeft;

    case Source::RotateRight:
        return Operation::RotateRight;

    case Source::RotateLeftThroughT:
        return Operation::RotateLeftThroughT;

    case Source::RotateRightThroughT:
        return Operation::RotateRightThroughT;
    case Source::ShiftArithmeticDynamic:
        return Operation::ShiftArithmeticDynamic;

    case Source::ShiftLogicalDynamic:
        return Operation::ShiftLogicalDynamic;
    case Source::MultiplyLong:
        return Operation::MultiplyLong;

    case Source::MultiplySignedWord:
        return Operation::MultiplySignedWord;

    case Source::MultiplyUnsignedWord:
        return Operation::MultiplyUnsignedWord;
    case Source::DoubleMultiplySignedLong:
        return Operation::DoubleMultiplySignedLong;

    case Source::DoubleMultiplyUnsignedLong:
        return Operation::DoubleMultiplyUnsignedLong;
    case Source::MultiplyAccumulateWord:
        return Operation::MultiplyAccumulateWord;

    case Source::MultiplyAccumulateLong:
        return Operation::MultiplyAccumulateLong;
    case Source::DivideInitializeUnsigned:
        return Operation::DivideInitializeUnsigned;

    case Source::DivideInitializeSigned:
        return Operation::DivideInitializeSigned;

    case Source::DivideStep:
        return Operation::DivideStep;
    case Source::ClearMac:
        return Operation::ClearMac;
    case Source::AndRegister:
        return Operation::AndRegister;

    case Source::OrRegister:
        return Operation::OrRegister;

    case Source::XorRegister:
        return Operation::XorRegister;

    case Source::AndImmediate:
        return Operation::AndImmediate;

    case Source::OrImmediate:
        return Operation::OrImmediate;

    case Source::XorImmediate:
        return Operation::XorImmediate;
    case Source::ClearS:
        return Operation::ClearS;

    case Source::SetS:
        return Operation::SetS;
    case Source::ClearT:
        return Operation::ClearT;

    case Source::SetT:
        return Operation::SetT;

    case Source::CompareEqualImmediate:
        return Operation::CompareEqualImmediate;

    case Source::CompareEqualRegister:
        return Operation::CompareEqualRegister;

    case Source::CompareHigherOrSame:
        return Operation::CompareHigherOrSame;

    case Source::CompareGreaterOrEqual:
        return Operation::CompareGreaterOrEqual;

    case Source::CompareHigher:
        return Operation::CompareHigher;

    case Source::CompareGreaterThan:
        return Operation::CompareGreaterThan;

    case Source::ComparePositiveOrZero:
        return Operation::ComparePositiveOrZero;

    case Source::ComparePositive:
        return Operation::ComparePositive;

    case Source::CompareString:
        return Operation::CompareString;
    case Source::TestImmediate:
        return Operation::TestImmediate;

    case Source::TestRegister:
        return Operation::TestRegister;
    case Source::TestByteImmediate:
        return Operation::TestByteImmediate;
    case Source::AndByteImmediate:
        return Operation::AndByteImmediate;
    case Source::XorByteImmediate:
        return Operation::XorByteImmediate;
    case Source::OrByteImmediate:
        return Operation::OrByteImmediate;
    case Source::TestAndSetByte:
        return Operation::TestAndSetByte;
    case Source::MovByteLoad:
        return Operation::LoadByteSigned;

    case Source::MovWordLoad:
        return Operation::LoadWordSigned;

    case Source::MovLongLoad:
        return Operation::LoadLong;

    case Source::MovByteStore:
        return Operation::StoreByte;

    case Source::MovWordStore:
        return Operation::StoreWord;

    case Source::MovLongStore:
        return Operation::StoreLong;

    case Source::MovByteStorePreDecrement:
        return Operation::StoreBytePreDecrement;

    case Source::MovWordStorePreDecrement:
        return Operation::StoreWordPreDecrement;

    case Source::MovLongStorePreDecrement:
        return Operation::StoreLongPreDecrement;

    case Source::MovByteLoadPostIncrement:
        return Operation::LoadByteSignedPostIncrement;

    case Source::MovWordLoadPostIncrement:
        return Operation::LoadWordSignedPostIncrement;

    case Source::MovLongLoadPostIncrement:
        return Operation::LoadLongPostIncrement;

    case Source::MovByteStoreDisplacement:
        return Operation::StoreByteDisplacement;

    case Source::MovWordStoreDisplacement:
        return Operation::StoreWordDisplacement;

    case Source::MovLongStoreDisplacement:
        return Operation::StoreLongDisplacement;

    case Source::MovByteLoadDisplacement:
        return Operation::LoadByteSignedDisplacement;

    case Source::MovWordLoadDisplacement:
        return Operation::LoadWordSignedDisplacement;

    case Source::MovLongLoadDisplacement:
        return Operation::LoadLongDisplacement;

    case Source::MovByteStoreR0Indexed:
        return Operation::StoreByteR0Indexed;

    case Source::MovWordStoreR0Indexed:
        return Operation::StoreWordR0Indexed;

    case Source::MovLongStoreR0Indexed:
        return Operation::StoreLongR0Indexed;

    case Source::MovByteLoadR0Indexed:
        return Operation::LoadByteSignedR0Indexed;

    case Source::MovWordLoadR0Indexed:
        return Operation::LoadWordSignedR0Indexed;

    case Source::MovLongLoadR0Indexed:
        return Operation::LoadLongR0Indexed;

    case Source::MovByteStoreGbrDisplacement:
        return Operation::StoreByteGbrDisplacement;

    case Source::MovWordStoreGbrDisplacement:
        return Operation::StoreWordGbrDisplacement;

    case Source::MovLongStoreGbrDisplacement:
        return Operation::StoreLongGbrDisplacement;

    case Source::MovByteLoadGbrDisplacement:
        return Operation::LoadByteSignedGbrDisplacement;

    case Source::MovWordLoadGbrDisplacement:
        return Operation::LoadWordSignedGbrDisplacement;

    case Source::MovLongLoadGbrDisplacement:
        return Operation::LoadLongGbrDisplacement;

    case Source::MovWordLoadPcRelative:
        return Operation::LoadWordSignedPcRelative;

    case Source::MovLongLoadPcRelative:
        return Operation::LoadLongPcRelative;

    case Source::MoveAddressPcRelative:
        return Operation::MoveAddressPcRelative;

    case Source::StoreSpecialRegister:
        return Operation::StoreSpecialRegister;

    case Source::StoreSpecialRegisterPreDecrement:
        return Operation::StoreSpecialRegisterPreDecrement;

    case Source::LoadSpecialRegister:
        return Operation::LoadSpecialRegister;

    case Source::LoadSpecialRegisterPostIncrement:
        return Operation::LoadSpecialRegisterPostIncrement;

    case Source::TrapAlways:
        return Operation::TrapAlways;

    case Source::ReturnFromException:
        return Operation::ReturnFromException;

    case Source::Sleep:
        return Operation::Sleep;
    case Source::Prefetch:
        return Operation::Prefetch;

    case Source::FmovRegister:
        return Operation::FmovRegister;
    case Source::FmovLoad:
        return Operation::FmovLoad;
    case Source::FmovLoadPostIncrement:
        return Operation::FmovLoadPostIncrement;
    case Source::FmovLoadR0Indexed:
        return Operation::FmovLoadR0Indexed;
    case Source::FmovStore:
        return Operation::FmovStore;
    case Source::FmovStorePreDecrement:
        return Operation::FmovStorePreDecrement;
    case Source::FmovStoreR0Indexed:
        return Operation::FmovStoreR0Indexed;
    case Source::Fldi0:
        return Operation::Fldi0;
    case Source::Fldi1:
        return Operation::Fldi1;
    case Source::Flds:
        return Operation::Flds;
    case Source::Fsts:
        return Operation::Fsts;
    case Source::Fabs:
        return Operation::Fabs;
    case Source::Fadd:
        return Operation::Fadd;
    case Source::FcmpEqual:
        return Operation::FcmpEqual;
    case Source::FcmpGreater:
        return Operation::FcmpGreater;
    case Source::Fdiv:
        return Operation::Fdiv;
    case Source::FloatFromFpul:
        return Operation::FloatFromFpul;
    case Source::Fmac:
        return Operation::Fmac;
    case Source::Fmul:
        return Operation::Fmul;
    case Source::Fneg:
        return Operation::Fneg;
    case Source::Fsqrt:
        return Operation::Fsqrt;
    case Source::Fsrra:
        return Operation::Fsrra;
    case Source::Fsca:
        return Operation::Fsca;
    case Source::Fipr:
        return Operation::Fipr;
    case Source::Ftrv:
        return Operation::Ftrv;
    case Source::Fsub:
        return Operation::Fsub;
    case Source::Ftrc:
        return Operation::Ftrc;
    case Source::FcnvDoubleToSingle:
        return Operation::FcnvDoubleToSingle;
    case Source::FcnvSingleToDouble:
        return Operation::FcnvSingleToDouble;
    case Source::Frchg:
        return Operation::Frchg;
    case Source::Fschg:
        return Operation::Fschg;

    case Source::Bra:
        return Operation::Branch;

    case Source::Bsr:
        return Operation::Call;

    case Source::Braf:
        return Operation::JumpRegister;

    case Source::Bsrf:
        return Operation::CallRegister;

    case Source::Bt:
    case Source::BtS:
        return Operation::BranchIfTrue;

    case Source::Bf:
    case Source::BfS:
        return Operation::BranchIfFalse;

    case Source::Jmp:
        return Operation::JumpRegister;

    case Source::Jsr:
        return Operation::CallRegister;

    case Source::Rts:
        return Operation::Return;

    case Source::Unknown:
        return Operation::Unknown;
    }

    return Operation::Unknown;
}

Instruction lower_instruction(const katana::sh4::DisassemblyLine& source) {
    Instruction result;

    result.source_address = source.address;
    result.original_opcode = source.opcode;
    result.operation = lower_operation(source.instruction.kind);
    result.original_operation = result.operation;
    result.widths = operation_operand_widths(result.operation);
    result.special_register = lower_special_register(source.instruction.special_register);
    result.accumulator_effects =
        operation_accumulator_effects(result.operation, result.special_register);

    result.destination_register = source.instruction.destination_register;
    result.source_register = source.instruction.source_register;
    result.branch_register = source.instruction.branch_register;
    result.memory_effects = instruction_memory_effects(
        result.operation, result.destination_register, result.source_register);

    result.immediate = source.instruction.immediate;
    result.displacement = source.instruction.displacement;
    result.status_effects = instruction_status_effects(result.operation, result.special_register);

    const auto displacement = static_cast<std::uint32_t>(source.instruction.displacement);

    switch (result.operation) {
    case Operation::LoadWordSignedPcRelative:
        result.effective_address = source.address + 4u + displacement;
        break;

    case Operation::LoadLongPcRelative:
    case Operation::MoveAddressPcRelative:
        result.effective_address = (source.address & 0xFFFFFFFCu) + 4u + displacement;
        break;

    default:
        break;
    }

    result.target_address = source.target_address;

    if (source.instruction.has_delay_slot) {
        result.delay_slot = {DelaySlotRole::Owner, source.address + 2u};
    } else if (source.is_delay_slot) {
        result.delay_slot = {DelaySlotRole::Slot, source.address - 2u};
    }
    result.is_privileged = source.instruction.is_privileged;
    result.branch_register_relative =
        source.instruction.kind == katana::sh4::InstructionKind::Braf ||
        source.instruction.kind == katana::sh4::InstructionKind::Bsrf;

    return result;
}

} // namespace

Operation lowering_operation_for_instruction(const katana::sh4::InstructionKind kind) noexcept {
    return lower_operation(kind);
}

std::string_view operation_name(const Operation operation) noexcept {
    switch (operation) {
    case Operation::Unknown:
        return "unknown";

    case Operation::Nop:
        return "nop";

    case Operation::MovImmediate:
        return "mov_imm";

    case Operation::Constant32:
        return "constant32";

    case Operation::AddImmediate:
        return "add_imm";

    case Operation::MovRegister:
        return "mov_reg";

    case Operation::AddRegister:
        return "add_reg";

    case Operation::SubRegister:
        return "sub_reg";

    case Operation::NegateRegister:
        return "neg_reg";

    case Operation::NotRegister:
        return "not_reg";
    case Operation::AddWithCarry:
        return "add_with_carry";

    case Operation::AddWithOverflow:
        return "add_with_overflow";

    case Operation::SubWithCarry:
        return "sub_with_carry";

    case Operation::SubWithOverflow:
        return "sub_with_overflow";

    case Operation::NegateWithCarry:
        return "negate_with_carry";
    case Operation::ExtendUnsignedByte:
        return "extend_unsigned_byte";

    case Operation::ExtendUnsignedWord:
        return "extend_unsigned_word";

    case Operation::ExtendSignedByte:
        return "extend_signed_byte";

    case Operation::ExtendSignedWord:
        return "extend_signed_word";

    case Operation::SwapBytes:
        return "swap_bytes";

    case Operation::SwapWords:
        return "swap_words";

    case Operation::ExtractMiddle:
        return "extract_middle";
    case Operation::DecrementAndTest:
        return "decrement_and_test";

    case Operation::MoveT:
        return "move_t";
    case Operation::ShiftLogicalLeftOne:
        return "shift_logical_left_one";

    case Operation::ShiftLogicalRightOne:
        return "shift_logical_right_one";

    case Operation::ShiftArithmeticLeftOne:
        return "shift_arithmetic_left_one";

    case Operation::ShiftArithmeticRightOne:
        return "shift_arithmetic_right_one";
    case Operation::ShiftLogicalLeftTwo:
        return "shift_logical_left_two";

    case Operation::ShiftLogicalLeftEight:
        return "shift_logical_left_eight";

    case Operation::ShiftLogicalLeftSixteen:
        return "shift_logical_left_sixteen";

    case Operation::ShiftLogicalRightTwo:
        return "shift_logical_right_two";

    case Operation::ShiftLogicalRightEight:
        return "shift_logical_right_eight";

    case Operation::ShiftLogicalRightSixteen:
        return "shift_logical_right_sixteen";
    case Operation::RotateLeft:
        return "rotate_left";

    case Operation::RotateRight:
        return "rotate_right";

    case Operation::RotateLeftThroughT:
        return "rotate_left_through_t";

    case Operation::RotateRightThroughT:
        return "rotate_right_through_t";
    case Operation::ShiftArithmeticDynamic:
        return "shift_arithmetic_dynamic";

    case Operation::ShiftLogicalDynamic:
        return "shift_logical_dynamic";
    case Operation::MultiplyLong:
        return "multiply_long";

    case Operation::MultiplySignedWord:
        return "multiply_signed_word";

    case Operation::MultiplyUnsignedWord:
        return "multiply_unsigned_word";
    case Operation::DoubleMultiplySignedLong:
        return "double_multiply_signed_long";

    case Operation::DoubleMultiplyUnsignedLong:
        return "double_multiply_unsigned_long";
    case Operation::MultiplyAccumulateWord:
        return "multiply_accumulate_word";

    case Operation::MultiplyAccumulateLong:
        return "multiply_accumulate_long";
    case Operation::DivideInitializeUnsigned:
        return "divide_initialize_unsigned";

    case Operation::DivideInitializeSigned:
        return "divide_initialize_signed";

    case Operation::DivideStep:
        return "divide_step";
    case Operation::ClearMac:
        return "clear_mac";
    case Operation::AndRegister:
        return "and_reg";

    case Operation::OrRegister:
        return "or_reg";

    case Operation::XorRegister:
        return "xor_reg";

    case Operation::AndImmediate:
        return "and_imm";

    case Operation::OrImmediate:
        return "or_imm";

    case Operation::XorImmediate:
        return "xor_imm";
    case Operation::ClearS:
        return "clear_s";

    case Operation::SetS:
        return "set_s";
    case Operation::ClearT:
        return "clear_t";

    case Operation::SetT:
        return "set_t";

    case Operation::CompareEqualImmediate:
        return "compare_equal_imm";

    case Operation::CompareEqualRegister:
        return "compare_equal_reg";

    case Operation::CompareHigherOrSame:
        return "compare_higher_or_same";

    case Operation::CompareGreaterOrEqual:
        return "compare_greater_or_equal";

    case Operation::CompareHigher:
        return "compare_higher";

    case Operation::CompareGreaterThan:
        return "compare_greater_than";

    case Operation::ComparePositiveOrZero:
        return "compare_positive_or_zero";

    case Operation::ComparePositive:
        return "compare_positive";

    case Operation::CompareString:
        return "compare_string";
    case Operation::TestImmediate:
        return "test_imm";

    case Operation::TestRegister:
        return "test_reg";
    case Operation::TestByteImmediate:
        return "test_byte_imm";
    case Operation::AndByteImmediate:
        return "and_byte_imm";
    case Operation::XorByteImmediate:
        return "xor_byte_imm";
    case Operation::OrByteImmediate:
        return "or_byte_imm";
    case Operation::TestAndSetByte:
        return "test_and_set_byte";
    case Operation::LoadByteSigned:
        return "load_s8";

    case Operation::LoadWordSigned:
        return "load_s16";

    case Operation::LoadLong:
        return "load_u32";

    case Operation::StoreByte:
        return "store_u8";

    case Operation::StoreWord:
        return "store_u16";

    case Operation::StoreLong:
        return "store_u32";

    case Operation::StoreBytePreDecrement:
        return "store_u8_predecrement";

    case Operation::StoreWordPreDecrement:
        return "store_u16_predecrement";

    case Operation::StoreLongPreDecrement:
        return "store_u32_predecrement";

    case Operation::LoadByteSignedPostIncrement:
        return "load_s8_postincrement";

    case Operation::LoadWordSignedPostIncrement:
        return "load_s16_postincrement";

    case Operation::LoadLongPostIncrement:
        return "load_u32_postincrement";

    case Operation::StoreByteDisplacement:
        return "store_u8_displacement";

    case Operation::StoreWordDisplacement:
        return "store_u16_displacement";

    case Operation::StoreLongDisplacement:
        return "store_u32_displacement";

    case Operation::LoadByteSignedDisplacement:
        return "load_s8_displacement";

    case Operation::LoadWordSignedDisplacement:
        return "load_s16_displacement";

    case Operation::LoadLongDisplacement:
        return "load_u32_displacement";

    case Operation::StoreByteR0Indexed:
        return "store_u8_r0_indexed";

    case Operation::StoreWordR0Indexed:
        return "store_u16_r0_indexed";

    case Operation::StoreLongR0Indexed:
        return "store_u32_r0_indexed";

    case Operation::LoadByteSignedR0Indexed:
        return "load_s8_r0_indexed";

    case Operation::LoadWordSignedR0Indexed:
        return "load_s16_r0_indexed";

    case Operation::LoadLongR0Indexed:
        return "load_u32_r0_indexed";

    case Operation::StoreByteGbrDisplacement:
        return "store_u8_gbr_displacement";

    case Operation::StoreWordGbrDisplacement:
        return "store_u16_gbr_displacement";

    case Operation::StoreLongGbrDisplacement:
        return "store_u32_gbr_displacement";

    case Operation::LoadByteSignedGbrDisplacement:
        return "load_s8_gbr_displacement";

    case Operation::LoadWordSignedGbrDisplacement:
        return "load_s16_gbr_displacement";

    case Operation::LoadLongGbrDisplacement:
        return "load_u32_gbr_displacement";

    case Operation::LoadWordSignedPcRelative:
        return "load_s16_pc_relative";

    case Operation::LoadLongPcRelative:
        return "load_u32_pc_relative";

    case Operation::MoveAddressPcRelative:
        return "move_address_pc_relative";

    case Operation::StoreSpecialRegister:
        return "store_special_register";

    case Operation::StoreSpecialRegisterPreDecrement:
        return "store_special_register_pre_decrement";

    case Operation::LoadSpecialRegister:
        return "load_special_register";

    case Operation::LoadSpecialRegisterPostIncrement:
        return "load_special_register_post_increment";

    case Operation::TrapAlways:
        return "trap_always";

    case Operation::ReturnFromException:
        return "return_from_exception";

    case Operation::Sleep:
        return "sleep";
    case Operation::Prefetch:
        return "prefetch";
    case Operation::FmovRegister:
        return "fmov_reg";
    case Operation::FmovLoad:
        return "fmov_load";
    case Operation::FmovLoadPostIncrement:
        return "fmov_load_postinc";
    case Operation::FmovLoadR0Indexed:
        return "fmov_load_r0";
    case Operation::FmovStore:
        return "fmov_store";
    case Operation::FmovStorePreDecrement:
        return "fmov_store_predec";
    case Operation::FmovStoreR0Indexed:
        return "fmov_store_r0";
    case Operation::Fldi0:
        return "fldi0";
    case Operation::Fldi1:
        return "fldi1";
    case Operation::Flds:
        return "flds";
    case Operation::Fsts:
        return "fsts";
    case Operation::Fabs:
        return "fabs";
    case Operation::Fadd:
        return "fadd";
    case Operation::FcmpEqual:
        return "fcmp_eq";
    case Operation::FcmpGreater:
        return "fcmp_gt";
    case Operation::Fdiv:
        return "fdiv";
    case Operation::FloatFromFpul:
        return "float";
    case Operation::Fmac:
        return "fmac";
    case Operation::Fmul:
        return "fmul";
    case Operation::Fneg:
        return "fneg";
    case Operation::Fsqrt:
        return "fsqrt";
    case Operation::Fsrra:
        return "fsrra";
    case Operation::Fsca:
        return "fsca";
    case Operation::Fipr:
        return "fipr";
    case Operation::Ftrv:
        return "ftrv";
    case Operation::Fsub:
        return "fsub";
    case Operation::Ftrc:
        return "ftrc";
    case Operation::FcnvDoubleToSingle:
        return "fcnvds";
    case Operation::FcnvSingleToDouble:
        return "fcnvsd";
    case Operation::Frchg:
        return "frchg";
    case Operation::Fschg:
        return "fschg";

    case Operation::Branch:
        return "branch";

    case Operation::Call:
        return "call";

    case Operation::BranchIfTrue:
        return "branch_if_true";

    case Operation::BranchIfFalse:
        return "branch_if_false";

    case Operation::JumpRegister:
        return "jump_register";

    case Operation::CallRegister:
        return "call_register";

    case Operation::Return:
        return "return";
    }

    return "unknown";
}

Function
lower_function(const std::span<const katana::sh4::DisassemblyLine> lines,
               const katana::analysis::FunctionInfo& function,
               const std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges,
               const std::span<const std::uint32_t> function_entries) {
    const auto source_blocks =
        katana::analysis::build_basic_blocks(lines, resolved_edges, function_entries);

    std::unordered_map<std::uint32_t, const katana::analysis::BasicBlock*> block_by_address;

    block_by_address.reserve(source_blocks.size());

    for (const auto& block : source_blocks) {
        block_by_address.emplace(block.start_address, &block);
    }

    Function result;
    result.entry_address = function.entry_address;
    result.direct_callees = function.direct_callees;
    result.indirect_call_sites = function.indirect_call_sites;

    result.blocks.reserve(function.block_addresses.size());

    for (const auto block_address : function.block_addresses) {
        const auto block_iterator = block_by_address.find(block_address);

        if (block_iterator == block_by_address.end()) {
            throw std::runtime_error("Ein Funktionsblock wurde in der CFG nicht gefunden.");
        }

        const auto& source_block = *block_iterator->second;

        BasicBlock target_block;
        target_block.start_address = source_block.start_address;
        target_block.successors = source_block.successors;
        target_block.successors.erase(
            std::remove_if(target_block.successors.begin(),
                           target_block.successors.end(),
                           [&function](const std::uint32_t successor) {
                               return std::find(function.block_addresses.begin(),
                                                function.block_addresses.end(),
                                                successor) == function.block_addresses.end();
                           }),
            target_block.successors.end());
        target_block.has_indirect_successor = source_block.has_indirect_successor;

        target_block.instructions.reserve(source_block.lines.size());

        for (const auto& source_line : source_block.lines) {
            auto instruction = lower_instruction(source_line);
            for (const auto& edge : resolved_edges) {
                if (edge.instruction_address == source_line.address) {
                    instruction.resolved_targets.push_back(edge.target_address);
                }
            }
            std::sort(instruction.resolved_targets.begin(), instruction.resolved_targets.end());
            instruction.resolved_targets.erase(std::unique(instruction.resolved_targets.begin(),
                                                           instruction.resolved_targets.end()),
                                               instruction.resolved_targets.end());
            target_block.instructions.push_back(std::move(instruction));
        }

        result.blocks.push_back(std::move(target_block));
    }

    std::sort(result.blocks.begin(),
              result.blocks.end(),
              [](const BasicBlock& left, const BasicBlock& right) {
                  return left.start_address < right.start_address;
              });

    return result;
}

std::vector<Function>
lower_program(const std::span<const katana::sh4::DisassemblyLine> lines,
              const std::span<const katana::analysis::FunctionInfo> functions,
              const std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges) {
    std::vector<Function> result;
    result.reserve(functions.size());

    std::vector<std::uint32_t> function_entries;
    function_entries.reserve(functions.size());
    for (const auto& function : functions) {
        function_entries.push_back(function.entry_address);
    }

    for (const auto& function : functions) {
        result.push_back(lower_function(lines, function, resolved_edges, function_entries));
    }

    std::sort(result.begin(), result.end(), [](const Function& left, const Function& right) {
        return left.entry_address < right.entry_address;
    });

    return result;
}

std::vector<Function> lower_program(const katana::analysis::ControlFlowAnalysisResult& analysis) {
    std::vector<std::uint32_t> seeds;
    seeds.reserve(analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions) {
        if (katana::analysis::control_flow_evidence_proven(function.evidence))
            seeds.push_back(function.address);
    }
    for (const auto& edge : analysis.resolved_edges) {
        if (edge.kind == katana::analysis::ResolvedControlFlowKind::Call &&
            katana::analysis::control_flow_evidence_proven(
                katana::analysis::resolved_edge_evidence(edge))) {
            seeds.push_back(edge.target_address);
        }
    }
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    const auto functions = katana::analysis::discover_functions(
        analysis.recursive.instructions, seeds, analysis.resolved_edges);
    return lower_program(analysis.recursive.instructions, functions, analysis.resolved_edges);
}

} // namespace katana::ir
