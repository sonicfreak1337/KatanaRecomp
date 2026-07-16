#include "katana/ir/ir.hpp"

namespace katana::ir {
namespace {

constexpr StatusRegisterBit combine(const StatusRegisterBit left,
                                    const StatusRegisterBit right) noexcept {
    return static_cast<StatusRegisterBit>(static_cast<std::uint8_t>(left) |
                                          static_cast<std::uint8_t>(right));
}

constexpr StatusRegisterBit t = StatusRegisterBit::T;
constexpr StatusRegisterBit s = StatusRegisterBit::S;
constexpr StatusRegisterBit q = StatusRegisterBit::Q;
constexpr StatusRegisterBit m = StatusRegisterBit::M;
constexpr StatusRegisterBit full = StatusRegisterBit::Full;
constexpr StatusRegisterBit tqm = combine(combine(t, q), m);

} // namespace

StatusRegisterEffects instruction_status_effects(const Operation operation,
                                                 const SpecialRegister special_register) noexcept {
    switch (operation) {
    case Operation::MoveT:
    case Operation::BranchIfTrue:
    case Operation::BranchIfFalse:
        return {t, StatusRegisterBit::None};

    case Operation::AddWithCarry:
    case Operation::SubWithCarry:
    case Operation::NegateWithCarry:
    case Operation::RotateLeftThroughT:
    case Operation::RotateRightThroughT:
        return {t, t};

    case Operation::AddWithOverflow:
    case Operation::SubWithOverflow:
    case Operation::DecrementAndTest:
    case Operation::ShiftLogicalLeftOne:
    case Operation::ShiftLogicalRightOne:
    case Operation::ShiftArithmeticLeftOne:
    case Operation::ShiftArithmeticRightOne:
    case Operation::RotateLeft:
    case Operation::RotateRight:
    case Operation::ClearT:
    case Operation::SetT:
    case Operation::CompareEqualImmediate:
    case Operation::CompareEqualRegister:
    case Operation::CompareHigherOrSame:
    case Operation::CompareGreaterOrEqual:
    case Operation::CompareHigher:
    case Operation::CompareGreaterThan:
    case Operation::ComparePositiveOrZero:
    case Operation::ComparePositive:
    case Operation::CompareString:
    case Operation::TestImmediate:
    case Operation::TestRegister:
    case Operation::TestByteImmediate:
    case Operation::TestAndSetByte:
    case Operation::FcmpEqual:
    case Operation::FcmpGreater:
        return {StatusRegisterBit::None, t};

    case Operation::ClearS:
    case Operation::SetS:
        return {StatusRegisterBit::None, s};

    case Operation::MultiplyAccumulateWord:
    case Operation::MultiplyAccumulateLong:
        return {s, StatusRegisterBit::None};

    case Operation::DivideInitializeUnsigned:
    case Operation::DivideInitializeSigned:
        return {StatusRegisterBit::None, tqm};

    case Operation::DivideStep:
        return {tqm, combine(t, q)};

    case Operation::TrapAlways:
        return {full, full};
    case Operation::ReturnFromException:
        return {StatusRegisterBit::None, full};

    case Operation::StoreSpecialRegister:
    case Operation::StoreSpecialRegisterPreDecrement:
        return special_register == SpecialRegister::Sr
                   ? StatusRegisterEffects{full, StatusRegisterBit::None}
                   : StatusRegisterEffects{};
    case Operation::LoadSpecialRegister:
    case Operation::LoadSpecialRegisterPostIncrement:
        return special_register == SpecialRegister::Sr
                   ? StatusRegisterEffects{StatusRegisterBit::None, full}
                   : StatusRegisterEffects{};

    default:
        return {};
    }
}

bool contains_status_bit(const StatusRegisterBit effects, const StatusRegisterBit bit) noexcept {
    const auto mask = static_cast<std::uint8_t>(effects);
    const auto requested = static_cast<std::uint8_t>(bit);
    const auto full_mask = static_cast<std::uint8_t>(StatusRegisterBit::Full);
    const auto named_bits = static_cast<std::uint8_t>(StatusRegisterBit::T) |
                            static_cast<std::uint8_t>(StatusRegisterBit::S) |
                            static_cast<std::uint8_t>(StatusRegisterBit::Q) |
                            static_cast<std::uint8_t>(StatusRegisterBit::M);
    if ((mask & full_mask) != 0u && requested != 0u &&
        (requested & static_cast<std::uint8_t>(~named_bits)) == 0u) {
        return true;
    }
    return requested != 0u && (mask & requested) == requested;
}

} // namespace katana::ir
