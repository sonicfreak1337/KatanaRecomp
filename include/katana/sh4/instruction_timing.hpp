#pragma once

#include "katana/sh4/decoder.hpp"

#include <cstdint>

namespace katana::sh4 {

enum class InstructionTimingClass : std::uint8_t {
    SimpleInteger,
    ControlFlow,
    Memory,
    MacDiv,
    SimpleFpu,
    ComplexFpu,
    DeviceBusBoundary,
};

struct InstructionTiming {
    InstructionTimingClass timing_class = InstructionTimingClass::SimpleInteger;
    std::uint64_t guest_cycles = 1u;
    bool requires_cycle_flush = false;
};

[[nodiscard]] inline InstructionTiming
instruction_timing(const DecodedInstruction& instruction) noexcept {
    using Kind = InstructionKind;
    switch (instruction.kind) {
    case Kind::Rts:
    case Kind::TrapAlways:
    case Kind::ReturnFromException:
    case Kind::Sleep:
    case Kind::Bra:
    case Kind::Bsr:
    case Kind::Braf:
    case Kind::Bsrf:
    case Kind::Bt:
    case Kind::Bf:
    case Kind::BtS:
    case Kind::BfS:
    case Kind::Jmp:
    case Kind::Jsr:
        return {InstructionTimingClass::ControlFlow, 2u, false};

    case Kind::MovByteStore:
    case Kind::MovWordStore:
    case Kind::MovLongStore:
    case Kind::MovByteLoad:
    case Kind::MovWordLoad:
    case Kind::MovLongLoad:
    case Kind::MovByteStorePreDecrement:
    case Kind::MovWordStorePreDecrement:
    case Kind::MovLongStorePreDecrement:
    case Kind::MovByteLoadPostIncrement:
    case Kind::MovWordLoadPostIncrement:
    case Kind::MovLongLoadPostIncrement:
    case Kind::MovByteStoreDisplacement:
    case Kind::MovWordStoreDisplacement:
    case Kind::MovLongStoreDisplacement:
    case Kind::MovByteLoadDisplacement:
    case Kind::MovWordLoadDisplacement:
    case Kind::MovLongLoadDisplacement:
    case Kind::MovByteStoreR0Indexed:
    case Kind::MovWordStoreR0Indexed:
    case Kind::MovLongStoreR0Indexed:
    case Kind::MovByteLoadR0Indexed:
    case Kind::MovWordLoadR0Indexed:
    case Kind::MovLongLoadR0Indexed:
    case Kind::MovByteStoreGbrDisplacement:
    case Kind::MovWordStoreGbrDisplacement:
    case Kind::MovLongStoreGbrDisplacement:
    case Kind::MovByteLoadGbrDisplacement:
    case Kind::MovWordLoadGbrDisplacement:
    case Kind::MovLongLoadGbrDisplacement:
    case Kind::MovWordLoadPcRelative:
    case Kind::MovLongLoadPcRelative:
    case Kind::StoreSpecialRegisterPreDecrement:
    case Kind::LoadSpecialRegisterPostIncrement:
    case Kind::TestByteImmediate:
    case Kind::AndByteImmediate:
    case Kind::XorByteImmediate:
    case Kind::OrByteImmediate:
    case Kind::TestAndSetByte:
    case Kind::FmovLoad:
    case Kind::FmovLoadPostIncrement:
    case Kind::FmovLoadR0Indexed:
    case Kind::FmovStore:
    case Kind::FmovStorePreDecrement:
    case Kind::FmovStoreR0Indexed:
        return {InstructionTimingClass::Memory, 2u, true};

    case Kind::MultiplyLong:
    case Kind::MultiplySignedWord:
    case Kind::MultiplyUnsignedWord:
    case Kind::DoubleMultiplySignedLong:
    case Kind::DoubleMultiplyUnsignedLong:
    case Kind::MultiplyAccumulateWord:
    case Kind::MultiplyAccumulateLong:
    case Kind::DivideInitializeUnsigned:
    case Kind::DivideInitializeSigned:
    case Kind::DivideStep:
        return {InstructionTimingClass::MacDiv, 3u, false};

    case Kind::FmovRegister:
    case Kind::Fldi0:
    case Kind::Fldi1:
    case Kind::Flds:
    case Kind::Fsts:
    case Kind::Fabs:
    case Kind::Fadd:
    case Kind::FcmpEqual:
    case Kind::FcmpGreater:
    case Kind::FloatFromFpul:
    case Kind::Fmac:
    case Kind::Fmul:
    case Kind::Fneg:
    case Kind::Fsub:
    case Kind::Ftrc:
    case Kind::Frchg:
    case Kind::Fschg:
        return {InstructionTimingClass::SimpleFpu, 2u, false};

    case Kind::Fdiv:
    case Kind::Fsqrt:
    case Kind::Fsrra:
    case Kind::Fsca:
    case Kind::Fipr:
    case Kind::Ftrv:
    case Kind::FcnvDoubleToSingle:
    case Kind::FcnvSingleToDouble:
        return {InstructionTimingClass::ComplexFpu, 4u, false};

    case Kind::LoadTlb:
    case Kind::Prefetch:
    case Kind::Ocbi:
    case Kind::Ocbp:
    case Kind::Ocbwb:
    case Kind::MovcaLong:
        return {InstructionTimingClass::DeviceBusBoundary, 3u, true};

    default:
        return {InstructionTimingClass::SimpleInteger, 1u, false};
    }
}

[[nodiscard]] inline InstructionTiming instruction_timing(const std::uint16_t opcode) {
    return instruction_timing(decode(opcode));
}

} // namespace katana::sh4
