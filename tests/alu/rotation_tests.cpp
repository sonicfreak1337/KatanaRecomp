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

    const auto rotl = katana::sh4::decode(0x4104u);
    const auto rotr = katana::sh4::decode(0x4205u);
    const auto rotcl = katana::sh4::decode(0x4324u);
    const auto rotcr = katana::sh4::decode(0x4425u);

    require(
        rotl.kind == InstructionKind::RotateLeft &&
        rotl.destination_register == 1u &&
        rotl.text == "rotl r1",
        "ROTL wurde falsch dekodiert."
    );

    require(
        rotr.kind == InstructionKind::RotateRight &&
        rotr.destination_register == 2u &&
        rotr.text == "rotr r2",
        "ROTR wurde falsch dekodiert."
    );

    require(
        rotcl.kind == InstructionKind::RotateLeftThroughT &&
        rotcl.destination_register == 3u &&
        rotcl.text == "rotcl r3",
        "ROTCL wurde falsch dekodiert."
    );

    require(
        rotcr.kind == InstructionKind::RotateRightThroughT &&
        rotcr.destination_register == 4u &&
        rotcr.text == "rotcr r4",
        "ROTCR wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 12> bytes = {
        0x04, 0x41,
        0x05, 0x42,
        0x24, 0x43,
        0x25, 0x44,
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
        "Der KR-1203-Test muss genau eine Funktion besitzen."
    );

    std::vector<Operation> operations;

    for (const auto& block : program[0].blocks) {
        for (const auto& instruction : block.instructions) {
            operations.push_back(instruction.operation);
        }
    }

    require(
        operations.size() == 6u,
        "Der KR-1203-Test besitzt eine falsche IR-Groesse."
    );

    require(
        operations[0] == Operation::RotateLeft,
        "ROTL wurde falsch in IR abgesenkt."
    );
    require(
        operations[1] == Operation::RotateRight,
        "ROTR wurde falsch in IR abgesenkt."
    );
    require(
        operations[2] == Operation::RotateLeftThroughT,
        "ROTCL wurde falsch in IR abgesenkt."
    );
    require(
        operations[3] == Operation::RotateRightThroughT,
        "ROTCR wurde falsch in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find(
            "(value << 1u) | (value >> 31u)"
        ) != std::string::npos,
        "ROTL fehlt im generierten C++."
    );

    require(
        source.find(
            "(value >> 1u) | (value << 31u)"
        ) != std::string::npos,
        "ROTR fehlt im generierten C++."
    );

    require(
        source.find(
            "(value << 1u) | (old_t ? 1u : 0u)"
        ) != std::string::npos,
        "ROTCL fehlt im generierten C++."
    );

    require(
        source.find(
            "(old_t ? 0x80000000u : 0u)"
        ) != std::string::npos,
        "ROTCR fehlt im generierten C++."
    );

    std::cout
        << "Alle KR-1203 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
