#include "katana/analysis/recursive_analysis.hpp"

#include <algorithm>
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
    using namespace katana::io;
    ExecutableImage image("recursive-fixture.bin");
    image.add_segment({".text",
                       0x8C010000u,
                       0u,
                       14u,
                       SegmentKind::Code,
                       {true, false, true},
                       {0x02u,
                        0xB0u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u,
                        0x09u,
                        0x00u,
                        0x0Bu,
                        0x00u,
                        0x09u,
                        0x00u,
                        0xFFu,
                        0xFFu}});
    image.add_segment(
        {".data", 0x8C020000u, 14u, 4u, SegmentKind::Data, {true, true, false}, {1u, 2u, 3u, 4u}});
    image.add_segment({".mystery",
                       0x8C030000u,
                       18u,
                       4u,
                       SegmentKind::Unknown,
                       {true, false, false},
                       {5u, 6u, 7u, 8u}});
    image.add_entry_point(0x8C010000u);
    image.add_symbol({"subroutine", 0x8C010008u, 4u, SymbolKind::Function, SymbolBinding::Global});

    const auto result = katana::analysis::analyze_reachable_code(image);
    require(result.instructions.size() == 6u,
            "Die Worklist hat nicht genau den erreichbaren Code entdeckt.");
    require(result.instructions.front().address == 0x8C010000u, "Der Einstiegspunkt fehlt.");
    require(result.instructions[1].is_delay_slot, "Der Call-Delay-Slot wurde nicht markiert.");
    require(result.instructions[4].address == 0x8C010008u,
            "Das direkte Callziel wurde nicht verfolgt.");
    require(result.instructions.back().address == 0x8C01000Au,
            "Der Delay-Slot des Callziels fehlt.");

    for (const auto& line : result.instructions) {
        require(line.address != 0x8C01000Cu, "Nicht erreichbare Bytes wurden linear dekodiert.");
    }
    require(result.ranges.size() == 4u, "Die Klassifikationsbereiche wurden nicht normalisiert.");
    require(result.ranges[0].start_address == 0x8C010000u && result.ranges[0].size == 12u &&
                result.ranges[0].kind == katana::analysis::DiscoveredByteKind::Code,
            "Erreichbarer Code wurde falsch klassifiziert.");
    require(result.ranges[1].start_address == 0x8C01000Cu && result.ranges[1].size == 2u &&
                result.ranges[1].kind == katana::analysis::DiscoveredByteKind::Unknown,
            "Nicht erreichbarer Codebereich wurde nicht als unknown erhalten.");
    require(result.ranges[2].kind == katana::analysis::DiscoveredByteKind::Data,
            "Datensegment wurde falsch klassifiziert.");
    require(result.ranges[3].kind == katana::analysis::DiscoveredByteKind::Unknown,
            "Unknown-Segment wurde falsch klassifiziert.");
    require(result.unreachable_code.size() == 1u &&
                result.unreachable_code[0].start_address == 0x8C01000Cu &&
                result.unreachable_code[0].size == 2u,
            "Der nicht erreichbare committed Codebereich wurde falsch ermittelt.");
    require(std::string(katana::analysis::discovered_byte_kind_name(result.ranges[2].kind)) ==
                "data",
            "Klassifikationsname ist instabil.");
    require(result.functions.size() == 2u, "Funktionskandidaten wurden nicht zusammengefuehrt.");
    require(result.functions[0].address == 0x8C010000u &&
                result.functions[0].confidence == katana::analysis::AnalysisConfidence::Certain &&
                result.functions[0].origins ==
                    std::vector<katana::analysis::FunctionOrigin>{
                        katana::analysis::FunctionOrigin::EntryPoint},
            "Einstiegspunkt-Herkunft oder Konfidenz ist falsch.");
    require(result.functions[1].address == 0x8C010008u &&
                result.functions[1].confidence == katana::analysis::AnalysisConfidence::High &&
                result.functions[1].origins.size() == 2u,
            "Call- und Symbolherkunft wurden nicht kombiniert.");
    require(std::string(katana::analysis::function_origin_name(result.functions[1].origins[0])) ==
                    "direct-call" &&
                std::string(katana::analysis::analysis_confidence_name(
                    result.functions[0].confidence)) == "certain",
            "Herkunfts- oder Konfidenzname ist instabil.");

    katana::analysis::RecursiveAnalysisOptions guarded_options;
    guarded_options.additional_seeds.push_back(
        {0x8C01000Cu, {katana::analysis::FunctionOrigin::GuardedSnapshot}});
    const auto guarded_result = katana::analysis::analyze_reachable_code(image, guarded_options);
    const auto guarded_function =
        std::find_if(guarded_result.functions.begin(),
                     guarded_result.functions.end(),
                     [](const auto& function) { return function.address == 0x8C01000Cu; });
    require(guarded_function != guarded_result.functions.end() &&
                guarded_function->confidence == katana::analysis::AnalysisConfidence::Medium &&
                guarded_function->origins ==
                    std::vector{katana::analysis::FunctionOrigin::GuardedSnapshot} &&
                std::string(katana::analysis::function_origin_name(
                    katana::analysis::FunctionOrigin::GuardedSnapshot)) == "guarded-snapshot",
            "Bewachte Snapshotfunktion verlor Herkunft oder mittlere Konfidenz.");

    ExecutableImage invalid;
    invalid.add_segment(
        {".data", 0x1000u, 0u, 2u, SegmentKind::Data, {true, true, false}, {0u, 0u}});
    invalid.add_entry_point(0x1000u);
    bool rejected = false;
    try {
        static_cast<void>(katana::analysis::analyze_reachable_code(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Ein Einstiegspunkt in Daten wurde akzeptiert.");

    ExecutableImage overlap;
    overlap.add_segment(
        {".text",
         0x1000u,
         0u,
         12u,
         SegmentKind::Code,
         {true, false, true},
         {0x02u, 0xB0u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    overlap.add_entry_point(0x1002u);
    overlap.add_symbol({"owner", 0x1000u, 4u, SymbolKind::Function, SymbolBinding::Global});
    const auto overlap_result = katana::analysis::analyze_reachable_code(overlap);
    require(overlap_result.conflicts.size() == 1u &&
                overlap_result.conflicts[0].address == 0x1002u &&
                overlap_result.conflicts[0].size == 2u &&
                overlap_result.conflicts[0].kind ==
                    katana::analysis::AnalysisConflictKind::FunctionEntryInDelaySlot,
            "Ueberlappende Funktions- und Delay-Slot-Rollen wurden nicht gemeldet.");
    require(std::string(katana::analysis::analysis_conflict_kind_name(
                overlap_result.conflicts[0].kind)) == "function-entry-in-delay-slot",
            "Konfliktname ist instabil.");
    const auto normal_context = std::count_if(overlap_result.contextual_instructions.begin(),
                                              overlap_result.contextual_instructions.end(),
                                              [](const auto& contextual) {
                                                  return contextual.line.address == 0x1002u &&
                                                         !contextual.delay_slot_owner.has_value();
                                              });
    const auto slot_context = std::count_if(overlap_result.contextual_instructions.begin(),
                                            overlap_result.contextual_instructions.end(),
                                            [](const auto& contextual) {
                                                return contextual.line.address == 0x1002u &&
                                                       contextual.delay_slot_owner == 0x1000u;
                                            });
    require(
        normal_context == 1 && slot_context == 1,
        "Dieselbe Adresse wurde nicht getrennt als normale Instruktion und Delay Slot erfasst.");
    const auto report = katana::analysis::format_recursive_analysis_report(result);
    require(report == katana::analysis::format_recursive_analysis_report(result),
            "Analysebericht ist nicht deterministisch.");
    require(report.find("Funktion 0x8C010000 Konfidenz=certain Evidenz=proven-complete "
                        "Herkunft=entry-point") != std::string::npos,
            "Analysebericht erklaert den Einstiegspunkt nicht.");
    require(report.find("Funktion 0x8C010008 Konfidenz=high Evidenz=proven-complete "
                        "Herkunft=direct-call,symbol") != std::string::npos,
            "Analysebericht erklaert den Call-/Symbolkandidaten nicht.");
    require(report.find("Unerreichbar 0x8C01000C Groesse=2") != std::string::npos,
            "Unerreichbarer Bereich fehlt im Bericht.");

    ExecutableImage unknown;
    unknown.add_segment({".text",
                         0u,
                         0u,
                         6u,
                         SegmentKind::Code,
                         {true, false, true},
                         {0x09u, 0x00u, 0xFFu, 0xFFu, 0x09u, 0x00u}});
    unknown.add_entry_point(0u);
    const auto unknown_result = katana::analysis::analyze_reachable_code(unknown);
    require(unknown_result.instructions.size() == 2u &&
                unknown_result.instructions.back().address == 2u,
            "Unbekannter Opcode hat den linearen Analysepfad nicht beendet.");
    require(unknown_result.diagnostics.size() == 1u &&
                unknown_result.diagnostics[0].address == 2u &&
                unknown_result.diagnostics[0].opcode == 0xFFFFu &&
                unknown_result.diagnostics[0].reason == "unknown-opcode",
            "Unbekannter Opcode wurde nicht stabil diagnostiziert.");
    const auto unknown_report = katana::analysis::format_recursive_analysis_report(unknown_result);
    require(unknown_report.find("Diagnose 0x00000002 Opcode=0xFFFF Grund=unknown-opcode") !=
                std::string::npos,
            "Bericht nennt Adresse, Opcode und Abbruchgrund nicht.");

    ExecutableImage alternate;
    alternate.add_segment({".text",
                           0u,
                           0u,
                           10u,
                           SegmentKind::Code,
                           {true, false, true},
                           {0x09u, 0x00u, 0xFFu, 0xFFu, 0x0Bu, 0x00u, 0xFDu, 0xAFu, 0x09u, 0x00u}});
    alternate.add_entry_point(0u);
    alternate.add_entry_point(6u);
    const auto alternate_result = katana::analysis::analyze_reachable_code(alternate);
    require(std::any_of(alternate_result.instructions.begin(),
                        alternate_result.instructions.end(),
                        [](const auto& line) { return line.address == 4u; }),
            "Ein separates direktes Sprungziel hinter einem unbekannten Opcode wurde nicht "
            "analysiert.");

    ExecutableImage unknown_delay;
    unknown_delay.add_segment(
        {".text",
         0u,
         0u,
         12u,
         SegmentKind::Code,
         {true, false, true},
         {0x02u, 0xB0u, 0xFFu, 0xFFu, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    unknown_delay.add_entry_point(0u);
    const auto unknown_delay_result = katana::analysis::analyze_reachable_code(unknown_delay);
    require(unknown_delay_result.instructions.size() == 2u &&
                unknown_delay_result.instructions[1].is_delay_slot &&
                unknown_delay_result.diagnostics.size() == 2u &&
                std::any_of(
                    unknown_delay_result.diagnostics.begin(),
                    unknown_delay_result.diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.reason == "unknown-opcode"; }) &&
                std::any_of(unknown_delay_result.diagnostics.begin(),
                            unknown_delay_result.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.reason == "delay-slot-unknown-opcode";
                            }),
            "Unbekannter Delay Slot wurde nicht vollstaendig diagnostiziert.");
    require(std::none_of(unknown_delay_result.instructions.begin(),
                         unknown_delay_result.instructions.end(),
                         [](const auto& line) { return line.address == 4u || line.address == 8u; }),
            "Unbekannter Delay Slot liess unsicheren Call- oder Rueckkehrpfad weiterlaufen.");

    ExecutableImage control_in_delay;
    control_in_delay.add_segment({".text",
                                  0u,
                                  0u,
                                  8u,
                                  SegmentKind::Code,
                                  {true, false, true},
                                  {0x00u, 0xB0u, 0xFDu, 0xAFu, 0x09u, 0x00u, 0x09u, 0x00u}});
    control_in_delay.add_entry_point(0u);
    const auto control_in_delay_result = katana::analysis::analyze_reachable_code(control_in_delay);
    require(std::any_of(control_in_delay_result.diagnostics.begin(),
                        control_in_delay_result.diagnostics.end(),
                        [](const auto& diagnostic) {
                            return diagnostic.address == 2u &&
                                   diagnostic.reason == "control-flow-in-delay-slot";
                        }),
            "Kontrollfluss im Delay Slot wurde nicht kontexttreu diagnostiziert.");

    ExecutableImage missing_delay;
    missing_delay.add_segment(
        {".text", 0u, 0u, 2u, SegmentKind::Code, {true, false, true}, {0x00u, 0xB0u}});
    missing_delay.add_entry_point(0u);
    const auto missing_delay_result = katana::analysis::analyze_reachable_code(missing_delay);
    require(missing_delay_result.diagnostics.size() == 1u &&
                missing_delay_result.diagnostics.front().address == 0u &&
                missing_delay_result.diagnostics.front().reason == "delay-slot-unavailable",
            "Fehlender Delay Slot wurde nicht am Owner sichtbar diagnostiziert.");

    ExecutableImage prefetch;
    prefetch.add_segment({".text",
                          0u,
                          0u,
                          8u,
                          SegmentKind::Code,
                          {true, false, true},
                          {0x83u, 0x03u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    prefetch.add_entry_point(0u);
    const auto prefetch_result = katana::analysis::analyze_reachable_code(prefetch);
    require(prefetch_result.instructions.size() == 4u &&
                prefetch_result.instructions[0].instruction.kind ==
                    katana::sh4::InstructionKind::Prefetch &&
                prefetch_result.instructions.back().address == 6u &&
                prefetch_result.diagnostics.empty(),
            "PREF beendet die rekursive Analyse faelschlich als unbekannter Opcode.");

    std::cout << "KR-1701 Worklist ab Einstiegspunkten erfolgreich.\n";
    return EXIT_SUCCESS;
}
