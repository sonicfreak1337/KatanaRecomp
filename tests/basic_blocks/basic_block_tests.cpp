#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(
    const bool condition,
    const std::string& message
) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    constexpr std::array<std::uint8_t, 20> bytes = {
        0x02, 0x89,
        0x01, 0xE0,
        0x03, 0xA0,
        0x09, 0x00,
        0x01, 0x70,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    const auto blocks =
        katana::analysis::build_basic_blocks(lines);

    require(
        blocks.size() == 4,
        "Es wurden nicht genau vier Basic Blocks erkannt."
    );

    require(
        blocks[0].start_address == 0x8C010000u,
        "Block 0 besitzt eine falsche Startadresse."
    );
    require(
        blocks[0].end_address == 0x8C010000u,
        "Block 0 besitzt eine falsche Endadresse."
    );
    require(
        blocks[0].successors.size() == 2,
        "Block 0 muss zwei Nachfolger besitzen."
    );
    require(
        blocks[0].successors[0] == 0x8C010002u,
        "Der Fallthrough von Block 0 ist falsch."
    );
    require(
        blocks[0].successors[1] == 0x8C010008u,
        "Das Sprungziel von Block 0 ist falsch."
    );

    require(
        blocks[1].start_address == 0x8C010002u,
        "Block 1 besitzt eine falsche Startadresse."
    );
    require(
        blocks[1].end_address == 0x8C010006u,
        "Block 1 muss BRA und Delay Slot enthalten."
    );
    require(
        blocks[1].lines.size() == 3,
        "Block 1 besitzt die falsche Instruktionsanzahl."
    );
    require(
        blocks[1].successors.size() == 1,
        "Block 1 muss genau einen Nachfolger besitzen."
    );
    require(
        blocks[1].successors[0] == 0x8C01000Eu,
        "Das BRA-Ziel von Block 1 ist falsch."
    );

    require(
        blocks[2].start_address == 0x8C010008u,
        "Block 2 besitzt eine falsche Startadresse."
    );
    require(
        blocks[2].end_address == 0x8C01000Cu,
        "Block 2 muss RTS und Delay Slot enthalten."
    );
    require(
        blocks[2].successors.empty(),
        "Ein RTS-Block darf keinen direkten Nachfolger besitzen."
    );

    require(
        blocks[3].start_address == 0x8C01000Eu,
        "Block 3 besitzt eine falsche Startadresse."
    );
    require(
        blocks[3].end_address == 0x8C010012u,
        "Block 3 muss RTS und Delay Slot enthalten."
    );
    require(
        blocks[3].successors.empty(),
        "Der letzte RTS-Block darf keinen Nachfolger besitzen."
    );

    std::cout
        << "Alle Basic-Block- und CFG-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
