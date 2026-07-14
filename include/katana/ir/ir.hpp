#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace katana::ir {

enum class OperandWidth : std::uint8_t {
    None = 0u,
    Bit1 = 1u,
    Bits4 = 4u,
    Bits8 = 8u,
    Bits12 = 12u,
    Bits16 = 16u,
    Bits32 = 32u,
    Bits64 = 64u
};

struct OperandWidths {
    // Semantic value widths are independent from encoded field widths. Multiple
    // register inputs of one operation share `input` on the current SH-4 IR.
    OperandWidth result = OperandWidth::None;
    OperandWidth input = OperandWidth::None;
    // Immediate and displacement describe their encoded instruction fields.
    OperandWidth immediate = OperandWidth::None;
    OperandWidth displacement = OperandWidth::None;
    // Memory is the transfer width; address is the effective-address width.
    OperandWidth memory = OperandWidth::None;
    OperandWidth address = OperandWidth::None;
};

enum class StatusRegisterBit : std::uint8_t {
    None = 0u,
    T = 1u << 0u,
    S = 1u << 1u,
    Q = 1u << 2u,
    M = 1u << 3u,
    Full = 1u << 7u
};

struct StatusRegisterEffects {
    StatusRegisterBit reads = StatusRegisterBit::None;
    StatusRegisterBit writes = StatusRegisterBit::None;
};

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

enum class Operation {
    Unknown,
    Nop,
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
    LoadByteSigned,
    LoadWordSigned,
    LoadLong,
    StoreByte,
    StoreWord,
    StoreLong,
    StoreBytePreDecrement,
    StoreWordPreDecrement,
    StoreLongPreDecrement,
    LoadByteSignedPostIncrement,
    LoadWordSignedPostIncrement,
    LoadLongPostIncrement,
    StoreByteDisplacement,
    StoreWordDisplacement,
    StoreLongDisplacement,
    LoadByteSignedDisplacement,
    LoadWordSignedDisplacement,
    LoadLongDisplacement,
    StoreByteR0Indexed,
    StoreWordR0Indexed,
    StoreLongR0Indexed,
    LoadByteSignedR0Indexed,
    LoadWordSignedR0Indexed,
    LoadLongR0Indexed,
    StoreByteGbrDisplacement,
    StoreWordGbrDisplacement,
    StoreLongGbrDisplacement,
    LoadByteSignedGbrDisplacement,
    LoadWordSignedGbrDisplacement,
    LoadLongGbrDisplacement,
    LoadWordSignedPcRelative,
    LoadLongPcRelative,
    MoveAddressPcRelative,
    StoreSpecialRegister,
    StoreSpecialRegisterPreDecrement,
    LoadSpecialRegister,
    LoadSpecialRegisterPostIncrement,
    TrapAlways,
    ReturnFromException,
    Sleep,
    Branch,
    Call,
    BranchIfTrue,
    BranchIfFalse,
    JumpRegister,
    CallRegister,
    Return
};

struct Instruction {
    std::uint32_t source_address = 0;
    std::uint16_t original_opcode = 0;

    Operation operation = Operation::Unknown;
    OperandWidths widths;
    StatusRegisterEffects status_effects;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::uint8_t branch_register = 0;

    std::int32_t immediate = 0;
    std::int32_t displacement = 0;
    SpecialRegister special_register = SpecialRegister::None;
    std::optional<std::uint32_t> effective_address;
    std::optional<std::uint32_t> target_address;

    bool has_delay_slot = false;
    bool is_delay_slot = false;
    bool is_privileged = false;
};

struct BasicBlock {
    std::uint32_t start_address = 0;
    std::vector<Instruction> instructions;
    std::vector<std::uint32_t> successors;
    bool has_indirect_successor = false;
};

struct Function {
    std::uint32_t entry_address = 0;

    std::vector<BasicBlock> blocks;
    std::vector<std::uint32_t> direct_callees;
    std::vector<std::uint32_t> indirect_call_sites;
};

[[nodiscard]] std::string_view operation_name(
    Operation operation
) noexcept;

[[nodiscard]] OperandWidths operation_operand_widths(Operation operation) noexcept;
[[nodiscard]] std::string_view operand_width_name(OperandWidth width) noexcept;
[[nodiscard]] StatusRegisterEffects instruction_status_effects(
    Operation operation,
    SpecialRegister special_register = SpecialRegister::None
) noexcept;
[[nodiscard]] bool contains_status_bit(
    StatusRegisterBit effects,
    StatusRegisterBit bit
) noexcept;

}
