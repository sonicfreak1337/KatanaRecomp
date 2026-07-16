#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
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

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 16> bytes = {
        0x02u,
        0xB0u, // BSR 0x8C010008
        0x09u,
        0x00u, // NOP
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u, // NOP
        0x05u,
        0xE1u, // MOV #5,R1
        0xFFu,
        0x71u, // ADD #-1,R1
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    const auto program = katana::ir::lower_program(lines, discovered);

    const auto text = katana::ir::emit_ir_text(program);
    const auto json = katana::ir::emit_ir_json(program);
    require(text == katana::ir::emit_ir_text(program),
            "Textausgabe ist zwischen zwei Laeufen nicht stabil.");
    require(json == katana::ir::emit_ir_json(program),
            "JSON-Ausgabe ist zwischen zwei Laeufen nicht stabil.");

    auto reordered = program;
    std::reverse(reordered.begin(), reordered.end());
    for (auto& function : reordered) {
        std::reverse(function.blocks.begin(), function.blocks.end());
        std::reverse(function.direct_callees.begin(), function.direct_callees.end());
        std::reverse(function.indirect_call_sites.begin(), function.indirect_call_sites.end());
        for (auto& block : function.blocks) {
            std::reverse(block.successors.begin(), block.successors.end());
        }
    }
    require(text == katana::ir::emit_ir_text(reordered),
            "Textausgabe haengt von Containerreihenfolgen ab.");
    require(json == katana::ir::emit_ir_json(reordered),
            "JSON-Ausgabe haengt von Containerreihenfolgen ab.");

    auto duplicate = program;
    duplicate.push_back(program.front());
    bool duplicate_rejected = false;
    try {
        static_cast<void>(katana::ir::emit_ir_text(duplicate));
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "Doppelte Funktionseinstiege erlauben mehrdeutige Ausgabe.");

    require(text.starts_with("katana-ir-v2\nfunction 0x8C010000\n"),
            "Textausgabe besitzt keinen stabilen Kopf oder keine Sortierung.");
    require(text.find("widths result=i32 input=none immediate=i8") != std::string::npos &&
                text.find("delay role=owner counterpart=0x8C010002") != std::string::npos &&
                text.find("status reads=[] writes=[]") != std::string::npos,
            "Textausgabe enthaelt nicht alle IR-Metadaten.");

    require(json.starts_with("{\"schema\":\"katana-ir-v2\",\"functions\":["),
            "JSON-Ausgabe besitzt kein stabiles Schema.");
    require(json.ends_with("]}\n") &&
                json.find("\"entry_address\":\"0x8C010000\"") != std::string::npos &&
                json.find("\"memory_effects\":{") != std::string::npos &&
                json.find("\"accumulator_effects\":{") != std::string::npos &&
                json.find("\"delay_slot\":{\"role\":\"owner\"") != std::string::npos,
            "JSON-Ausgabe ist unvollstaendig oder nicht abgeschlossen.");

    std::cout << "KR-1906 deterministische IR-Ausgabe erfolgreich.\n";
    return EXIT_SUCCESS;
}
