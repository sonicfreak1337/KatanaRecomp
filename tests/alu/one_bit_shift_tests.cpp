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

    const auto shll = katana::sh4::decode(0x4300u);
    const auto shlr = katana::sh4::decode(0x4401u);
    const auto shal = katana::sh4::decode(0x4520u);
    const auto shar = katana::sh4::decode(0x4621u);

    require(
        shll.kind == InstructionKind::ShiftLogicalLeftOne &&
        shll.destination_register == 3u &&
        shll.text == "shll r3",
        "SHLL wurde falsch dekodiert."
    );

    require(
        shlr.kind == InstructionKind::ShiftLogicalRightOne &&
        shlr.destination_register == 4u &&
        shlr.text == "shlr r4",
        "SHLR wurde falsch dekodiert."
    );

    require(
        shal.kind == InstructionKind::ShiftArithmeticLeftOne &&
        shal.destination_register == 5u &&
        shal.text == "shal r5",
        "SHAL wurde falsch dekodiert."
    );

    require(
        shar.kind == InstructionKind::ShiftArithmeticRightOne &&
        shar.destination_register == 6u &&
        shar.text == "shar r6",
        "SHAR wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 12> bytes = {
        0x00, 0x43,
        0x01, 0x44,
        0x20, 0x45,
        0x21, 0x46,
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
        "Der KR-1201-Test muss genau eine Funktion besitzen."
    );

    std::vector<Operation> operations;

    for (const auto& block : program[0].blocks) {
        for (const auto& instruction : block.instructions) {
            operations.push_back(instruction.operation);
        }
    }

    require(
        operations.size() == 6u,
        "Der KR-1201-Test besitzt eine falsche IR-Groesse."
    );

    require(
        operations[0] == Operation::ShiftLogicalLeftOne,
        "SHLL wurde falsch in IR abgesenkt."
    );

    require(
        operations[1] == Operation::ShiftLogicalRightOne,
        "SHLR wurde falsch in IR abgesenkt."
    );

    require(
        operations[2] == Operation::ShiftArithmeticLeftOne,
        "SHAL wurde falsch in IR abgesenkt."
    );

    require(
        operations[3] == Operation::ShiftArithmeticRightOne,
        "SHAR wurde falsch in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find(
            "cpu.t = (value & 0x80000000u) != 0u;"
        ) != std::string::npos,
        "Linksshift muss das alte Bit 31 nach T uebernehmen."
    );

    require(
        source.find(
            "cpu.t = (value & 0x00000001u) != 0u;"
        ) != std::string::npos,
        "Rechtsshift muss das alte Bit 0 nach T uebernehmen."
    );

    require(
        source.find(
            "(value & 0x80000000u);"
        ) != std::string::npos,
        "SHAR muss das Vorzeichenbit erhalten."
    );

    std::cout
        << "Alle KR-1201 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
