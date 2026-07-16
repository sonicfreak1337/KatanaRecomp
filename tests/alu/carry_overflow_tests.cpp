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

    const auto addc = katana::sh4::decode(0x312Eu);
    const auto addv = katana::sh4::decode(0x312Fu);
    const auto subc = katana::sh4::decode(0x312Au);
    const auto subv = katana::sh4::decode(0x312Bu);
    const auto negc = katana::sh4::decode(0x631Au);

    require(addc.kind == InstructionKind::AddWithCarry && addc.text == "addc r2, r1",
            "ADDC wurde falsch dekodiert.");

    require(addv.kind == InstructionKind::AddWithOverflow && addv.text == "addv r2, r1",
            "ADDV wurde falsch dekodiert.");

    require(subc.kind == InstructionKind::SubWithCarry && subc.text == "subc r2, r1",
            "SUBC wurde falsch dekodiert.");

    require(subv.kind == InstructionKind::SubWithOverflow && subv.text == "subv r2, r1",
            "SUBV wurde falsch dekodiert.");

    require(negc.kind == InstructionKind::NegateWithCarry && negc.destination_register == 3u &&
                negc.source_register == 1u && negc.text == "negc r1, r3",
            "NEGC wurde falsch dekodiert.");

    constexpr std::array<std::uint8_t, 14> bytes = {
        0x2E, 0x31, 0x2F, 0x31, 0x2A, 0x31, 0x2B, 0x31, 0x1A, 0x63, 0x0B, 0x00, 0x09, 0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, functions);

    require(program.size() == 1u, "Der KR-1104-Test muss genau eine Funktion besitzen.");

    const auto& instructions = program[0].blocks[0].instructions;

    require(instructions.size() == 7u, "Der KR-1104-IR-Block besitzt eine falsche Groesse.");

    require(instructions[0].operation == Operation::AddWithCarry,
            "ADDC wurde falsch in IR abgesenkt.");
    require(instructions[1].operation == Operation::AddWithOverflow,
            "ADDV wurde falsch in IR abgesenkt.");
    require(instructions[2].operation == Operation::SubWithCarry,
            "SUBC wurde falsch in IR abgesenkt.");
    require(instructions[3].operation == Operation::SubWithOverflow,
            "SUBV wurde falsch in IR abgesenkt.");
    require(instructions[4].operation == Operation::NegateWithCarry,
            "NEGC wurde falsch in IR abgesenkt.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("const std::uint64_t carry_in") != std::string::npos &&
                source.find("cpu.t = result > 0xFFFFFFFFull;") != std::string::npos,
            "ADDC-Semantik fehlt im generierten C++.");

    require(source.find("const std::uint64_t borrow_in") != std::string::npos &&
                source.find("cpu.t = minuend < subtrahend;") != std::string::npos,
            "SUBC-Semantik fehlt im generierten C++.");

    require(source.find("cpu.t = subtrahend != 0ull;") != std::string::npos,
            "NEGC-Semantik fehlt im generierten C++.");

    require(source.find("~(lhs ^ rhs) & (lhs ^ result)") != std::string::npos,
            "ADDV-Overflowtest fehlt im generierten C++.");

    require(source.find("(lhs ^ rhs) & (lhs ^ result)") != std::string::npos,
            "SUBV-Overflowtest fehlt im generierten C++.");

    std::cout << "Alle KR-1104 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
