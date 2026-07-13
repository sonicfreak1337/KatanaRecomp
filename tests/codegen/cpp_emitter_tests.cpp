#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
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
    constexpr std::array<std::uint8_t, 16> bytes = {
        0x02, 0xB0,
        0x07, 0xE2,
        0x0B, 0x00,
        0x09, 0x00,
        0x05, 0xE1,
        0xFF, 0x71,
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

    const auto discovered =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    const auto program =
        katana::ir::lower_program(
            lines,
            discovered
        );

    require(
        program.size() == 2,
        "Das Codegen-Testprogramm muss zwei Funktionen enthalten."
    );

    require(
        program[0].blocks[0].successors.size() == 1,
        "Ein Call-Block darf intraprozedural nur den Rueckkehrpfad besitzen."
    );

    require(
        program[0].blocks[0].successors[0] == 0x8C010004u,
        "Der intraprozedurale Call-Nachfolger ist falsch."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("struct CpuState") != std::string::npos,
        "Der generierte Runtime-State fehlt."
    );

    require(
        source.find("fn_8C010000") != std::string::npos,
        "Die generierte Einstiegsfunktion fehlt."
    );

    require(
        source.find("fn_8C010008") != std::string::npos,
        "Die generierte Unterfunktion fehlt."
    );

    const auto delay_position =
        source.find("cpu.r[2] = static_cast<std::uint32_t>(7);");

    const auto call_position =
        source.find("fn_8C010008(cpu);");

    require(
        delay_position != std::string::npos,
        "Die Delay-Slot-Instruktion fehlt im generierten Code."
    );

    require(
        call_position != std::string::npos,
        "Der direkte Funktionsaufruf fehlt im generierten Code."
    );

    require(
        delay_position < call_position,
        "Der Delay Slot muss vor dem Funktionsaufruf ausgefuehrt werden."
    );

    require(
        source.find(
            "cpu.r[1] += static_cast<std::uint32_t>(-1);"
        ) != std::string::npos,
        "ADD Immediate wurde nicht generiert."
    );

    require(
        source.find("cpu.pc = cpu.pr;") != std::string::npos,
        "Die Ruecksprungsemantik fehlt."
    );

    std::cout
        << "Alle C++-Codegenerator-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
