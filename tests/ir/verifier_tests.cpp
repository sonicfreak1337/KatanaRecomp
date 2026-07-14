#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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
    constexpr std::array<std::uint8_t, 6> bytes = {
        0x00u, 0xE1u, // MOV #0,R1
        0x0Bu, 0x00u, // RTS
        0x09u, 0x00u  // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, discovered);

    require(
        katana::ir::verify_function(program.front()).empty(),
        "Gueltige abgesenkte Funktion wird abgelehnt."
    );

    auto invalid_width = program.front();
    invalid_width.blocks.front().instructions.front().widths.result =
        katana::ir::OperandWidth::Bits8;
    require(
        !katana::ir::verify_function(invalid_width).empty(),
        "Widerspruechliche Operandbreite wird nicht abgelehnt."
    );

    auto orphan_slot = program.front();
    orphan_slot.blocks.front().instructions.front().delay_slot = {
        katana::ir::DelaySlotRole::Slot,
        0x8C00FFFEu
    };
    require(
        !katana::ir::verify_function(orphan_slot).empty(),
        "Verwaister Delay Slot wird nicht abgelehnt."
    );

    bool codegen_rejected = false;
    try {
        const std::array<katana::ir::Function, 1> invalid_program = {invalid_width};
        static_cast<void>(katana::codegen::emit_cpp_program(
            invalid_program,
            invalid_width.entry_address
        ));
    } catch (const std::invalid_argument& error) {
        codegen_rejected = std::string(error.what()).find("Operandbreiten") !=
            std::string::npos;
    }
    require(codegen_rejected, "Codegenerator akzeptiert ungueltige Katana-IR.");

    std::cout << "KR-1905 IR-Verifier erfolgreich.\n";
    return EXIT_SUCCESS;
}
