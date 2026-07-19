#include "katana/ir/ir.hpp"

namespace katana::ir {
namespace {

constexpr MemoryEffects read(const OperandWidth width) noexcept {
    return {MemoryAccessKind::Read, width, 1u, AddressUpdateKind::None, 0u};
}

constexpr MemoryEffects write(const OperandWidth width) noexcept {
    return {MemoryAccessKind::Write, width, 1u, AddressUpdateKind::None, 0u};
}

constexpr MemoryEffects predecrement_write(const OperandWidth width) noexcept {
    return {MemoryAccessKind::Write, width, 1u, AddressUpdateKind::PreDecrement, 1u};
}

constexpr MemoryEffects postincrement_read(const OperandWidth width) noexcept {
    return {MemoryAccessKind::Read, width, 1u, AddressUpdateKind::PostIncrement, 1u};
}

} // namespace

MemoryEffects instruction_memory_effects(const Operation operation,
                                         const std::uint8_t destination_register,
                                         const std::uint8_t source_register) noexcept {
    switch (operation) {
    case Operation::LoadByteSigned:
    case Operation::LoadByteSignedDisplacement:
    case Operation::LoadByteSignedR0Indexed:
    case Operation::LoadByteSignedGbrDisplacement:
    case Operation::TestByteImmediate:
        return read(OperandWidth::Bits8);
    case Operation::LoadWordSigned:
    case Operation::LoadWordSignedDisplacement:
    case Operation::LoadWordSignedR0Indexed:
    case Operation::LoadWordSignedGbrDisplacement:
    case Operation::LoadWordSignedPcRelative:
        return read(OperandWidth::Bits16);
    case Operation::LoadLong:
    case Operation::LoadLongDisplacement:
    case Operation::LoadLongR0Indexed:
    case Operation::LoadLongGbrDisplacement:
    case Operation::LoadLongPcRelative:
        return read(OperandWidth::Bits32);
    case Operation::FmovLoad:
    case Operation::FmovLoadR0Indexed:
        return {MemoryAccessKind::Read, OperandWidth::Bits64, 2u};

    case Operation::StoreByte:
    case Operation::StoreByteDisplacement:
    case Operation::StoreByteR0Indexed:
    case Operation::StoreByteGbrDisplacement:
        return write(OperandWidth::Bits8);
    case Operation::StoreWord:
    case Operation::StoreWordDisplacement:
    case Operation::StoreWordR0Indexed:
    case Operation::StoreWordGbrDisplacement:
        return write(OperandWidth::Bits16);
    case Operation::StoreLong:
    case Operation::MovcaLong:
    case Operation::StoreLongDisplacement:
    case Operation::StoreLongR0Indexed:
    case Operation::StoreLongGbrDisplacement:
        return write(OperandWidth::Bits32);
    case Operation::FmovStore:
    case Operation::FmovStoreR0Indexed:
        return {MemoryAccessKind::Write, OperandWidth::Bits64, 2u};
    case Operation::AndByteImmediate:
    case Operation::XorByteImmediate:
    case Operation::OrByteImmediate:
    case Operation::TestAndSetByte:
        return {MemoryAccessKind::Write, OperandWidth::Bits8, 2u};

    case Operation::StoreBytePreDecrement:
        return predecrement_write(OperandWidth::Bits8);
    case Operation::StoreWordPreDecrement:
        return predecrement_write(OperandWidth::Bits16);
    case Operation::StoreLongPreDecrement:
        return predecrement_write(OperandWidth::Bits32);
    case Operation::StoreSpecialRegisterPreDecrement:
        return predecrement_write(OperandWidth::Bits32);
    case Operation::FmovStorePreDecrement:
        return {
            MemoryAccessKind::Write, OperandWidth::Bits64, 2u, AddressUpdateKind::PreDecrement, 1u};

    case Operation::LoadByteSignedPostIncrement:
        return destination_register == source_register ? read(OperandWidth::Bits8)
                                                       : postincrement_read(OperandWidth::Bits8);
    case Operation::LoadWordSignedPostIncrement:
        return destination_register == source_register ? read(OperandWidth::Bits16)
                                                       : postincrement_read(OperandWidth::Bits16);
    case Operation::LoadLongPostIncrement:
        return destination_register == source_register ? read(OperandWidth::Bits32)
                                                       : postincrement_read(OperandWidth::Bits32);
    case Operation::LoadSpecialRegisterPostIncrement:
        return postincrement_read(OperandWidth::Bits32);
    case Operation::FmovLoadPostIncrement:
        return {
            MemoryAccessKind::Read, OperandWidth::Bits64, 2u, AddressUpdateKind::PostIncrement, 1u};

    case Operation::MultiplyAccumulateWord:
        return {
            MemoryAccessKind::Read, OperandWidth::Bits16, 2u, AddressUpdateKind::PostIncrement, 2u};
    case Operation::MultiplyAccumulateLong:
        return {
            MemoryAccessKind::Read, OperandWidth::Bits32, 2u, AddressUpdateKind::PostIncrement, 2u};

    default:
        return {};
    }
}

} // namespace katana::ir
