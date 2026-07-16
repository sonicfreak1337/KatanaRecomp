#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
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

    const auto sub = katana::sh4::decode(0x3128u);

    require(sub.kind == InstructionKind::SubRegister, "SUB wurde nicht erkannt.");
    require(sub.destination_register == 1u && sub.source_register == 2u,
            "SUB verwendet falsche Register.");
    require(sub.text == "sub r2, r1", "SUB besitzt falschen Disassembly-Text.");

    const auto neg = katana::sh4::decode(0x631Bu);

    require(neg.kind == InstructionKind::NegateRegister, "NEG wurde nicht erkannt.");
    require(neg.destination_register == 3u && neg.source_register == 1u,
            "NEG verwendet falsche Register.");
    require(neg.text == "neg r1, r3", "NEG besitzt falschen Disassembly-Text.");

    const auto bit_not = katana::sh4::decode(0x6427u);

    require(bit_not.kind == InstructionKind::NotRegister, "NOT wurde nicht erkannt.");
    require(bit_not.destination_register == 4u && bit_not.source_register == 2u,
            "NOT verwendet falsche Register.");
    require(bit_not.text == "not r2, r4", "NOT besitzt falschen Disassembly-Text.");

    constexpr std::array<std::uint8_t, 14> bytes = {
        0x05, 0xE1, 0x02, 0xE2, 0x28, 0x31, 0x1B, 0x63, 0x27, 0x64, 0x0B, 0x00, 0x09, 0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, functions);

    require(program.size() == 1u, "Der ALU-Test muss genau eine Funktion besitzen.");

    const auto& instructions = program[0].blocks[0].instructions;

    require(instructions.size() == 7u, "Der IR-Block besitzt eine falsche Instruktionsanzahl.");

    require(instructions[2].operation == Operation::SubRegister,
            "SUB wurde nicht korrekt in IR abgesenkt.");
    require(instructions[3].operation == Operation::NegateRegister,
            "NEG wurde nicht korrekt in IR abgesenkt.");
    require(instructions[4].operation == Operation::NotRegister,
            "NOT wurde nicht korrekt in IR abgesenkt.");

    const auto verification = katana::ir::verify_function(program[0]);
    if (!verification.empty()) {
        std::cerr << verification.front().message << '\n';
    }
    require(verification.empty(), "ALU-Test erzeugt ungueltige Katana-IR.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("cpu.r[1] -= cpu.r[2];") != std::string::npos,
            "SUB fehlt im generierten C++.");

    require(source.find("cpu.r[3] = 0u - cpu.r[1];") != std::string::npos,
            "NEG fehlt im generierten C++.");

    require(source.find("cpu.r[4] = ~cpu.r[2];") != std::string::npos,
            "NOT fehlt im generierten C++.");

    std::cout << "Alle KR-1101 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
