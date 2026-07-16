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

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto shll2 = katana::sh4::decode(0x4108u);
    const auto shll8 = katana::sh4::decode(0x4218u);
    const auto shll16 = katana::sh4::decode(0x4328u);
    const auto shlr2 = katana::sh4::decode(0x4409u);
    const auto shlr8 = katana::sh4::decode(0x4519u);
    const auto shlr16 = katana::sh4::decode(0x4629u);

    require(shll2.kind == InstructionKind::ShiftLogicalLeftTwo &&
                shll2.destination_register == 1u && shll2.text == "shll2 r1",
            "SHLL2 wurde falsch dekodiert.");

    require(shll8.kind == InstructionKind::ShiftLogicalLeftEight &&
                shll8.destination_register == 2u && shll8.text == "shll8 r2",
            "SHLL8 wurde falsch dekodiert.");

    require(shll16.kind == InstructionKind::ShiftLogicalLeftSixteen &&
                shll16.destination_register == 3u && shll16.text == "shll16 r3",
            "SHLL16 wurde falsch dekodiert.");

    require(shlr2.kind == InstructionKind::ShiftLogicalRightTwo &&
                shlr2.destination_register == 4u && shlr2.text == "shlr2 r4",
            "SHLR2 wurde falsch dekodiert.");

    require(shlr8.kind == InstructionKind::ShiftLogicalRightEight &&
                shlr8.destination_register == 5u && shlr8.text == "shlr8 r5",
            "SHLR8 wurde falsch dekodiert.");

    require(shlr16.kind == InstructionKind::ShiftLogicalRightSixteen &&
                shlr16.destination_register == 6u && shlr16.text == "shlr16 r6",
            "SHLR16 wurde falsch dekodiert.");

    constexpr std::array<std::uint8_t, 16> bytes = {0x08,
                                                    0x41,
                                                    0x18,
                                                    0x42,
                                                    0x28,
                                                    0x43,
                                                    0x09,
                                                    0x44,
                                                    0x19,
                                                    0x45,
                                                    0x29,
                                                    0x46,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, functions);

    require(program.size() == 1u, "Der KR-1202-Test muss genau eine Funktion besitzen.");

    std::vector<Operation> operations;

    for (const auto& block : program[0].blocks) {
        for (const auto& instruction : block.instructions) {
            operations.push_back(instruction.operation);
        }
    }

    require(operations.size() == 8u, "Der KR-1202-Test besitzt eine falsche IR-Groesse.");

    require(operations[0] == Operation::ShiftLogicalLeftTwo, "SHLL2 wurde falsch in IR abgesenkt.");
    require(operations[1] == Operation::ShiftLogicalLeftEight,
            "SHLL8 wurde falsch in IR abgesenkt.");
    require(operations[2] == Operation::ShiftLogicalLeftSixteen,
            "SHLL16 wurde falsch in IR abgesenkt.");
    require(operations[3] == Operation::ShiftLogicalRightTwo,
            "SHLR2 wurde falsch in IR abgesenkt.");
    require(operations[4] == Operation::ShiftLogicalRightEight,
            "SHLR8 wurde falsch in IR abgesenkt.");
    require(operations[5] == Operation::ShiftLogicalRightSixteen,
            "SHLR16 wurde falsch in IR abgesenkt.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("] <<= 2u;") != std::string::npos &&
                source.find("] <<= 8u;") != std::string::npos &&
                source.find("] <<= 16u;") != std::string::npos,
            "Linke feste Mehrfach-Shifts fehlen im generierten C++.");

    require(source.find("] >>= 2u;") != std::string::npos &&
                source.find("] >>= 8u;") != std::string::npos &&
                source.find("] >>= 16u;") != std::string::npos,
            "Rechte feste Mehrfach-Shifts fehlen im generierten C++.");

    require(source.find("cpu.t =") == std::string::npos,
            "Feste Mehrfach-Shifts duerfen das T-Bit nicht veraendern.");

    std::cout << "Alle KR-1202 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
