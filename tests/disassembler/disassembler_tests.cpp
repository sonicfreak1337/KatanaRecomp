#include "katana/io/binary_reader.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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
    using katana::sh4::InstructionKind;

    constexpr std::array<std::uint8_t, 8> bytes = {
        0x09, 0x00,
        0xFF, 0xE1,
        0x2C, 0x31,
        0x0B, 0x00
    };

    require(
        katana::io::read_u16_le(bytes, 0) == 0x0009u,
        "Der erste Little-Endian-Opcode ist falsch."
    );

    require(
        katana::io::read_u16_le(bytes, 2) == 0xE1FFu,
        "Der zweite Little-Endian-Opcode ist falsch."
    );

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    require(lines.size() == 4, "Falsche Anzahl Instruktionen.");

    require(
        lines[0].address == 0x8C010000u,
        "Die erste Adresse ist falsch."
    );

    require(
        lines[1].address == 0x8C010002u,
        "Die zweite Adresse ist falsch."
    );

    require(
        lines[2].address == 0x8C010004u,
        "Die dritte Adresse ist falsch."
    );

    require(
        lines[3].address == 0x8C010006u,
        "Die vierte Adresse ist falsch."
    );

    require(
        lines[0].instruction.kind == InstructionKind::Nop,
        "NOP wurde nicht erkannt."
    );

    require(
        lines[1].instruction.kind == InstructionKind::MovImmediate,
        "MOV Immediate wurde nicht erkannt."
    );

    require(
        lines[2].instruction.kind == InstructionKind::AddRegister,
        "ADD Register wurde nicht erkannt."
    );

    require(
        lines[3].instruction.kind == InstructionKind::Rts,
        "RTS wurde nicht erkannt."
    );

    constexpr std::array<std::uint8_t, 3> odd_bytes = {
        0x09, 0x00, 0xFF
    };

    bool odd_size_rejected = false;

    try {
        static_cast<void>(
            katana::sh4::disassemble(odd_bytes)
        );
    } catch (const std::invalid_argument&) {
        odd_size_rejected = true;
    }

    require(
        odd_size_rejected,
        "Eine ungerade Dateigröße wurde nicht abgelehnt."
    );

    std::cout
        << "Alle Binärdatei- und Disassembler-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
