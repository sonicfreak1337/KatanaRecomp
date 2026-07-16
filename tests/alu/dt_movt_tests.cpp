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

    const auto dt = katana::sh4::decode(0x4310u);
    const auto movt = katana::sh4::decode(0x0529u);

    require(dt.kind == InstructionKind::DecrementAndTest && dt.destination_register == 3u &&
                dt.text == "dt r3",
            "DT wurde falsch dekodiert.");

    require(movt.kind == InstructionKind::MoveT && movt.destination_register == 5u &&
                movt.text == "movt r5",
            "MOVT wurde falsch dekodiert.");

    constexpr std::array<std::uint8_t, 10> bytes = {
        0x10, 0x43, 0x29, 0x05, 0x10, 0x44, 0x0B, 0x00, 0x09, 0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, functions);

    require(program.size() == 1u, "Der KR-1106-Test muss genau eine Funktion besitzen.");

    const auto& instructions = program[0].blocks[0].instructions;

    require(instructions.size() == 5u, "Der KR-1106-IR-Block besitzt eine falsche Groesse.");

    require(instructions[0].operation == Operation::DecrementAndTest,
            "DT wurde falsch in IR abgesenkt.");

    require(instructions[1].operation == Operation::MoveT, "MOVT wurde falsch in IR abgesenkt.");

    require(instructions[2].operation == Operation::DecrementAndTest,
            "Das zweite DT wurde falsch in IR abgesenkt.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("] - 1u;") != std::string::npos &&
                source.find("cpu.t = result == 0u;") != std::string::npos,
            "DT-Semantik fehlt im generierten C++.");

    require(source.find("] = cpu.t ? 1u : 0u;") != std::string::npos,
            "MOVT-Semantik fehlt im generierten C++.");

    std::cout << "Alle KR-1106 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
