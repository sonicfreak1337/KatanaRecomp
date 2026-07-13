#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto shad = katana::sh4::decode(0x443Cu);
    const auto shld = katana::sh4::decode(0x465Du);

    require(
        shad.kind == InstructionKind::ShiftArithmeticDynamic &&
        shad.source_register == 3u &&
        shad.destination_register == 4u &&
        shad.text == "shad r3, r4",
        "SHAD wurde falsch dekodiert."
    );

    require(
        shld.kind == InstructionKind::ShiftLogicalDynamic &&
        shld.source_register == 5u &&
        shld.destination_register == 6u &&
        shld.text == "shld r5, r6",
        "SHLD wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 8> bytes = {
        0x3C, 0x44,
        0x5D, 0x46,
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
        program.size() == 1u,
        "Der KR-1204-Test muss genau eine Funktion besitzen."
    );

    std::vector<Operation> operations;

    for (const auto& block : program[0].blocks) {
        for (const auto& instruction : block.instructions) {
            operations.push_back(instruction.operation);
        }
    }

    require(
        operations.size() == 4u,
        "Der KR-1204-Test besitzt eine falsche IR-Groesse."
    );

    require(
        operations[0] == Operation::ShiftArithmeticDynamic,
        "SHAD wurde falsch in IR abgesenkt."
    );

    require(
        operations[1] == Operation::ShiftLogicalDynamic,
        "SHLD wurde falsch in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find(
            "((~count) & 0x1Fu) + 1u"
        ) != std::string::npos,
        "Negative dynamische Shiftzaehler fehlen im generierten C++."
    );

    require(
        source.find(
            "amount == 32u ? 0u : value >> amount"
        ) != std::string::npos,
        "SHLD-Sonderfall fuer negative Vielfache von 32 fehlt."
    );

    require(
        source.find(
            "? 0xFFFFFFFFu"
        ) != std::string::npos,
        "SHAD-Sonderfall fuer negative Vielfache von 32 fehlt."
    );

    require(
        source.find("cpu.t =") == std::string::npos,
        "SHAD und SHLD duerfen das T-Bit nicht veraendern."
    );

    std::cout
        << "Alle KR-1204 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
