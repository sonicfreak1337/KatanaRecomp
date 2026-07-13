#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr std::uint32_t base_address = 0x100u;

constexpr std::array<std::uint8_t, 8> fixture = {
    0x7F, 0xC3,
    0x2B, 0x00, 0x01, 0x70,
    0x1B, 0x00
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<katana::ir::Function> build_program() {
    const auto lines = katana::sh4::disassemble(fixture, base_address);
    constexpr std::array<std::uint32_t, 3> seeds = {0x100u, 0x102u, 0x106u};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    return katana::ir::lower_program(lines, functions);
}

const katana::ir::Instruction* find_instruction(
    const std::vector<katana::ir::Function>& program,
    const std::uint32_t address
) {
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address == address) return &instruction;
            }
        }
    }
    return nullptr;
}

int emit_fixture(const std::string& output_path) {
    const auto source = katana::codegen::emit_cpp_program(build_program(), base_address);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return EXIT_FAILURE;
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

}

int main(const int argc, char* argv[]) {
    using katana::ir::Operation;
    using katana::sh4::ControlFlowKind;
    using katana::sh4::InstructionKind;

    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den KR-1407-Test.");

    const auto trapa = katana::sh4::decode(0xC3FFu);
    const auto rte = katana::sh4::decode(0x002Bu);
    const auto sleep = katana::sh4::decode(0x001Bu);
    require(
        trapa.kind == InstructionKind::TrapAlways &&
        trapa.immediate == 255 && trapa.control_flow == ControlFlowKind::Trap &&
        !trapa.is_privileged && trapa.text == "trapa #255",
        "TRAPA wurde falsch dekodiert."
    );
    require(
        rte.kind == InstructionKind::ReturnFromException &&
        rte.control_flow == ControlFlowKind::ExceptionReturn &&
        rte.has_delay_slot && rte.is_privileged && rte.text == "rte",
        "RTE wurde falsch dekodiert oder markiert."
    );
    require(
        sleep.kind == InstructionKind::Sleep &&
        sleep.control_flow == ControlFlowKind::Halt &&
        sleep.is_privileged && sleep.text == "sleep",
        "SLEEP wurde falsch dekodiert oder markiert."
    );

    const auto program = build_program();
    const auto* lowered_trapa = find_instruction(program, 0x100u);
    const auto* lowered_rte = find_instruction(program, 0x102u);
    const auto* delay_slot = find_instruction(program, 0x104u);
    const auto* lowered_sleep = find_instruction(program, 0x106u);
    require(
        program.size() == 3u && lowered_trapa != nullptr &&
        lowered_trapa->operation == Operation::TrapAlways &&
        lowered_trapa->immediate == 127 && !lowered_trapa->is_privileged,
        "TRAPA wurde falsch analysiert oder abgesenkt."
    );
    require(
        lowered_rte != nullptr && lowered_rte->operation == Operation::ReturnFromException &&
        lowered_rte->has_delay_slot && lowered_rte->is_privileged &&
        delay_slot != nullptr && delay_slot->is_delay_slot,
        "RTE-Delay-Slot wurde falsch analysiert oder abgesenkt."
    );
    require(
        lowered_sleep != nullptr && lowered_sleep->operation == Operation::Sleep &&
        lowered_sleep->is_privileged,
        "SLEEP wurde falsch abgesenkt."
    );

    const auto source = katana::codegen::emit_cpp_program(program, base_address);
    require(
        source.find("cpu.expevt = 0x00000160u;") != std::string::npos &&
        source.find("cpu.write_sr(cpu.ssr);") != std::string::npos &&
        source.find("cpu.sleeping = true;") != std::string::npos,
        "Der C++-Emitter bildet die Kontrollpfade nicht sichtbar ab."
    );

    std::cout << "Alle KR-1407 Decoder-, Analyse-, IR- und Codegen-Tests erfolgreich.\n";
    return EXIT_SUCCESS;
}
