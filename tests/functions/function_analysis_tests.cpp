#include "katana/analysis/function_analysis.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 16> bytes = {0x02,
                                                    0xB0,
                                                    0x09,
                                                    0x00,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00,
                                                    0x0B,
                                                    0x43,
                                                    0x09,
                                                    0x00,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    require(functions.size() == 2, "Es wurden nicht genau zwei Funktionen erkannt.");

    const auto& main_function = functions[0];
    const auto& sub_function = functions[1];

    require(main_function.entry_address == 0x8C010000u,
            "Die erste Funktion besitzt die falsche Startadresse.");
    require(main_function.block_addresses.size() == 2,
            "Die erste Funktion muss zwei Basic Blocks besitzen.");
    require(main_function.block_addresses[0] == 0x8C010000u,
            "Der erste Block der Hauptfunktion ist falsch.");
    require(main_function.block_addresses[1] == 0x8C010004u,
            "Der zweite Block der Hauptfunktion ist falsch.");
    require(main_function.direct_callees.size() == 1,
            "Die Hauptfunktion muss genau einen direkten Aufruf besitzen.");
    require(main_function.direct_callees[0] == 0x8C010008u,
            "Das direkte Aufrufziel der Hauptfunktion ist falsch.");
    require(main_function.indirect_call_sites.empty(),
            "Die Hauptfunktion darf keinen indirekten Aufruf besitzen.");

    require(sub_function.entry_address == 0x8C010008u,
            "Die zweite Funktion besitzt die falsche Startadresse.");
    require(sub_function.block_addresses.size() == 2,
            "Die zweite Funktion muss zwei Basic Blocks besitzen.");
    require(sub_function.block_addresses[0] == 0x8C010008u,
            "Der erste Block der Unterfunktion ist falsch.");
    require(sub_function.block_addresses[1] == 0x8C01000Cu,
            "Der zweite Block der Unterfunktion ist falsch.");
    require(sub_function.direct_callees.empty(),
            "Die Unterfunktion darf keinen direkten Aufruf besitzen.");
    require(sub_function.indirect_call_sites.size() == 1,
            "Die Unterfunktion muss einen indirekten Aufruf besitzen.");
    require(sub_function.indirect_call_sites[0] == 0x8C010008u,
            "Die Adresse des indirekten Aufrufs ist falsch.");

    constexpr std::size_t scaling_block_count = 4096u;
    constexpr std::uint32_t scaling_base = 0x8C100000u;
    constexpr std::array<std::uint8_t, 2u> nop_bytes = {0x09u, 0x00u};
    const auto nop = katana::sh4::disassemble(nop_bytes, scaling_base).front();
    std::vector<katana::analysis::BasicBlock> scaling_blocks(scaling_block_count);
    for (std::size_t index = 0u; index < scaling_blocks.size(); ++index) {
        const auto address = scaling_base + static_cast<std::uint32_t>(index * 2u);
        auto& block = scaling_blocks[index];
        block.id = index;
        block.start_address = address;
        block.end_address = address;
        block.lines.push_back(nop);
        block.lines.back().address = address;
        if (index + 1u < scaling_blocks.size()) block.successors.push_back(address + 2u);
    }
    constexpr std::array<std::uint32_t, 1u> scaling_seeds = {scaling_base};
    const auto scaling_start = std::chrono::steady_clock::now();
    const auto scaling_functions =
        katana::analysis::discover_functions_from_blocks(scaling_blocks, scaling_seeds);
    const auto scaling_elapsed = std::chrono::steady_clock::now() - scaling_start;
    require(scaling_functions.size() == 1u &&
                scaling_functions.front().block_addresses.size() == scaling_block_count &&
                scaling_functions.front().block_addresses.back() ==
                    scaling_base + static_cast<std::uint32_t>((scaling_block_count - 1u) * 2u),
            "Die gebuendelte Funktionsanalyse verlor einen Block der langen CFG-Kette.");
    require(scaling_elapsed < std::chrono::seconds(5),
            "Die Funktionsanalyse sortiert offenbar weiterhin nach jedem CFG-Block.");

    std::cout << "Alle Funktionsanalyse-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
