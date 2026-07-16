#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

const katana::analysis::IndirectControlFlowResolution*
find_resolution(const std::vector<katana::analysis::IndirectControlFlowResolution>& resolutions,
                const std::uint32_t address) {
    const auto found = std::find_if(
        resolutions.begin(), resolutions.end(), [address](const auto& item) {
            return item.instruction_address == address;
        });
    return found == resolutions.end() ? nullptr : &*found;
}

katana::io::ExecutableImage direct_call_image(const std::uint8_t test_register,
                                              const katana::io::GuestCallAbi abi,
                                              const std::string& source = {}) {
    katana::io::ExecutableImage image(source);
    image.set_guest_call_abi(abi);
    image.add_segment({".text",
                       0u,
                       0u,
                       16u,
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       {0x0Cu,
                        static_cast<std::uint8_t>(0xE0u | test_register),
                        0xFDu,
                        0xBFu,
                        0x0Eu,
                        static_cast<std::uint8_t>(0xE0u | test_register),
                        0x2Bu,
                        static_cast<std::uint8_t>(0x40u | test_register),
                        0x09u,
                        0x00u,
                        0x09u,
                        0x00u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u}});
    return image;
}

katana::io::ExecutableImage indirect_call_image(const std::uint8_t test_register,
                                                const katana::io::GuestCallAbi abi) {
    const auto branch_register = test_register < 8u ? 14u : 15u;
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(abi);
    image.add_segment({".text",
                       0u,
                       0u,
                       16u,
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       {0x00u,
                        static_cast<std::uint8_t>(0xE0u | branch_register),
                        0x0Cu,
                        static_cast<std::uint8_t>(0xE0u | test_register),
                        0x0Bu,
                        static_cast<std::uint8_t>(0x40u | branch_register),
                        0x0Eu,
                        static_cast<std::uint8_t>(0xE0u | test_register),
                        0x2Bu,
                        static_cast<std::uint8_t>(0x40u | test_register),
                        0x09u,
                        0x00u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u}});
    return image;
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

    katana::io::ExecutableImage register_relative;
    register_relative.add_segment({".text",
                                   0u,
                                   0u,
                                   16u,
                                   katana::io::SegmentKind::Code,
                                   {true, false, true},
                                   {0x08u,
                                    0xE0u, // mov #8,r0
                                    0x23u,
                                    0x00u, // braf r0 -> 0x0000000E
                                    0x09u,
                                    0x00u,
                                    0x09u,
                                    0x00u,
                                    0x09u,
                                    0x00u,
                                    0x09u,
                                    0x00u,
                                    0x09u,
                                    0x00u,
                                    0x0Bu,
                                    0x00u}});
    const auto relative_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(register_relative.segments()[0].bytes, 0u), register_relative);
    require(relative_resolution.size() == 1u &&
                relative_resolution[0].status == katana::analysis::ResolutionStatus::Resolved &&
                relative_resolution[0].target == 0x0Eu &&
                relative_resolution[0].reason == "register-relative-constant-register",
            "BRAF-Ziel verwendet nicht den vor dem Delay Slot gelesenen PC+4+Rm-Wert.");

    katana::io::ExecutableImage pc_zero_fill;
    pc_zero_fill.add_segment({".text",
                              0u,
                              0u,
                              16u,
                              katana::io::SegmentKind::Code,
                              {true, false, true},
                              {// mov.l @(4,pc),r1 -> Adresse 8 liegt im Zero-Fill
                               0x01u,
                               0xD1u,
                               0x2Bu,
                               0x41u,
                               0x09u,
                               0x00u}});
    const auto pc_zero_fill_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(pc_zero_fill.segments()[0].bytes, 0u), pc_zero_fill);
    require(pc_zero_fill_resolution.size() == 1u &&
                pc_zero_fill_resolution[0].status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                pc_zero_fill_resolution[0].reason == "register-value-unknown",
            "PC-relatives Literal im Zero-Fill wurde nicht absturzfrei abgelehnt.");

    for (std::uint8_t reg = 8u; reg <= 14u; ++reg) {
        for (const bool indirect : {false, true}) {
            const auto abi_image = indirect
                                       ? indirect_call_image(reg, katana::io::GuestCallAbi::SuperHC)
                                       : direct_call_image(reg, katana::io::GuestCallAbi::SuperHC);
            const auto abi_resolutions = katana::analysis::resolve_indirect_control_flow(
                katana::sh4::disassemble(abi_image.segments()[0].bytes, 0u), abi_image);
            const auto* after_call = find_resolution(abi_resolutions, indirect ? 8u : 6u);
            require(after_call != nullptr &&
                        after_call->status == katana::analysis::ResolutionStatus::Resolved &&
                        after_call->target == 14u &&
                        after_call->reason.starts_with("sh-c-abi-preserved-"),
                    "R8 bis R14 wurden ueber direkten/indirekten SH-C-Call oder dessen "
                    "Delay-Slot-Schreibzugriff verloren.");
        }
    }
    for (std::uint8_t reg = 0u; reg <= 7u; ++reg) {
        for (const bool indirect : {false, true}) {
            const auto abi_image = indirect
                                       ? indirect_call_image(reg, katana::io::GuestCallAbi::SuperHC)
                                       : direct_call_image(reg, katana::io::GuestCallAbi::SuperHC);
            const auto abi_resolutions = katana::analysis::resolve_indirect_control_flow(
                katana::sh4::disassemble(abi_image.segments()[0].bytes, 0u), abi_image);
            const auto* after_call = find_resolution(abi_resolutions, indirect ? 8u : 6u);
            require(after_call != nullptr &&
                        after_call->status == katana::analysis::ResolutionStatus::Unresolved,
                    "R0 bis R7 wurden ueber einen SH-C-Call faelschlich erhalten.");
        }
    }
    for (const auto& source : {std::string("raw.bin"), std::string("image.elf")}) {
        const auto abi_less = direct_call_image(8u, katana::io::GuestCallAbi::Unknown, source);
        const auto abi_less_resolutions = katana::analysis::resolve_indirect_control_flow(
            katana::sh4::disassemble(abi_less.segments()[0].bytes, 0u), abi_less);
        const auto* after_call = find_resolution(abi_less_resolutions, 6u);
        require(after_call != nullptr &&
                    after_call->status == katana::analysis::ResolutionStatus::Unresolved,
                "ABI-lose Raw-/ELF-Eingabe erhielt R8 faelschlich ueber einen Call.");
    }

    katana::analysis::RegisterConstants post_increment_initial;
    post_increment_initial.registers[1] = 0x100u;
    post_increment_initial.registers[8] = 0x20u;
    katana::sh4::DisassemblyLine post_increment;
    post_increment.address = 0u;
    post_increment.instruction.kind =
        katana::sh4::InstructionKind::MovLongLoadPostIncrement;
    post_increment.instruction.source_register = 1u;
    post_increment.instruction.destination_register = 1u;
    const std::array post_increment_lines{post_increment};
    const auto post_increment_trace =
        katana::analysis::propagate_local_constants(post_increment_lines, post_increment_initial);
    require(!post_increment_trace[0].after.registers[1].has_value() &&
                post_increment_trace[0].after.registers[8] == 0x20u,
            "Post-Increment mit identischem Quell-/Zielregister clobberte falsche Register.");

    katana::analysis::RegisterConstants pre_decrement_initial;
    pre_decrement_initial.registers[2] = 0x22u;
    pre_decrement_initial.registers[15] = 0x100u;
    katana::sh4::DisassemblyLine pre_decrement;
    pre_decrement.address = 0u;
    pre_decrement.instruction.kind =
        katana::sh4::InstructionKind::MovLongStorePreDecrement;
    pre_decrement.instruction.source_register = 2u;
    pre_decrement.instruction.destination_register = 15u;
    const std::array pre_decrement_lines{pre_decrement};
    const auto pre_decrement_trace =
        katana::analysis::propagate_local_constants(pre_decrement_lines, pre_decrement_initial);
    require(pre_decrement_trace[0].after.registers[2] == 0x22u &&
                !pre_decrement_trace[0].after.registers[15].has_value(),
            "Pre-Decrement invalidierte nicht ausschliesslich das aktualisierte Basisregister.");

    auto join_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 8>{0x0Cu, 0xE8u, 0x00u, 0x89u, 0x09u, 0x00u, 0x2Bu, 0x48u},
        0u);
    require(join_lines[1].target_address == 6u, "Synthetischer CFG-Join ist falsch kodiert.");
    const auto join_values = katana::analysis::analyze_register_values(join_lines);
    require(join_values.indirect_control_flow.size() == 1u &&
                !join_values.indirect_control_flow[0].value.has_value(),
            "Konstante wurde ueber einen mehrdeutigen CFG-Join getragen.");

    katana::io::ExecutableImage immutable_load;
    immutable_load.add_segment({".text",
                                0u,
                                0u,
                                0x20u,
                                katana::io::SegmentKind::Code,
                                {true, false, true},
                                {0x20u, 0xE1u, 0x12u, 0x62u, 0x2Bu, 0x42u, 0x09u, 0x00u,
                                 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
                                 0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
                                 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u}});
    immutable_load.add_segment({".rodata",
                                0x20u,
                                0x20u,
                                4u,
                                katana::io::SegmentKind::Data,
                                {true, false, false},
                                {0x10u, 0x00u, 0x00u, 0x00u}});
    const auto immutable_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(immutable_load.segments()[0].bytes, 0u), immutable_load);
    require(immutable_resolution[0].status == katana::analysis::ResolutionStatus::Resolved &&
                immutable_resolution[0].target == 0x10u &&
                immutable_resolution[0].reason == "bounded-immutable-pointer",
            "Read-only Speicherload wurde nicht reproduzierbar bewiesen.");
    katana::io::ExecutableImage mutable_load;
    mutable_load.add_segment(immutable_load.segments()[0]);
    mutable_load.add_segment({".data",
                              0x20u,
                              0x20u,
                              4u,
                              katana::io::SegmentKind::Data,
                              {true, true, false},
                              {0x10u, 0x00u, 0x00u, 0x00u}});
    const auto mutable_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(mutable_load.segments()[0].bytes, 0u), mutable_load);
    require(mutable_resolution[0].status == katana::analysis::ResolutionStatus::Unresolved,
            "Beschreibbarer Speicherload wurde statisch eingefroren.");

    std::cout << "KR-1801/KR-4506/KR-4711/KR-4712 Wertanalyse erfolgreich.\n";
    return EXIT_SUCCESS;
}
