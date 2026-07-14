#include "katana/analysis/value_analysis.hpp"
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
    constexpr std::array<std::uint8_t, 10> bytes{
        0x05u, 0xE1u,
        0x03u, 0x71u,
        0x13u, 0x62u,
        0x09u, 0x00u,
        0x2Cu, 0x32u
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    const auto trace = katana::analysis::propagate_local_constants(lines);
    require(trace.size() == 5u, "Die lokale Spur ist unvollstaendig.");
    require(trace[0].after.registers[1] == 5u, "MOV #imm wurde nicht propagiert.");
    require(trace[1].after.registers[1] == 8u, "ADD #imm wurde nicht propagiert.");
    require(trace[2].after.registers[2] == 8u, "Registerkopie wurde nicht propagiert.");
    require(trace[3].after.registers[2] == 8u, "NOP hat eine Konstante zerstoert.");
    require(
        trace[4].after.registers[1] == 8u
            && trace[4].after.registers[2] == 16u
            && !trace[4].after.registers[3].has_value(),
        "Registerweises ADD hat unabhaengige Konstanten verworfen oder den Zielwert falsch berechnet."
    );

    constexpr std::array<std::uint8_t, 8> indirect_bytes{
        0x20u, 0xE1u,
        0x03u, 0x71u,
        0x13u, 0x62u,
        0x2Bu, 0x42u
    };
    const auto indirect_lines = katana::sh4::disassemble(indirect_bytes, 0x1000u);
    const auto values = katana::analysis::analyze_register_values(indirect_lines);
    require(values.indirect_control_flow.size() == 1u, "Indirekte Kontrollflussstelle wurde nicht beobachtet.");
    require(
        values.indirect_control_flow[0].instruction_address == 0x1006u
            && values.indirect_control_flow[0].register_index == 2u
            && values.indirect_control_flow[0].value == 0x23u,
        "Registerwert am indirekten Sprung wurde falsch analysiert."
    );

    katana::io::ExecutableImage image;
    image.add_segment({
        ".text", 0u, 0u, 12u, katana::io::SegmentKind::Code, {true, false, true},
        {0x08u, 0xE1u, 0x2Bu, 0x41u, 0x09u, 0x00u, 0x2Bu, 0x42u, 0x0Bu, 0x00u, 0x09u, 0x00u}
    });
    const auto resolution_lines = katana::sh4::disassemble(image.segments()[0].bytes, 0u);
    const auto resolutions = katana::analysis::resolve_indirect_control_flow(resolution_lines, image);
    require(resolutions.size() == 2u, "Indirekte Kontrollflussstellen wurden nicht vollstaendig klassifiziert.");
    require(
        resolutions[0].status == katana::analysis::ResolutionStatus::Resolved
            && resolutions[0].target == 8u
            && resolutions[0].reason == "constant-register",
        "Ein beweisbares indirektes Sprungziel wurde nicht aufgeloest."
    );
    require(
        resolutions[1].status == katana::analysis::ResolutionStatus::Unresolved
            && resolutions[1].reason == "register-value-unknown",
        "Ein unbekanntes Registerziel wurde faelschlich aufgeloest."
    );

    std::cout << "KR-1801 Lokale Konstantenpropagation erfolgreich.\n";
    return EXIT_SUCCESS;
}
