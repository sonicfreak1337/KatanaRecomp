#pragma once

#include <cstdint>
#include <string>

namespace katana::sh4 {

enum class SpecialRegister {
    None,
    Mach,
    Macl,
    Pr,
    Fpul,
    Fpscr,
    Sr,
    Gbr,
    Vbr,
    Ssr,
    Spc,
    Sgr,
    Dbr,
    Bank0,
    Bank1,
    Bank2,
    Bank3,
    Bank4,
    Bank5,
    Bank6,
    Bank7
};

enum class InstructionKind {
    Unknown,
    Nop,
    Rts,
    MovImmediate,
    AddImmediate,
    MovRegister,
    AddRegister,
    SubRegister,
    NegateRegister,
    NotRegister,
    AddWithCarry,
    AddWithOverflow,
    SubWithCarry,
    SubWithOverflow,
    NegateWithCarry,
    ExtendUnsignedByte,
    ExtendUnsignedWord,
    ExtendSignedByte,
    ExtendSignedWord,
    SwapBytes,
    SwapWords,
    ExtractMiddle,
    DecrementAndTest,
    MoveT,
    ShiftLogicalLeftOne,
    ShiftLogicalRightOne,
    ShiftArithmeticLeftOne,
    ShiftArithmeticRightOne,
    ShiftLogicalLeftTwo,
    ShiftLogicalLeftEight,
    ShiftLogicalLeftSixteen,
    ShiftLogicalRightTwo,
    ShiftLogicalRightEight,
    ShiftLogicalRightSixteen,
    RotateLeft,
    RotateRight,
    RotateLeftThroughT,
    RotateRightThroughT,
    ShiftArithmeticDynamic,
    ShiftLogicalDynamic,
    MultiplyLong,
    MultiplySignedWord,
    MultiplyUnsignedWord,
    DoubleMultiplySignedLong,
    DoubleMultiplyUnsignedLong,
    MultiplyAccumulateWord,
    MultiplyAccumulateLong,
    DivideInitializeUnsigned,
    DivideInitializeSigned,
    DivideStep,
    AndRegister,
    OrRegister,
    XorRegister,
    AndImmediate,
    OrImmediate,
    XorImmediate,
    ClearS,
    SetS,
    ClearT,
    SetT,
    CompareEqualImmediate,
    CompareEqualRegister,
    CompareHigherOrSame,
    CompareGreaterOrEqual,
    CompareHigher,
    CompareGreaterThan,
    ComparePositiveOrZero,
    ComparePositive,
    CompareString,
    TestImmediate,
    TestRegister,
    MovByteStore,
    MovWordStore,
    MovLongStore,
    MovByteLoad,
    MovWordLoad,
    MovLongLoad,
    MovByteStorePreDecrement,
    MovWordStorePreDecrement,
    MovLongStorePreDecrement,
    MovByteLoadPostIncrement,
    MovWordLoadPostIncrement,
    MovLongLoadPostIncrement,
    MovByteStoreDisplacement,
    MovWordStoreDisplacement,
    MovLongStoreDisplacement,
    MovByteLoadDisplacement,
    MovWordLoadDisplacement,
    MovLongLoadDisplacement,
    MovByteStoreR0Indexed,
    MovWordStoreR0Indexed,
    MovLongStoreR0Indexed,
    MovByteLoadR0Indexed,
    MovWordLoadR0Indexed,
    MovLongLoadR0Indexed,
    MovByteStoreGbrDisplacement,
    MovWordStoreGbrDisplacement,
    MovLongStoreGbrDisplacement,
    MovByteLoadGbrDisplacement,
    MovWordLoadGbrDisplacement,
    MovLongLoadGbrDisplacement,
    MovWordLoadPcRelative,
    MovLongLoadPcRelative,
    MoveAddressPcRelative,
    StoreSpecialRegister,
    StoreSpecialRegisterPreDecrement,
    LoadSpecialRegister,
    LoadSpecialRegisterPostIncrement,
    TrapAlways,
    ReturnFromException,
    Sleep,
    Bra,
    Bsr,
    Bt,
    Bf,
    BtS,
    BfS,
    Jmp,
    Jsr,
    FmovRegister,
    FmovLoad,
    FmovLoadPostIncrement,
    FmovLoadR0Indexed,
    FmovStore,
    FmovStorePreDecrement,
    FmovStoreR0Indexed,
    Fldi0,
    Fldi1,
    Flds,
    Fsts,
    Fabs,
    Fadd,
    FcmpEqual,
    FcmpGreater,
    Fdiv,
    FloatFromFpul,
    Fmac,
    Fmul,
    Fneg,
    Fsqrt,
    Fsub,
    Ftrc,
    FcnvDoubleToSingle,
    FcnvSingleToDouble,
    Frchg,
    Fschg
};

enum class ControlFlowKind {
    None,
    UnconditionalBranch,
    ConditionalBranch,
    Call,
    Return,
    IndirectBranch,
    IndirectCall,
    Trap,
    ExceptionReturn,
    Halt
};

struct DecodedInstruction {
    std::uint16_t opcode = 0;
    InstructionKind kind = InstructionKind::Unknown;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::uint8_t branch_register = 0;

    std::int32_t immediate = 0;
    std::int32_t displacement = 0;
    SpecialRegister special_register = SpecialRegister::None;

    ControlFlowKind control_flow = ControlFlowKind::None;
    bool has_delay_slot = false;
    bool is_privileged = false;

    std::string text;

    [[nodiscard]] bool is_known() const noexcept {
        return kind != InstructionKind::Unknown;
    }

    [[nodiscard]] bool changes_control_flow() const noexcept {
        return control_flow != ControlFlowKind::None;
    }
};

}
