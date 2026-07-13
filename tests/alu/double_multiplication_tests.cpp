#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
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

std::size_t count_operation(
    const std::span<const katana::ir::Function> functions,
    const katana::ir::Operation operation
) {
    std::size_t count = 0;

    for (const auto& function : functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.operation == operation) {
                    ++count;
                }
            }
        }
    }

    return count;
}

}

int main() {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto dmulu = katana::sh4::decode(0x3215u);
    const auto dmuls = katana::sh4::decode(0x343Du);

    require(
        dmulu.kind == InstructionKind::DoubleMultiplyUnsignedLong &&
        dmulu.source_register == 1u &&
        dmulu.destination_register == 2u &&
        dmulu.text == "dmulu.l r1, r2",
        "DMULU.L wurde falsch dekodiert."
    );

    require(
        dmuls.kind == InstructionKind::DoubleMultiplySignedLong &&
        dmuls.source_register == 3u &&
        dmuls.destination_register == 4u &&
        dmuls.text == "dmuls.l r3, r4",
        "DMULS.L wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 30> bytes = {
        0x06, 0xB0,
        0x09, 0x00,
        0x08, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x15, 0x32,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x3D, 0x34,
        0x0B, 0x00,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    constexpr std::array<std::uint32_t, 1> seeds = {
        0x8C010000u
    };

    const auto functions =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    const auto program =
        katana::ir::lower_program(
            lines,
            functions
        );

    require(
        program.size() == 3u,
        "Der KR-1302-Test muss Einstieg plus zwei Multiplikationsfunktionen besitzen."
    );

    require(
        count_operation(
            program,
            Operation::DoubleMultiplyUnsignedLong
        ) == 1u,
        "DMULU.L wurde nicht genau einmal in IR abgesenkt."
    );

    require(
        count_operation(
            program,
            Operation::DoubleMultiplySignedLong
        ) == 1u,
        "DMULS.L wurde nicht genau einmal in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("std::uint32_t mach = 0;") != std::string::npos,
        "MACH fehlt im generierten CPU-Zustand."
    );

    require(
        source.find(
            "cpu.mach = static_cast<std::uint32_t>(product >> 32u);"
        ) != std::string::npos,
        "Die oberen 32 Produktbits werden nicht nach MACH geschrieben."
    );

    require(
        source.find(
            "static_cast<std::int64_t>(source_raw) - 0x100000000ll"
        ) != std::string::npos,
        "DMULS.L besitzt keine portable 32-Bit-Vorzeichenerweiterung."
    );

    require(
        source.find(
            "static_cast<std::uint64_t>(signed_product)"
        ) != std::string::npos,
        "Die signed Produktbits werden nicht definiert nach MACH:MACL uebertragen."
    );

    require(
        source.find("cpu.t =") == std::string::npos,
        "Doppelte Multiplikation darf das T-Bit nicht veraendern."
    );

    std::cout
        << "Alle KR-1302 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
