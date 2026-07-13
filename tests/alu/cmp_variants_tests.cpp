#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
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
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto hs = katana::sh4::decode(0x3122u);
    const auto ge = katana::sh4::decode(0x3123u);
    const auto hi = katana::sh4::decode(0x3126u);
    const auto gt = katana::sh4::decode(0x3127u);
    const auto pz = katana::sh4::decode(0x4311u);
    const auto pl = katana::sh4::decode(0x4415u);
    const auto str = katana::sh4::decode(0x212Cu);

    require(
        hs.kind == InstructionKind::CompareHigherOrSame &&
        hs.text == "cmp/hs r2, r1",
        "CMP/HS wurde falsch dekodiert."
    );

    require(
        ge.kind == InstructionKind::CompareGreaterOrEqual &&
        ge.text == "cmp/ge r2, r1",
        "CMP/GE wurde falsch dekodiert."
    );

    require(
        hi.kind == InstructionKind::CompareHigher &&
        hi.text == "cmp/hi r2, r1",
        "CMP/HI wurde falsch dekodiert."
    );

    require(
        gt.kind == InstructionKind::CompareGreaterThan &&
        gt.text == "cmp/gt r2, r1",
        "CMP/GT wurde falsch dekodiert."
    );

    require(
        pz.kind == InstructionKind::ComparePositiveOrZero &&
        pz.destination_register == 3u &&
        pz.text == "cmp/pz r3",
        "CMP/PZ wurde falsch dekodiert."
    );

    require(
        pl.kind == InstructionKind::ComparePositive &&
        pl.destination_register == 4u &&
        pl.text == "cmp/pl r4",
        "CMP/PL wurde falsch dekodiert."
    );

    require(
        str.kind == InstructionKind::CompareString &&
        str.destination_register == 1u &&
        str.source_register == 2u &&
        str.text == "cmp/str r2, r1",
        "CMP/STR wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 18> bytes = {
        0x22, 0x31,
        0x23, 0x31,
        0x26, 0x31,
        0x27, 0x31,
        0x11, 0x43,
        0x15, 0x44,
        0x2C, 0x21,
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
        "Der KR-1103-Test muss genau eine Funktion besitzen."
    );

    const auto& instructions =
        program[0].blocks[0].instructions;

    require(
        instructions.size() == 9u,
        "Der KR-1103-IR-Block besitzt eine falsche Groesse."
    );

    require(
        instructions[0].operation == Operation::CompareHigherOrSame,
        "CMP/HS wurde falsch in IR abgesenkt."
    );
    require(
        instructions[1].operation == Operation::CompareGreaterOrEqual,
        "CMP/GE wurde falsch in IR abgesenkt."
    );
    require(
        instructions[2].operation == Operation::CompareHigher,
        "CMP/HI wurde falsch in IR abgesenkt."
    );
    require(
        instructions[3].operation == Operation::CompareGreaterThan,
        "CMP/GT wurde falsch in IR abgesenkt."
    );
    require(
        instructions[4].operation == Operation::ComparePositiveOrZero,
        "CMP/PZ wurde falsch in IR abgesenkt."
    );
    require(
        instructions[5].operation == Operation::ComparePositive,
        "CMP/PL wurde falsch in IR abgesenkt."
    );
    require(
        instructions[6].operation == Operation::CompareString,
        "CMP/STR wurde falsch in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("cpu.t = cpu.r[1] >= cpu.r[2];") !=
            std::string::npos,
        "CMP/HS fehlt im generierten C++."
    );

    require(
        source.find(
            "cpu.t = (cpu.r[1] ^ 0x80000000u) >= "
            "(cpu.r[2] ^ 0x80000000u);"
        ) != std::string::npos,
        "CMP/GE fehlt im generierten C++."
    );

    require(
        source.find("cpu.t = cpu.r[1] > cpu.r[2];") !=
            std::string::npos,
        "CMP/HI fehlt im generierten C++."
    );

    require(
        source.find(
            "cpu.t = (cpu.r[1] ^ 0x80000000u) > "
            "(cpu.r[2] ^ 0x80000000u);"
        ) != std::string::npos,
        "CMP/GT fehlt im generierten C++."
    );

    require(
        source.find(
            "cpu.t = (cpu.r[3] & 0x80000000u) == 0u;"
        ) != std::string::npos,
        "CMP/PZ fehlt im generierten C++."
    );

    require(
        source.find(
            "cpu.t = cpu.r[4] != 0u && "
            "(cpu.r[4] & 0x80000000u) == 0u;"
        ) != std::string::npos,
        "CMP/PL fehlt im generierten C++."
    );

    require(
        source.find("0x000000FFu") != std::string::npos &&
        source.find("0xFF000000u") != std::string::npos,
        "CMP/STR-Bytevergleich fehlt im generierten C++."
    );

    std::cout
        << "Alle KR-1103 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
