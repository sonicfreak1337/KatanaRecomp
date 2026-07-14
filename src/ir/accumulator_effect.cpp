#include "katana/ir/ir.hpp"

namespace katana::ir {
namespace {

constexpr AccumulatorRegister both = static_cast<AccumulatorRegister>(
    static_cast<std::uint8_t>(AccumulatorRegister::Mach) |
    static_cast<std::uint8_t>(AccumulatorRegister::Macl)
);

}

AccumulatorEffects operation_accumulator_effects(
    const Operation operation
) noexcept {
    switch (operation) {
        case Operation::MultiplyAccumulateWord:
            return {
                both,
                AccumulatorRegister::Macl,
                both,
                AccumulatorRegister::Macl
            };
        case Operation::MultiplyAccumulateLong:
            return {both, both, both, both};
        default:
            return {};
    }
}

bool contains_accumulator_register(
    const AccumulatorRegister effects,
    const AccumulatorRegister accumulator
) noexcept {
    const auto mask = static_cast<std::uint8_t>(effects);
    const auto requested = static_cast<std::uint8_t>(accumulator);
    return requested != 0u && (mask & requested) == requested;
}

}
