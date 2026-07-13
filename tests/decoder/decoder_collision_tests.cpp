#include "katana/sh4/decoder_validation.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    using katana::sh4::OpcodeRuleView;
    using katana::sh4::find_decoder_collisions;
    using katana::sh4::opcode_rules_overlap;

    require(
        opcode_rules_overlap(
            OpcodeRuleView{"breit", 0xF000u, 0x6000u},
            OpcodeRuleView{"eng", 0xF00Fu, 0x6003u}
        ),
        "Eine echte Teilmengen-Kollision wurde nicht erkannt."
    );
    require(
        !opcode_rules_overlap(
            OpcodeRuleView{"mov", 0xF00Fu, 0x6003u},
            OpcodeRuleView{"neg", 0xF00Fu, 0x600Bu}
        ),
        "Disjunkte Regeln wurden als Kollision gemeldet."
    );

    const auto collisions = find_decoder_collisions();
    if (!collisions.empty()) {
        const auto& first = collisions.front();
        require(false, "Decoderregeln kollidieren: " + first.first_rule + " / " + first.second_rule + ".");
    }

    std::cout << "KR-1502 Decoder-Kollisionspruefung erfolgreich.\n";
    return EXIT_SUCCESS;
}
