#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace katana::sh4 {

struct OpcodeRuleView {
    std::string_view name;
    std::uint16_t mask;
    std::uint16_t pattern;
};

struct DecoderCollision {
    std::string first_rule;
    std::string second_rule;
    std::uint16_t example_opcode;
};

[[nodiscard]] constexpr bool opcode_rules_overlap(const OpcodeRuleView first,
                                                  const OpcodeRuleView second) noexcept {
    return ((first.pattern ^ second.pattern) & first.mask & second.mask) == 0u;
}

[[nodiscard]] std::vector<DecoderCollision> find_decoder_collisions();

} // namespace katana::sh4
