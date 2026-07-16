#include "katana/ir/ir.hpp"

namespace katana::ir {
namespace {

constexpr AccumulatorRegister both =
    static_cast<AccumulatorRegister>(static_cast<std::uint8_t>(AccumulatorRegister::Mach) |
                                     static_cast<std::uint8_t>(AccumulatorRegister::Macl));

}

AccumulatorEffects operation_accumulator_effects(const Operation operation,
                                                 const SpecialRegister special_register) noexcept {
    switch (operation) {
    case Operation::ClearMac:
        return {AccumulatorRegister::None, AccumulatorRegister::None, both, both};
    case Operation::MultiplyLong:
    case Operation::MultiplySignedWord:
    case Operation::MultiplyUnsignedWord:
        return {AccumulatorRegister::None,
                AccumulatorRegister::None,
                AccumulatorRegister::Macl,
                AccumulatorRegister::Macl};
    case Operation::DoubleMultiplySignedLong:
    case Operation::DoubleMultiplyUnsignedLong:
        return {AccumulatorRegister::None, AccumulatorRegister::None, both, both};
    case Operation::MultiplyAccumulateWord:
        return {both, AccumulatorRegister::Macl, both, AccumulatorRegister::Macl};
    case Operation::MultiplyAccumulateLong:
        return {both, both, both, both};
    case Operation::StoreSpecialRegister:
    case Operation::StoreSpecialRegisterPreDecrement:
        if (special_register == SpecialRegister::Mach) {
            return {AccumulatorRegister::Mach,
                    AccumulatorRegister::Mach,
                    AccumulatorRegister::None,
                    AccumulatorRegister::None};
        }
        if (special_register == SpecialRegister::Macl) {
            return {AccumulatorRegister::Macl,
                    AccumulatorRegister::Macl,
                    AccumulatorRegister::None,
                    AccumulatorRegister::None};
        }
        return {};
    case Operation::LoadSpecialRegister:
    case Operation::LoadSpecialRegisterPostIncrement:
        if (special_register == SpecialRegister::Mach) {
            return {AccumulatorRegister::None,
                    AccumulatorRegister::None,
                    AccumulatorRegister::Mach,
                    AccumulatorRegister::Mach};
        }
        if (special_register == SpecialRegister::Macl) {
            return {AccumulatorRegister::None,
                    AccumulatorRegister::None,
                    AccumulatorRegister::Macl,
                    AccumulatorRegister::Macl};
        }
        return {};
    default:
        return {};
    }
}

bool contains_accumulator_register(const AccumulatorRegister effects,
                                   const AccumulatorRegister accumulator) noexcept {
    const auto mask = static_cast<std::uint8_t>(effects);
    const auto requested = static_cast<std::uint8_t>(accumulator);
    return requested != 0u && (mask & requested) == requested;
}

} // namespace katana::ir
