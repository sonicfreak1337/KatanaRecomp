#include "katana/ir/ir.hpp"

namespace katana::ir {
namespace {

constexpr auto none = OperandWidth::None;
constexpr auto bit = OperandWidth::Bit1;
constexpr auto nibble = OperandWidth::Bits4;
constexpr auto byte = OperandWidth::Bits8;
constexpr auto branch_displacement = OperandWidth::Bits12;
constexpr auto word = OperandWidth::Bits16;
constexpr auto longword = OperandWidth::Bits32;
constexpr auto quadword = OperandWidth::Bits64;

OperandWidths register_binary() {
    return {longword, longword, none, none, none, none};
}

OperandWidths register_unary() {
    return {longword, longword, none, none, none, none};
}

OperandWidths memory_load(const OperandWidth memory_width) {
    return {longword, none, none, none, memory_width, longword};
}

OperandWidths memory_store(const OperandWidth memory_width) {
    return {none, longword, none, none, memory_width, longword};
}

} // namespace

OperandWidths operation_operand_widths(const Operation operation) noexcept {
    switch (operation) {
    case Operation::Unknown:
    case Operation::Nop:
    case Operation::ClearS:
    case Operation::SetS:
    case Operation::ClearT:
    case Operation::SetT:
    case Operation::DivideInitializeUnsigned:
    case Operation::ClearMac:
    case Operation::Sleep:
    case Operation::LoadTlb:
    case Operation::Frchg:
    case Operation::Fschg:
        return {};

    case Operation::Prefetch:
    case Operation::Ocbi:
    case Operation::Ocbp:
    case Operation::Ocbwb:
        return {none, longword, none, none, none, longword};

    case Operation::MovcaLong:
        return memory_store(longword);

    case Operation::DivideInitializeSigned:
        return {none, longword, none, none, none, none};
    case Operation::ReturnFromException:
        return {none, longword, none, none, none, longword};

    case Operation::MovImmediate:
        return {longword, none, byte, none, none, none};
    case Operation::Constant32:
        return {longword, none, longword, none, none, none};
    case Operation::AddImmediate:
    case Operation::AndImmediate:
    case Operation::OrImmediate:
    case Operation::XorImmediate:
        return {longword, longword, byte, none, none, none};
    case Operation::CompareEqualImmediate:
    case Operation::TestImmediate:
        return {bit, longword, byte, none, none, none};
    case Operation::TestByteImmediate:
        return {bit, byte, byte, none, byte, longword};
    case Operation::AndByteImmediate:
    case Operation::XorByteImmediate:
    case Operation::OrByteImmediate:
        return {none, byte, byte, none, byte, longword};
    case Operation::TestAndSetByte:
        return {bit, byte, none, none, byte, longword};

    case Operation::MoveT:
        return {longword, bit, none, none, none, none};
    case Operation::Fldi0:
    case Operation::Fldi1:
        return {longword, none, none, none, none, none};
    case Operation::Flds:
    case Operation::Fsts:
    case Operation::FloatFromFpul:
    case Operation::Ftrc:
        return register_unary();
    case Operation::FcnvDoubleToSingle:
        return {longword, quadword, none, none, none, none};
    case Operation::FcnvSingleToDouble:
        return {quadword, longword, none, none, none, none};
    case Operation::ExtendUnsignedByte:
    case Operation::ExtendSignedByte:
        return {longword, byte, none, none, none, none};
    case Operation::ExtendUnsignedWord:
    case Operation::ExtendSignedWord:
        return {longword, word, none, none, none, none};

    case Operation::MultiplySignedWord:
    case Operation::MultiplyUnsignedWord:
        return {longword, word, none, none, none, none};
    case Operation::DoubleMultiplySignedLong:
    case Operation::DoubleMultiplyUnsignedLong:
        return {quadword, longword, none, none, none, none};
    case Operation::MultiplyAccumulateWord:
        return {none, word, none, none, word, longword};
    case Operation::MultiplyAccumulateLong:
        return {none, longword, none, none, longword, longword};

    case Operation::StoreByte:
    case Operation::StoreBytePreDecrement:
    case Operation::StoreByteR0Indexed:
        return memory_store(byte);
    case Operation::LoadByteSigned:
    case Operation::LoadByteSignedPostIncrement:
    case Operation::LoadByteSignedR0Indexed:
        return memory_load(byte);
    case Operation::StoreWord:
    case Operation::StoreWordPreDecrement:
    case Operation::StoreWordR0Indexed:
        return memory_store(word);
    case Operation::LoadWordSigned:
    case Operation::LoadWordSignedPostIncrement:
    case Operation::LoadWordSignedR0Indexed:
        return memory_load(word);
    case Operation::StoreLong:
    case Operation::StoreLongPreDecrement:
    case Operation::StoreLongR0Indexed:
        return memory_store(longword);
    case Operation::FmovStore:
    case Operation::FmovStorePreDecrement:
    case Operation::FmovStoreR0Indexed:
        return memory_store(quadword);
    case Operation::LoadLong:
    case Operation::LoadLongPostIncrement:
    case Operation::LoadLongR0Indexed:
        return memory_load(longword);
    case Operation::FmovLoad:
    case Operation::FmovLoadPostIncrement:
    case Operation::FmovLoadR0Indexed:
        return memory_load(quadword);

    case Operation::StoreByteDisplacement:
        return {none, longword, none, nibble, byte, longword};
    case Operation::LoadByteSignedDisplacement:
        return {longword, none, none, nibble, byte, longword};
    case Operation::StoreWordDisplacement:
        return {none, longword, none, nibble, word, longword};
    case Operation::LoadWordSignedDisplacement:
        return {longword, none, none, nibble, word, longword};
    case Operation::StoreLongDisplacement:
        return {none, longword, none, nibble, longword, longword};
    case Operation::LoadLongDisplacement:
        return {longword, none, none, nibble, longword, longword};

    case Operation::StoreByteGbrDisplacement:
        return {none, longword, none, byte, byte, longword};
    case Operation::LoadByteSignedGbrDisplacement:
        return {longword, none, none, byte, byte, longword};
    case Operation::StoreWordGbrDisplacement:
        return {none, longword, none, byte, word, longword};
    case Operation::LoadWordSignedGbrDisplacement:
    case Operation::LoadWordSignedPcRelative:
        return {longword, none, none, byte, word, longword};
    case Operation::StoreLongGbrDisplacement:
        return {none, longword, none, byte, longword, longword};
    case Operation::LoadLongGbrDisplacement:
    case Operation::LoadLongPcRelative:
        return {longword, none, none, byte, longword, longword};
    case Operation::MoveAddressPcRelative:
        return {longword, none, none, byte, none, longword};

    case Operation::StoreSpecialRegister:
    case Operation::LoadSpecialRegister:
        return register_binary();
    case Operation::StoreSpecialRegisterPreDecrement:
        return {none, longword, none, none, longword, longword};
    case Operation::LoadSpecialRegisterPostIncrement:
        return {longword, none, none, none, longword, longword};

    case Operation::Branch:
    case Operation::Call:
        return {none, none, none, branch_displacement, none, longword};
    case Operation::BranchIfTrue:
    case Operation::BranchIfFalse:
        return {none, bit, none, byte, none, longword};
    case Operation::JumpRegister:
    case Operation::CallRegister:
        return {none, longword, none, none, none, longword};
    case Operation::Return:
        return {none, longword, none, none, none, longword};
    case Operation::TrapAlways:
        return {none, none, byte, none, none, longword};

    case Operation::CompareEqualRegister:
    case Operation::CompareHigherOrSame:
    case Operation::CompareGreaterOrEqual:
    case Operation::CompareHigher:
    case Operation::CompareGreaterThan:
    case Operation::CompareString:
    case Operation::TestRegister:
        return {bit, longword, none, none, none, none};
    case Operation::ComparePositiveOrZero:
    case Operation::ComparePositive:
        return {bit, longword, none, none, none, none};
    case Operation::FcmpEqual:
    case Operation::FcmpGreater:
        return {bit, longword, none, none, none, none};

    case Operation::MovRegister:
    case Operation::AddRegister:
    case Operation::SubRegister:
    case Operation::NegateRegister:
    case Operation::NotRegister:
    case Operation::AddWithCarry:
    case Operation::AddWithOverflow:
    case Operation::SubWithCarry:
    case Operation::SubWithOverflow:
    case Operation::NegateWithCarry:
    case Operation::SwapBytes:
    case Operation::SwapWords:
    case Operation::ExtractMiddle:
    case Operation::ShiftArithmeticDynamic:
    case Operation::ShiftLogicalDynamic:
    case Operation::MultiplyLong:
    case Operation::DivideStep:
    case Operation::AndRegister:
    case Operation::OrRegister:
    case Operation::XorRegister:
    case Operation::FmovRegister:
    case Operation::Fadd:
    case Operation::Fdiv:
    case Operation::Fmac:
    case Operation::Fmul:
    case Operation::Fsub:
        return register_binary();
    case Operation::DecrementAndTest:
    case Operation::ShiftLogicalLeftOne:
    case Operation::ShiftLogicalRightOne:
    case Operation::ShiftArithmeticLeftOne:
    case Operation::ShiftArithmeticRightOne:
    case Operation::ShiftLogicalLeftTwo:
    case Operation::ShiftLogicalLeftEight:
    case Operation::ShiftLogicalLeftSixteen:
    case Operation::ShiftLogicalRightTwo:
    case Operation::ShiftLogicalRightEight:
    case Operation::ShiftLogicalRightSixteen:
    case Operation::RotateLeft:
    case Operation::RotateRight:
    case Operation::RotateLeftThroughT:
    case Operation::RotateRightThroughT:
    case Operation::Fabs:
    case Operation::Fneg:
    case Operation::Fsqrt:
    case Operation::Fsrra:
        return register_unary();
    case Operation::Fsca:
        return {quadword, longword, none, none, none, none};
    case Operation::Fipr:
    case Operation::Ftrv:
        return {longword, longword, none, none, none, none};
    }
    return {};
}

std::string_view operand_width_name(const OperandWidth width) noexcept {
    switch (width) {
    case OperandWidth::None:
        return "none";
    case OperandWidth::Bit1:
        return "i1";
    case OperandWidth::Bits4:
        return "i4";
    case OperandWidth::Bits8:
        return "i8";
    case OperandWidth::Bits12:
        return "i12";
    case OperandWidth::Bits16:
        return "i16";
    case OperandWidth::Bits32:
        return "i32";
    case OperandWidth::Bits64:
        return "i64";
    }
    return "unknown";
}

} // namespace katana::ir
