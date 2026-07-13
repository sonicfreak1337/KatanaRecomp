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

    const auto macw = katana::sh4::decode(0x421Fu);
    const auto macl = katana::sh4::decode(0x043Fu);
    const auto clrs = katana::sh4::decode(0x0048u);
    const auto sets = katana::sh4::decode(0x0058u);

    require(
        macw.kind == InstructionKind::MultiplyAccumulateWord &&
        macw.source_register == 1u &&
        macw.destination_register == 2u &&
        macw.text == "mac.w @r1+, @r2+",
        "MAC.W wurde falsch dekodiert."
    );

    require(
        macl.kind == InstructionKind::MultiplyAccumulateLong &&
        macl.source_register == 3u &&
        macl.destination_register == 4u &&
        macl.text == "mac.l @r3+, @r4+",
        "MAC.L wurde falsch dekodiert."
    );

    require(
        clrs.kind == InstructionKind::ClearS &&
        clrs.text == "clrs",
        "CLRS wurde falsch dekodiert."
    );

    require(
        sets.kind == InstructionKind::SetS &&
        sets.text == "sets",
        "SETS wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 56> bytes = {
        0x58, 0x00,
        0x48, 0x00,
        0x08, 0xB0,
        0x09, 0x00,
        0x0A, 0xB0,
        0x09, 0x00,
        0x0C, 0xB0,
        0x09, 0x00,
        0x0E, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x1F, 0x42,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x3F, 0x04,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x5F, 0x45,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x6F, 0x06,
        0x0B, 0x00,
        0x09, 0x00,
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
        program.size() == 5u,
        "Der KR-1303-Test muss Einstieg plus vier MAC-Funktionen besitzen."
    );

    require(
        count_operation(program, Operation::MultiplyAccumulateWord) == 2u,
        "MAC.W wurde nicht genau zweimal in IR abgesenkt."
    );

    require(
        count_operation(program, Operation::MultiplyAccumulateLong) == 2u,
        "MAC.L wurde nicht genau zweimal in IR abgesenkt."
    );

    require(
        count_operation(program, Operation::SetS) == 1u,
        "SETS wurde nicht genau einmal in IR abgesenkt."
    );

    require(
        count_operation(program, Operation::ClearS) == 1u,
        "CLRS wurde nicht genau einmal in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("bool s = false;") != std::string::npos,
        "Das S-Bit fehlt im generierten CPU-Zustand."
    );

    require(
        source.find("cpu.s = true;") != std::string::npos &&
        source.find("cpu.s = false;") != std::string::npos,
        "SETS oder CLRS fehlen im generierten C++."
    );

    require(
        source.find("same_register ? 2u : 0u") != std::string::npos &&
        source.find("same_register ? 4u : 0u") != std::string::npos,
        "Der Sonderfall identischer MAC-Adressregister fehlt."
    );

    require(
        source.find("0x000000007FFFFFFFll") != std::string::npos &&
        source.find("-0x0000000080000000ll") != std::string::npos,
        "Die 32-Bit-Saettigung von MAC.W fehlt."
    );

    require(
        source.find("0x00007FFFFFFFFFFFll") != std::string::npos &&
        source.find("-0x0000800000000000ll") != std::string::npos,
        "Die 48-Bit-Saettigung von MAC.L fehlt."
    );

    std::cout
        << "Alle KR-1303 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
