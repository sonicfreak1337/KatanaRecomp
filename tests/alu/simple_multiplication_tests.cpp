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

    const auto mul_l = katana::sh4::decode(0x0217u);
    const auto muls_w = katana::sh4::decode(0x243Fu);
    const auto mulu_w = katana::sh4::decode(0x265Eu);

    require(
        mul_l.kind == InstructionKind::MultiplyLong &&
        mul_l.source_register == 1u &&
        mul_l.destination_register == 2u &&
        mul_l.text == "mul.l r1, r2",
        "MUL.L wurde falsch dekodiert."
    );

    require(
        muls_w.kind == InstructionKind::MultiplySignedWord &&
        muls_w.source_register == 3u &&
        muls_w.destination_register == 4u &&
        muls_w.text == "muls.w r3, r4",
        "MULS.W wurde falsch dekodiert."
    );

    require(
        mulu_w.kind == InstructionKind::MultiplyUnsignedWord &&
        mulu_w.source_register == 5u &&
        mulu_w.destination_register == 6u &&
        mulu_w.text == "mulu.w r5, r6",
        "MULU.W wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 38> bytes = {
        0x06, 0xB0,
        0x09, 0x00,
        0x08, 0xB0,
        0x09, 0x00,
        0x0A, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x17, 0x02,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x3F, 0x24,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x5E, 0x26,
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
        program.size() == 4u,
        "Der KR-1301-Test muss Einstieg plus drei Multiplikationsfunktionen besitzen."
    );

    require(
        count_operation(program, Operation::MultiplyLong) == 1u,
        "MUL.L wurde nicht genau einmal in IR abgesenkt."
    );

    require(
        count_operation(program, Operation::MultiplySignedWord) == 1u,
        "MULS.W wurde nicht genau einmal in IR abgesenkt."
    );

    require(
        count_operation(program, Operation::MultiplyUnsignedWord) == 1u,
        "MULU.W wurde nicht genau einmal in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("std::uint32_t macl = 0;") != std::string::npos,
        "MACL fehlt im generierten CPU-Zustand."
    );

    require(
        source.find(
            "const std::uint64_t product ="
        ) != std::string::npos,
        "MUL.L fehlt im generierten C++."
    );

    require(
        source.find(
            "static_cast<std::int32_t>(source_raw) - 0x00010000"
        ) != std::string::npos,
        "MULS.W besitzt keine portable Vorzeichenerweiterung."
    );

    require(
        source.find(
            "& 0x0000FFFFu) *"
        ) != std::string::npos,
        "MULU.W fehlt im generierten C++."
    );

    require(
        source.find("cpu.t =") == std::string::npos,
        "Multiplikationsinstruktionen duerfen das T-Bit nicht veraendern."
    );

    std::cout
        << "Alle KR-1301 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
