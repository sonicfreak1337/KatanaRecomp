#include "katana/analysis/control_flow_report.hpp"
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

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 10> bytes{
        0x05u, 0xE1u, 0x03u, 0x71u, 0x13u, 0x62u, 0x09u, 0x00u, 0x2Cu, 0x32u};
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    const auto trace = katana::analysis::propagate_local_constants(lines);
    require(trace.size() == 5u, "Die lokale Spur ist unvollstaendig.");
    require(trace[0].after.registers[1] == 5u, "MOV #imm wurde nicht propagiert.");
    require(trace[1].after.registers[1] == 8u, "ADD #imm wurde nicht propagiert.");
    require(trace[2].after.registers[2] == 8u, "Registerkopie wurde nicht propagiert.");
    require(trace[3].after.registers[2] == 8u, "NOP hat eine Konstante zerstoert.");
    require(trace[4].after.registers[1] == 8u && trace[4].after.registers[2] == 16u &&
                !trace[4].after.registers[3].has_value(),
            "Registerweises ADD hat unabhaengige Konstanten verworfen oder den Zielwert falsch "
            "berechnet.");

    constexpr std::array<std::uint8_t, 8> indirect_bytes{
        0x20u, 0xE1u, 0x03u, 0x71u, 0x13u, 0x62u, 0x2Bu, 0x42u};
    const auto indirect_lines = katana::sh4::disassemble(indirect_bytes, 0x1000u);
    const auto values = katana::analysis::analyze_register_values(indirect_lines);
    require(values.indirect_control_flow.size() == 1u,
            "Indirekte Kontrollflussstelle wurde nicht beobachtet.");
    require(values.indirect_control_flow[0].instruction_address == 0x1006u &&
                values.indirect_control_flow[0].register_index == 2u &&
                values.indirect_control_flow[0].value == 0x23u,
            "Registerwert am indirekten Sprung wurde falsch analysiert.");

    katana::io::ExecutableImage image;
    image.add_segment(
        {".text",
         0u,
         0u,
         12u,
         katana::io::SegmentKind::Code,
         {true, false, true},
         {0x08u, 0xE1u, 0x2Bu, 0x41u, 0x09u, 0x00u, 0x2Bu, 0x42u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    const auto resolution_lines = katana::sh4::disassemble(image.segments()[0].bytes, 0u);
    const auto resolutions =
        katana::analysis::resolve_indirect_control_flow(resolution_lines, image);
    require(resolutions.size() == 2u,
            "Indirekte Kontrollflussstellen wurden nicht vollstaendig klassifiziert.");
    require(resolutions[0].status == katana::analysis::ResolutionStatus::Resolved &&
                resolutions[0].target == 8u && resolutions[0].reason == "constant-register",
            "Ein beweisbares indirektes Sprungziel wurde nicht aufgeloest.");
    require(resolutions[1].status == katana::analysis::ResolutionStatus::Unresolved &&
                resolutions[1].reason == "register-value-unknown",
            "Ein unbekanntes Registerziel wurde faelschlich aufgeloest.");
    const auto report = katana::analysis::format_indirect_control_flow_report(resolutions);
    require(report.find("Aufgeloest:") < report.find("Ungeloest:") &&
                report.find("0x00000008 [constant-register]") != std::string::npos,
            "Aufgeloester Kontrollfluss fehlt im getrennten Bericht.");
    require(report.find("[register-value-unknown]") != std::string::npos &&
                report.find("Hinweis: jump = 0x00000006 ZIEL") != std::string::npos,
            "Ungelesene Kontrollflussstelle besitzt keinen Grund oder Nutzerhinweis.");

    auto separated_lines = katana::sh4::disassemble(std::array<std::uint8_t, 2>{0x08u, 0xE1u}, 0u);
    const auto separated_jump =
        katana::sh4::disassemble(std::array<std::uint8_t, 2>{0x2Bu, 0x41u}, 0x100u);
    separated_lines.push_back(separated_jump[0]);
    const auto separated = katana::analysis::analyze_register_values(separated_lines);
    require(separated.indirect_control_flow.size() == 1u &&
                !separated.indirect_control_flow[0].value.has_value(),
            "Registerkonstante wurde ueber eine nicht zusammenhaengende Codeluecke getragen.");

    katana::io::ExecutableImage zero_fill;
    zero_fill.add_segment({".text",
                           0u,
                           0u,
                           8u,
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {0x04u, 0xE1u, 0x2Bu, 0x41u}});
    const auto zero_lines = katana::sh4::disassemble(zero_fill.segments()[0].bytes, 0u);
    const auto zero_resolution =
        katana::analysis::resolve_indirect_control_flow(zero_lines, zero_fill);
    require(zero_resolution.size() == 1u &&
                zero_resolution[0].status == katana::analysis::ResolutionStatus::Unresolved &&
                zero_resolution[0].reason == "outside-committed-data",
            "Konstantes indirektes Ziel im Zero-Fill wurde als sicher markiert.");

    katana::io::ExecutableImage pc_literal;
    pc_literal.add_segment({".text",
                            0u,
                            0u,
                            16u,
                            katana::io::SegmentKind::Code,
                            {true, false, true},
                            {// mov.l @(4,pc),r1; jmp @r1; nop; nop
                             0x01u,
                             0xD1u,
                             0x2Bu,
                             0x41u,
                             0x09u,
                             0x00u,
                             0x09u,
                             0x00u,
                             // Literalziel 0x0000000C; Ziel: nop; rts
                             0x0Cu,
                             0x00u,
                             0x00u,
                             0x00u,
                             0x09u,
                             0x00u,
                             0x0Bu,
                             0x00u}});
    const auto pc_literal_lines = katana::sh4::disassemble(pc_literal.segments()[0].bytes, 0u);
    const auto pc_literal_resolution =
        katana::analysis::resolve_indirect_control_flow(pc_literal_lines, pc_literal);
    require(pc_literal_resolution.size() == 1u &&
                pc_literal_resolution[0].status == katana::analysis::ResolutionStatus::Resolved &&
                pc_literal_resolution[0].target == 0x0Cu &&
                pc_literal_resolution[0].reason == "pc-relative-literal",
            "PC-relatives Literal wurde nicht als indirektes Sprungziel aufgeloest.");

    katana::io::ExecutableImage pc_word;
    pc_word.add_segment({".text",
                         0u,
                         0u,
                         16u,
                         katana::io::SegmentKind::Code,
                         {true, false, true},
                         {0x01u,
                          0x91u,
                          0x2Bu,
                          0x41u,
                          0x09u,
                          0x00u,
                          0x0Cu,
                          0x00u,
                          0x09u,
                          0x00u,
                          0x09u,
                          0x00u,
                          0x09u,
                          0x00u,
                          0x0Bu,
                          0x00u}});
    const auto pc_word_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(pc_word.segments()[0].bytes, 0u), pc_word);
    require(pc_word_resolution.size() == 1u && pc_word_resolution[0].target == 0x0Cu &&
                pc_word_resolution[0].reason == "pc-relative-literal",
            "Vorzeichenbehaftetes PC-relatives Wortliteral wurde nicht aufgeloest.");

    katana::io::ExecutableImage pc_address;
    pc_address.add_segment(
        {".text",
         0u,
         0u,
         12u,
         katana::io::SegmentKind::Code,
         {true, false, true},
         {0x01u, 0xC7u, 0x2Bu, 0x40u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    const auto pc_address_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(pc_address.segments()[0].bytes, 0u), pc_address);
    require(pc_address_resolution.size() == 1u && pc_address_resolution[0].target == 8u &&
                pc_address_resolution[0].reason == "pc-relative-address",
            "MOVA-Adresse wurde nicht als indirektes Sprungziel aufgeloest.");

    std::cout << "KR-1801/KR-4506 Lokale Konstantenpropagation erfolgreich.\n";
    return EXIT_SUCCESS;
}
