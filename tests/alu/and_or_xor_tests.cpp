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

    const auto and_register = katana::sh4::decode(0x2129u);

    require(
        and_register.kind == InstructionKind::AndRegister,
        "AND Register wurde nicht erkannt."
    );
    require(
        and_register.destination_register == 1u &&
        and_register.source_register == 2u,
        "AND Register verwendet falsche Register."
    );
    require(
        and_register.text == "and r2, r1",
        "AND Register besitzt falschen Disassembly-Text."
    );

    const auto xor_register = katana::sh4::decode(0x265Au);

    require(
        xor_register.kind == InstructionKind::XorRegister,
        "XOR Register wurde nicht erkannt."
    );
    require(
        xor_register.destination_register == 6u &&
        xor_register.source_register == 5u,
        "XOR Register verwendet falsche Register."
    );

    const auto or_register = katana::sh4::decode(0x243Bu);

    require(
        or_register.kind == InstructionKind::OrRegister,
        "OR Register wurde nicht erkannt."
    );
    require(
        or_register.destination_register == 4u &&
        or_register.source_register == 3u,
        "OR Register verwendet falsche Register."
    );

    const auto and_immediate = katana::sh4::decode(0xC90Fu);

    require(
        and_immediate.kind == InstructionKind::AndImmediate,
        "AND Immediate wurde nicht erkannt."
    );
    require(
        and_immediate.destination_register == 0u &&
        and_immediate.immediate == 15,
        "AND Immediate wurde falsch dekodiert."
    );

    const auto xor_immediate = katana::sh4::decode(0xCAFFu);

    require(
        xor_immediate.kind == InstructionKind::XorImmediate,
        "XOR Immediate wurde nicht erkannt."
    );
    require(
        xor_immediate.immediate == 255,
        "XOR Immediate muss ohne Vorzeichenerweiterung dekodiert werden."
    );

    const auto or_immediate = katana::sh4::decode(0xCB80u);

    require(
        or_immediate.kind == InstructionKind::OrImmediate,
        "OR Immediate wurde nicht erkannt."
    );
    require(
        or_immediate.immediate == 128,
        "OR Immediate muss ohne Vorzeichenerweiterung dekodiert werden."
    );

    constexpr std::array<std::uint8_t, 26> bytes = {
        0xF0, 0xE1,
        0x0F, 0xE2,
        0x29, 0x21,
        0xF0, 0xE3,
        0x2B, 0x23,
        0xF0, 0xE4,
        0x2A, 0x24,
        0xFF, 0xE0,
        0x0F, 0xC9,
        0x80, 0xCB,
        0xFF, 0xCA,
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
        "Der KR-1102-Test muss genau eine Funktion besitzen."
    );

    const auto& instructions =
        program[0].blocks[0].instructions;

    require(
        instructions.size() == 13u,
        "Der IR-Block besitzt eine falsche Instruktionsanzahl."
    );

    require(
        instructions[2].operation == Operation::AndRegister,
        "AND Register wurde nicht korrekt in IR abgesenkt."
    );
    require(
        instructions[4].operation == Operation::OrRegister,
        "OR Register wurde nicht korrekt in IR abgesenkt."
    );
    require(
        instructions[6].operation == Operation::XorRegister,
        "XOR Register wurde nicht korrekt in IR abgesenkt."
    );
    require(
        instructions[8].operation == Operation::AndImmediate,
        "AND Immediate wurde nicht korrekt in IR abgesenkt."
    );
    require(
        instructions[9].operation == Operation::OrImmediate,
        "OR Immediate wurde nicht korrekt in IR abgesenkt."
    );
    require(
        instructions[10].operation == Operation::XorImmediate,
        "XOR Immediate wurde nicht korrekt in IR abgesenkt."
    );

    const auto verification = katana::ir::verify_function(program[0]);
    if (!verification.empty()) {
        std::cerr << verification.front().message << '\n';
    }
    require(verification.empty(), "Logiktest erzeugt ungueltige Katana-IR.");

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("cpu.r[1] &= cpu.r[2];") != std::string::npos,
        "AND Register fehlt im generierten C++."
    );
    require(
        source.find("cpu.r[3] |= cpu.r[2];") != std::string::npos,
        "OR Register fehlt im generierten C++."
    );
    require(
        source.find("cpu.r[4] ^= cpu.r[2];") != std::string::npos,
        "XOR Register fehlt im generierten C++."
    );
    require(
        source.find("cpu.r[0] &= static_cast<std::uint32_t>(15);") !=
            std::string::npos,
        "AND Immediate fehlt im generierten C++."
    );
    require(
        source.find("cpu.r[0] |= static_cast<std::uint32_t>(128);") !=
            std::string::npos,
        "OR Immediate fehlt im generierten C++."
    );
    require(
        source.find("cpu.r[0] ^= static_cast<std::uint32_t>(255);") !=
            std::string::npos,
        "XOR Immediate fehlt im generierten C++."
    );

    std::cout
        << "Alle KR-1102 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
