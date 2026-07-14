#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
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

}

int main() {
    constexpr std::array<std::uint8_t, 12> bytes = {
        0x05u, 0xE1u, // MOV #5,R1
        0xFFu, 0x71u, // ADD #-1,R1
        0x03u, 0xE2u, // MOV #3,R2
        0x2Cu, 0x31u, // ADD R2,R1
        0x0Bu, 0x00u, // RTS
        0x09u, 0x00u  // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, discovered);
    auto& function = program.front();

    const auto result = katana::ir::fold_constants(function);
    const auto& instructions = function.blocks.front().instructions;
    require(result.changes == 2u,
        "Constant Folding meldet eine falsche Aenderungszahl.");
    require(
        instructions[1].operation == katana::ir::Operation::MovImmediate &&
        static_cast<std::uint32_t>(instructions[1].immediate) == 4u &&
        instructions[3].operation == katana::ir::Operation::MovImmediate &&
        static_cast<std::uint32_t>(instructions[3].immediate) == 7u,
        "Konstante 32-Bit-Ausdruecke wurden nicht korrekt gefaltet."
    );
    require(katana::ir::verify_function(function).empty(),
        "Constant Folding erzeugt ungueltige Katana-IR.");

    constexpr std::array<std::uint8_t, 12> copy_bytes = {
        0x13u, 0x62u, // MOV R1,R2
        0x2Cu, 0x33u, // ADD R2,R3
        0x05u, 0xE1u, // MOV #5,R1
        0x2Cu, 0x34u, // ADD R2,R4
        0x0Bu, 0x00u, // RTS
        0x09u, 0x00u  // NOP
    };
    const auto copy_lines = katana::sh4::disassemble(copy_bytes, 0x8C020000u);
    constexpr std::array<std::uint32_t, 1> copy_seeds = {0x8C020000u};
    const auto copy_discovered = katana::analysis::discover_functions(
        copy_lines,
        copy_seeds
    );
    auto copy_program = katana::ir::lower_program(copy_lines, copy_discovered);
    auto& copy_function = copy_program.front();
    const auto copy_result = katana::ir::propagate_copies(copy_function);
    const auto& copy_instructions = copy_function.blocks.front().instructions;
    require(
        copy_result.changes == 1u &&
        copy_instructions[1].source_register == 1u &&
        copy_instructions[3].source_register == 2u,
        "Copy Propagation ersetzt Aliase ueber einen Quellschreibzugriff hinweg."
    );
    require(katana::ir::verify_function(copy_function).empty(),
        "Copy Propagation erzeugt ungueltige Katana-IR.");

    std::cout << "KR-2001 Constant Folding erfolgreich.\n";
    return EXIT_SUCCESS;
}
