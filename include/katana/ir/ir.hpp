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

    bool operator==(const OperandWidths&) const = default;
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

    bool operator==(const StatusRegisterEffects&) const = default;
};

enum class MemoryAccessKind : std::uint8_t { None, Read, Write };

enum class AddressUpdateKind : std::uint8_t { None, PreDecrement, PostIncrement };

enum class MemoryRegionKind : std::uint8_t { Unknown, NormalRam, Volatile };

struct MemoryEffects {
    MemoryAccessKind access = MemoryAccessKind::None;
    OperandWidth width = OperandWidth::None;
    std::uint8_t access_count = 0u;
    AddressUpdateKind address_update = AddressUpdateKind::None;
    std::uint8_t updated_register_count = 0u;
    MemoryRegionKind region = MemoryRegionKind::Unknown;

    bool operator==(const MemoryEffects&) const = default;
};

enum class AccumulatorRegister : std::uint8_t { None = 0u, Mach = 1u << 0u, Macl = 1u << 1u };

struct AccumulatorEffects {
    AccumulatorRegister reads_if_s_clear = AccumulatorRegister::None;
    AccumulatorRegister reads_if_s_set = AccumulatorRegister::None;
    AccumulatorRegister writes_if_s_clear = AccumulatorRegister::None;
    AccumulatorRegister writes_if_s_set = AccumulatorRegister::None;

    bool operator==(const AccumulatorEffects&) const = default;
};

enum class DelaySlotRole : std::uint8_t { None, Owner, Slot };

struct DelaySlotRelation {
    DelaySlotRole role = DelaySlotRole::None;
    std::optional<std::uint32_t> counterpart_address;

    bool operator==(const DelaySlotRelation&) const = default;
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
    Constant32,
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
    ClearMac,
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
    TestByteImmediate,
    AndByteImmediate,
    XorByteImmediate,
    OrByteImmediate,
    TestAndSetByte,
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
    Prefetch,
    Ocbp,
    Ocbwb,
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
    Fsrra,
    Fsca,
    Fipr,
    Ftrv,
    Fsub,
    Ftrc,
    FcnvDoubleToSingle,
    FcnvSingleToDouble,
    Frchg,
    Fschg,
    Branch,
    Call,
    BranchIfTrue,
    BranchIfFalse,
    JumpRegister,
    CallRegister,
    Return
};

enum class DynamicTargetClass : std::uint8_t {
    NotApplicable,
    GuardedComplete,
    GuardedPartial,
    RuntimeOnly,
    Unresolved
};

struct Instruction {
    std::uint32_t source_address = 0;
    std::uint16_t original_opcode = 0;

    Operation original_operation = Operation::Unknown;
    Operation operation = Operation::Unknown;
    OperandWidths widths;
    StatusRegisterEffects status_effects;
    MemoryEffects memory_effects;
    AccumulatorEffects accumulator_effects;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::uint8_t branch_register = 0;

    std::int32_t immediate = 0;
    std::int32_t displacement = 0;
    SpecialRegister special_register = SpecialRegister::None;
    std::optional<std::uint32_t> effective_address;
    std::optional<std::uint32_t> target_address;
    std::vector<std::uint32_t> resolved_targets;
    std::optional<std::uint8_t> forwarded_value_register;
    DynamicTargetClass dynamic_target_class = DynamicTargetClass::NotApplicable;

    DelaySlotRelation delay_slot;
    bool is_privileged = false;
    bool branch_register_relative = false;
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

[[nodiscard]] std::string_view operation_name(Operation operation) noexcept;

[[nodiscard]] OperandWidths operation_operand_widths(Operation operation) noexcept;
[[nodiscard]] std::string_view operand_width_name(OperandWidth width) noexcept;
[[nodiscard]] StatusRegisterEffects
instruction_status_effects(Operation operation,
                           SpecialRegister special_register = SpecialRegister::None) noexcept;
[[nodiscard]] bool contains_status_bit(StatusRegisterBit effects, StatusRegisterBit bit) noexcept;
[[nodiscard]] MemoryEffects instruction_memory_effects(Operation operation,
                                                       std::uint8_t destination_register = 0u,
                                                       std::uint8_t source_register = 0u) noexcept;
[[nodiscard]] AccumulatorEffects
operation_accumulator_effects(Operation operation,
                              SpecialRegister special_register = SpecialRegister::None) noexcept;
[[nodiscard]] bool contains_accumulator_register(AccumulatorRegister effects,
                                                 AccumulatorRegister accumulator) noexcept;

} // namespace katana::ir
