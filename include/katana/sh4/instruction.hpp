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
    AndRegister,
    OrRegister,
    XorRegister,
    AndImmediate,
    OrImmediate,
    XorImmediate,
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
    Bra,
    Bsr,
    Bt,
    Bf,
    BtS,
    BfS,
    Jmp,
    Jsr
};

enum class ControlFlowKind {
    None,
    UnconditionalBranch,
    ConditionalBranch,
    Call,
    Return,
    IndirectBranch,
    IndirectCall
};

struct DecodedInstruction {
    std::uint16_t opcode = 0;
    InstructionKind kind = InstructionKind::Unknown;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::uint8_t branch_register = 0;

    std::int32_t immediate = 0;
    std::int32_t displacement = 0;

    ControlFlowKind control_flow = ControlFlowKind::None;
    bool has_delay_slot = false;

    std::string text;

    [[nodiscard]] bool is_known() const noexcept {
        return kind != InstructionKind::Unknown;
    }

    [[nodiscard]] bool changes_control_flow() const noexcept {
        return control_flow != ControlFlowKind::None;
    }
};

}
