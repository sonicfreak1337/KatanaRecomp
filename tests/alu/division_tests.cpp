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

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::size_t count_operation(const std::span<const katana::ir::Function> functions,
                            const katana::ir::Operation operation) {
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

} // namespace

int main() {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto div0u = katana::sh4::decode(0x0019u);
    const auto div0s = katana::sh4::decode(0x2437u);
    const auto div1 = katana::sh4::decode(0x3654u);

    require(div0u.kind == InstructionKind::DivideInitializeUnsigned && div0u.text == "div0u",
            "DIV0U wurde falsch dekodiert.");

    require(div0s.kind == InstructionKind::DivideInitializeSigned && div0s.source_register == 3u &&
                div0s.destination_register == 4u && div0s.text == "div0s r3, r4",
            "DIV0S wurde falsch dekodiert.");

    require(div1.kind == InstructionKind::DivideStep && div1.source_register == 5u &&
                div1.destination_register == 6u && div1.text == "div1 r5, r6",
            "DIV1 wurde falsch dekodiert.");

    constexpr std::array<std::uint8_t, 38> bytes = {
        0x06, 0xB0, 0x09, 0x00, 0x08, 0xB0, 0x09, 0x00, 0x0A, 0xB0, 0x09, 0x00, 0x0B,
        0x00, 0x09, 0x00, 0x19, 0x00, 0x0B, 0x00, 0x09, 0x00, 0x09, 0x00, 0x37, 0x24,
        0x0B, 0x00, 0x09, 0x00, 0x09, 0x00, 0x54, 0x36, 0x0B, 0x00, 0x09, 0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, functions);

    require(program.size() == 4u,
            "Der KR-1304-Test muss Einstieg plus drei Divisionsfunktionen besitzen.");

    require(count_operation(program, Operation::DivideInitializeUnsigned) == 1u,
            "DIV0U wurde nicht genau einmal in IR abgesenkt.");

    require(count_operation(program, Operation::DivideInitializeSigned) == 1u,
            "DIV0S wurde nicht genau einmal in IR abgesenkt.");

    require(count_operation(program, Operation::DivideStep) == 1u,
            "DIV1 wurde nicht genau einmal in IR abgesenkt.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("using CpuState = katana::runtime::CpuState;") != std::string::npos,
            "Der generierte Divisionscode bindet den zentralen CPU-Zustand nicht ein.");

    require(source.find("cpu.q = false;") != std::string::npos &&
                source.find("cpu.m = false;") != std::string::npos,
            "DIV0U fehlt im generierten C++.");

    require(source.find("cpu.t = cpu.m != cpu.q;") != std::string::npos,
            "DIV0S setzt T nicht als M XOR Q.");

    require(source.find("const bool old_q = cpu.q;") != std::string::npos &&
                source.find("cpu.t = cpu.q == cpu.m;") != std::string::npos,
            "DIV1 besitzt nicht die erwartete Q-/M-/T-Semantik.");

    require(source.find("const bool carry =") != std::string::npos &&
                source.find("const bool borrow =") != std::string::npos,
            "DIV1 behandelt Carry oder Borrow nicht explizit.");

    std::cout << "Alle KR-1304 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
