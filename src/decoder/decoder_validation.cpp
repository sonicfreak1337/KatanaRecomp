#include "katana/sh4/decoder_validation.hpp"

#include "katana/sh4/instruction_metadata.hpp"

#include <vector>

namespace katana::sh4 {
namespace {

struct OwnedRule {
    std::string name;
    std::uint16_t mask;
    std::uint16_t pattern;
};

std::vector<OwnedRule> decoder_rules() {
    std::vector<OwnedRule> rules;
    rules.reserve(instruction_metadata().size() + special_register_encoding_metadata().size());

    for (const auto& metadata : instruction_metadata()) {
        rules.push_back({std::string(metadata.name), metadata.mask, metadata.pattern});
    }
    for (const auto& metadata : special_register_encoding_metadata()) {
        rules.push_back({
            std::string(metadata.mnemonic) + " " + std::string(metadata.special_register_name),
            metadata.mask,
            metadata.pattern
        });
    }
    return rules;
}

}

std::vector<DecoderCollision> find_decoder_collisions() {
    const auto rules = decoder_rules();
    std::vector<DecoderCollision> collisions;

    for (std::size_t first = 0; first < rules.size(); ++first) {
        const auto first_view = OpcodeRuleView{
            rules[first].name,
            rules[first].mask,
            rules[first].pattern
        };
        for (std::size_t second = first + 1u; second < rules.size(); ++second) {
            const auto second_view = OpcodeRuleView{
                rules[second].name,
                rules[second].mask,
                rules[second].pattern
            };
            if (!opcode_rules_overlap(first_view, second_view)) {
                continue;
            }

            collisions.push_back({
                rules[first].name,
                rules[second].name,
                static_cast<std::uint16_t>(rules[first].pattern | rules[second].pattern)
            });
        }
    }
    return collisions;
}

}
