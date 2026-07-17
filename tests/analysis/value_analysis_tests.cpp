#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
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
    const auto found =
        std::find_if(resolutions.begin(), resolutions.end(), [address](const auto& item) {
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

    const auto mixed_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 8>{0x00u, 0xC7u, 0x04u, 0xE1u, 0x0Cu, 0x31u, 0x2Bu, 0x41u}, 0u);
    const auto mixed_values = katana::analysis::analyze_register_values(mixed_lines);
    require(mixed_values.indirect_control_flow.size() == 1u &&
                mixed_values.indirect_control_flow[0].value == 8u &&
                mixed_values.indirect_control_flow[0].source ==
                    "add(constant-register,pc-relative-address)",
            "Konstante plus PC-relative Adresse verlor ihre zusammengesetzte Herkunft.");

    auto arithmetic_line = [](const katana::sh4::InstructionKind kind,
                              const std::uint8_t source,
                              const std::uint8_t destination) {
        katana::sh4::DisassemblyLine result;
        result.instruction.kind = kind;
        result.instruction.source_register = source;
        result.instruction.destination_register = destination;
        return result;
    };
    katana::analysis::RegisterConstants provenance_initial;
    provenance_initial.registers[1] = 2u;
    provenance_initial.sources[1] = "immutable-value";
    provenance_initial.registers[2] = 3u;
    provenance_initial.sources[2] = "pc-relative-address";
    provenance_initial.registers[3] = 7u;
    provenance_initial.sources[3] = "third-source";
    const std::array provenance_lines{
        arithmetic_line(katana::sh4::InstructionKind::AddRegister, 2u, 1u),
        arithmetic_line(katana::sh4::InstructionKind::XorRegister, 3u, 1u)};
    auto sequenced_lines = provenance_lines;
    sequenced_lines[1].address = 2u;
    const auto provenance_trace =
        katana::analysis::propagate_local_constants(sequenced_lines, provenance_initial);
    require(provenance_trace[0].after.sources[1] == "add(immutable-value,pc-relative-address)" &&
                provenance_trace[1].after.sources[1] ==
                    "xor(add(immutable-value,pc-relative-address),third-source)",
            "Mehrstufige oder unterschiedlich hergeleitete Arithmetik verlor ihre Beweiskette.");

    katana::analysis::RegisterConstants self_initial;
    self_initial.registers[1] = 2u;
    self_initial.sources[1] = "self-source";
    const std::array self_add_lines{
        arithmetic_line(katana::sh4::InstructionKind::AddRegister, 1u, 1u)};
    const auto self_add = katana::analysis::propagate_local_constants(self_add_lines, self_initial);
    require(self_add[0].after.registers[1] == 4u &&
                self_add[0].after.sources[1] == "add(self-source,self-source)",
            "Identisches ADD-Quell-/Zielregister verlor Wert oder Herkunft.");

    const std::array self_xor_lines{
        arithmetic_line(katana::sh4::InstructionKind::XorRegister, 5u, 5u)};
    const auto self_xor = katana::analysis::propagate_local_constants(self_xor_lines);
    require(self_xor[0].after.registers[5] == 0u && self_xor[0].after.sources[5] == "xor-self",
            "XOR Rn,Rn wurde nicht unabhaengig vom Eingang sicher zu null gefaltet.");

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

    const auto writable_entry_image = [](const std::uint8_t store_address,
                                         const bool initialize_store) {
        std::vector<std::uint8_t> bytes(32u, 0x09u);
        std::size_t cursor = 0u;
        if (initialize_store) {
            bytes[cursor++] = store_address;
            bytes[cursor++] = 0xE1u; // mov #address,r1
        }
        bytes[cursor++] = 0x02u;
        bytes[cursor++] = 0x21u; // mov.l r0,@r1
        const auto load_address = cursor;
        const auto base = (load_address + 4u) & ~3u;
        const auto literal_address = 12u;
        bytes[cursor++] = static_cast<std::uint8_t>((literal_address - base) / 4u);
        bytes[cursor++] = 0xD0u; // mov.l @(disp,pc),r0
        bytes[cursor++] = 0x2Bu;
        bytes[cursor++] = 0x40u; // jmp @r0
        bytes[cursor++] = 0x09u;
        bytes[cursor++] = 0x00u; // nop (delay)
        bytes[12u] = 0x10u;
        bytes[13u] = 0x00u;
        bytes[14u] = 0x00u;
        bytes[15u] = 0x00u;
        bytes[16u] = 0x0Bu;
        bytes[17u] = 0x00u;
        bytes[18u] = 0x09u;
        bytes[19u] = 0x00u;
        katana::io::ExecutableImage image;
        image.add_segment({".rwx",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Code,
                           {true, true, true},
                           std::move(bytes)});
        image.add_entry_point(0u);
        return image;
    };
    auto stable_entry = writable_entry_image(24u, true);
    const auto stable_without_contract = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(stable_entry.segments()[0].bytes, 0u), stable_entry);
    require(stable_without_contract[0].status == katana::analysis::ResolutionStatus::Guarded &&
                stable_without_contract[0].target == 0x10u &&
                stable_without_contract[0].reason == "guarded-writable-pc-relative-literal",
            "RWX-Literal wurde ohne Beweis weder dynamisch bewacht noch korrekt klassifiziert.");
    const auto guarded_flow = katana::analysis::analyze_control_flow(stable_entry);
    const auto guarded_edge = std::find_if(
        guarded_flow.resolved_edges.begin(), guarded_flow.resolved_edges.end(), [](const auto& edge) {
            return edge.instruction_address == 6u && edge.target_address == 0x10u;
        });
    require(std::any_of(guarded_flow.recursive.instructions.begin(),
                        guarded_flow.recursive.instructions.end(),
                        [](const auto& line) { return line.address == 0x10u; }) &&
                guarded_flow.indirect_control_flow[0].status ==
                    katana::analysis::ResolutionStatus::Guarded &&
                guarded_edge != guarded_flow.resolved_edges.end() && guarded_edge->guarded,
            "Bewachter Snapshotkandidat wurde nicht als partielle CFG-Kante erhalten.");
    const auto guarded_ir = katana::ir::lower_program(guarded_flow);
    require(!guarded_ir.empty(), "Bewachter Snapshotkandidat erzeugte keine IR-Funktion.");
    const auto guarded_ir_block = std::find_if(
        guarded_ir.front().blocks.begin(), guarded_ir.front().blocks.end(), [](const auto& block) {
            return std::any_of(block.instructions.begin(), block.instructions.end(), [](const auto& instruction) {
                return instruction.source_address == 6u;
            });
        });
    require(guarded_ir_block != guarded_ir.front().blocks.end(),
            "Bewachter Snapshotkandidat verlor seinen IR-Block.");
    const auto guarded_ir_control =
        std::find_if(guarded_ir_block->instructions.begin(),
                     guarded_ir_block->instructions.end(),
                     [](const auto& instruction) { return instruction.source_address == 6u; });
    require(guarded_ir_control != guarded_ir_block->instructions.end() &&
                guarded_ir_control->resolved_targets == std::vector<std::uint32_t>{0x10u} &&
                guarded_ir_block->has_indirect_successor &&
                katana::ir::verify_program(guarded_ir).empty(),
            "Bewachtes IR verlor Kandidatenziel oder dynamischen Fallback.");
    const auto guarded_text = katana::analysis::format_indirect_control_flow_report(
        guarded_flow.indirect_control_flow, guarded_flow.jump_tables);
    const auto guarded_json = katana::analysis::format_control_flow_analysis_json(guarded_flow);
    require(guarded_text.find("snapshot-candidate=") != std::string::npos &&
                guarded_json.find("\"status\":\"guarded\"") != std::string::npos,
            "Bewachter Kandidat fehlt im stabilen Text-/JSON-Bericht.");
    stable_entry.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLine);
    const auto stable_with_contract = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(stable_entry.segments()[0].bytes, 0u), stable_entry);
    require(stable_with_contract[0].status == katana::analysis::ResolutionStatus::Resolved &&
                stable_with_contract[0].target == 0x10u &&
                stable_with_contract[0].reason == "entry-snapshot-pc-relative-literal",
            "Nicht ueberdecktes Entry-Snapshot-Literal wurde nicht zeitlich bewiesen.");

    auto overwritten_entry = writable_entry_image(12u, true);
    overwritten_entry.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLine);
    const auto overwritten_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(overwritten_entry.segments()[0].bytes, 0u), overwritten_entry);
    require(overwritten_resolution[0].status == katana::analysis::ResolutionStatus::Guarded,
            "Vor dem Load ueberdecktes RWX-Literal wurde als statischer Beweis ausgegeben.");

    auto unknown_store_entry = writable_entry_image(0u, false);
    unknown_store_entry.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLine);
    const auto unknown_store_resolution = katana::analysis::resolve_indirect_control_flow(
        katana::sh4::disassemble(unknown_store_entry.segments()[0].bytes, 0u), unknown_store_entry);
    require(unknown_store_resolution[0].status == katana::analysis::ResolutionStatus::Guarded,
            "Unbekanntes Schreibziel wurde faelschlich als Entry-Snapshot-Beweis behandelt.");

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
    post_increment.instruction.kind = katana::sh4::InstructionKind::MovLongLoadPostIncrement;
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
    pre_decrement.instruction.kind = katana::sh4::InstructionKind::MovLongStorePreDecrement;
    pre_decrement.instruction.source_register = 2u;
    pre_decrement.instruction.destination_register = 15u;
    const std::array pre_decrement_lines{pre_decrement};
    const auto pre_decrement_trace =
        katana::analysis::propagate_local_constants(pre_decrement_lines, pre_decrement_initial);
    require(pre_decrement_trace[0].after.registers[2] == 0x22u &&
                !pre_decrement_trace[0].after.registers[15].has_value(),
            "Pre-Decrement invalidierte nicht ausschliesslich das aktualisierte Basisregister.");

    auto join_lines = katana::sh4::disassemble(
        std::array<std::uint8_t, 8>{0x0Cu, 0xE8u, 0x00u, 0x89u, 0x09u, 0x00u, 0x2Bu, 0x48u}, 0u);
    require(join_lines[1].target_address == 6u, "Synthetischer CFG-Join ist falsch kodiert.");
    const auto join_values = katana::analysis::analyze_register_values(join_lines);
    require(join_values.indirect_control_flow.size() == 1u &&
                !join_values.indirect_control_flow[0].value.has_value(),
            "Konstante wurde ueber einen mehrdeutigen CFG-Join getragen.");

    katana::io::ExecutableImage immutable_load;
    immutable_load.add_segment(
        {".text",
         0u,
         0u,
         0x20u,
         katana::io::SegmentKind::Code,
         {true, false, true},
         {0x20u, 0xE1u, 0x12u, 0x62u, 0x2Bu, 0x42u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u,
          0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
          0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u}});
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
