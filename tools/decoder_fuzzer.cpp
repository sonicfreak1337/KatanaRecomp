#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction_metadata.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::uint32_t next_random(std::uint32_t& state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

bool same_instruction(
    const katana::sh4::DecodedInstruction& first,
    const katana::sh4::DecodedInstruction& second
) {
    return first.opcode == second.opcode &&
        first.kind == second.kind &&
        first.destination_register == second.destination_register &&
        first.source_register == second.source_register &&
        first.branch_register == second.branch_register &&
        first.immediate == second.immediate &&
        first.displacement == second.displacement &&
        first.special_register == second.special_register &&
        first.control_flow == second.control_flow &&
        first.has_delay_slot == second.has_delay_slot &&
        first.is_privileged == second.is_privileged &&
        first.text == second.text;
}

std::size_t matching_rule_count(
    const std::uint16_t opcode,
    katana::sh4::InstructionKind& matched_kind
) {
    std::size_t count = 0;
    for (const auto& metadata : katana::sh4::instruction_metadata()) {
        if (metadata.matches(opcode)) {
            matched_kind = metadata.kind;
            ++count;
        }
    }
    for (const auto& metadata : katana::sh4::special_register_encoding_metadata()) {
        if (metadata.matches(opcode)) {
            matched_kind = metadata.kind;
            ++count;
        }
    }
    return count;
}

int fail(const std::uint16_t opcode, const std::string& reason) {
    std::cerr << "FUZZ-FEHLER bei Opcode " << opcode << ": " << reason << '\n';
    return EXIT_FAILURE;
}

}

int main(const int argc, char* argv[]) {
    try {
        std::uint32_t seed = 0x00C0FFEEu;
        std::uint32_t iterations = 200000u;
        if (argc >= 2) {
            seed = static_cast<std::uint32_t>(std::stoul(argv[1], nullptr, 0));
        }
        if (argc >= 3) {
            iterations = static_cast<std::uint32_t>(std::stoul(argv[2], nullptr, 0));
        }
        if (seed == 0u || iterations == 0u) {
            std::cerr << "Seed und Iterationszahl muessen groesser als null sein.\n";
            return 2;
        }

        std::vector<std::uint16_t> corpus = {0x0000u, 0xFFFFu, 0xF000u, 0x407Au};
        for (const auto& metadata : katana::sh4::instruction_metadata()) {
            corpus.push_back(metadata.pattern);
        }
        for (const auto& metadata : katana::sh4::special_register_encoding_metadata()) {
            corpus.push_back(metadata.pattern);
        }

        auto random_state = seed;
        std::uint32_t known = 0;
        std::uint32_t unknown = 0;
        for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
            const auto random = next_random(random_state);
            auto opcode = corpus[iteration % corpus.size()];
            if ((iteration % 3u) == 0u) {
                opcode ^= static_cast<std::uint16_t>(1u << (random & 15u));
            } else if ((iteration % 3u) == 1u) {
                opcode = static_cast<std::uint16_t>(random);
            } else {
                opcode ^= static_cast<std::uint16_t>(random);
            }

            const auto first = katana::sh4::decode(opcode);
            const auto second = katana::sh4::decode(opcode);
            if (!same_instruction(first, second)) {
                return fail(opcode, "Decoder ist nicht deterministisch.");
            }
            if (first.text.empty()) {
                return fail(opcode, "Disassembly ist leer.");
            }
            if (first.destination_register > 15u || first.source_register > 15u || first.branch_register > 15u) {
                return fail(opcode, "Ein Registeroperand liegt ausserhalb R0 bis R15.");
            }

            auto matched_kind = katana::sh4::InstructionKind::Unknown;
            const auto matches = matching_rule_count(opcode, matched_kind);
            if (first.is_known()) {
                ++known;
                if (matches != 1u) {
                    return fail(opcode, "Ein bekannter Opcode besitzt nicht genau eine Metadatenregel.");
                }
                if (matched_kind != first.kind) {
                    return fail(opcode, "Metadatenregel und Decoder liefern verschiedene Instruktionsarten.");
                }
            } else {
                ++unknown;
                if (matches != 0u || first.kind != katana::sh4::InstructionKind::Unknown) {
                    return fail(opcode, "Unknown-Opcode besitzt eine Regel oder einen bekannten Kind-Wert.");
                }
            }
        }

        if (known == 0u || unknown == 0u) {
            std::cerr << "Der Fuzz-Lauf hat bekannte oder unbekannte Opcodes nicht erreicht.\n";
            return EXIT_FAILURE;
        }

        std::cout << "KR-1505 Decoder-Fuzzer erfolgreich: Seed=" << seed
                  << ", Iterationen=" << iterations
                  << ", bekannt=" << known
                  << ", unbekannt=" << unknown << "\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Fuzzer-Argumentfehler: " << error.what() << '\n';
        return 2;
    }
}
