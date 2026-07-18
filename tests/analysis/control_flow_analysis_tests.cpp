#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"

#include <algorithm>
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

bool has_instruction(const katana::analysis::ControlFlowAnalysisResult& analysis,
                     const std::uint32_t address) {
    return std::any_of(analysis.recursive.instructions.begin(),
                       analysis.recursive.instructions.end(),
                       [address](const auto& line) { return line.address == address; });
}

const katana::analysis::FunctionCandidate*
find_function(const katana::analysis::ControlFlowAnalysisResult& analysis,
              const std::uint32_t address) {
    const auto iterator =
        std::find_if(analysis.recursive.functions.begin(),
                     analysis.recursive.functions.end(),
                     [address](const auto& function) { return function.address == address; });
    return iterator == analysis.recursive.functions.end() ? nullptr : &*iterator;
}

katana::io::ExecutableImage code_image(std::vector<std::uint8_t> bytes) {
    katana::io::ExecutableImage image;
    image.add_segment({".text",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    return image;
}

template <typename Function> std::string failure(Function&& function) {
    try {
        function();
    } catch (const std::exception& error) {
        return error.what();
    }
    require(false, "Erwarteter Analysefehler blieb aus.");
    return {};
}

} // namespace

int main() {
    auto jump_image = code_image(
        {0x08u, 0xE1u, 0x2Bu, 0x41u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    const auto jump = katana::analysis::analyze_control_flow(jump_image);
    require(has_instruction(jump, 8u), "Aufgeloestes indirektes JMP-Ziel wurde nicht entdeckt.");
    require(!has_instruction(jump, 6u), "Indirektes JMP erzeugte falschen Fallthrough.");
    require(has_instruction(jump, 4u), "Delay Slot des indirekten JMP fehlt.");
    require(jump.indirect_control_flow.size() == 1u, "Indirektes JMP wurde doppelt aufgeloest.");

    auto pc_literal_jump = code_image({0x01u,
                                       0xD1u,
                                       0x2Bu,
                                       0x41u,
                                       0x09u,
                                       0x00u,
                                       0x09u,
                                       0x00u,
                                       0x0Cu,
                                       0x00u,
                                       0x00u,
                                       0x00u,
                                       0x09u,
                                       0x00u,
                                       0x0Bu,
                                       0x00u});
    const auto pc_literal_flow = katana::analysis::analyze_control_flow(pc_literal_jump);
    require(has_instruction(pc_literal_flow, 12u) &&
                pc_literal_flow.indirect_control_flow.size() == 1u &&
                pc_literal_flow.indirect_control_flow[0].reason == "pc-relative-literal",
            "PC-relatives indirektes Ziel setzte die rekursive Analyse nicht fort.");

    katana::analysis::AnalysisOverrides hints;
    hints.version = 2u;
    hints.mode = katana::analysis::AnalysisDirectiveMode::Hint;
    hints.source_path = "synthetic-hints.txt";
    hints.functions.push_back({8u, 1u});
    hints.jumps.push_back({2u, 8u, 2u});
    hints.jumps.push_back({2u, 10u, 3u});
    hints.jumps.push_back({6u, 8u, 4u});
    const auto hinted = katana::analysis::analyze_control_flow(jump_image, &hints);
    require(hinted.indirect_control_flow.size() == 1u &&
                hinted.indirect_control_flow[0].target == 8u,
            "Abweichender Hint hat ein statisch bewiesenes Sprungziel ueberschrieben.");
    for (const auto status : {katana::analysis::AnalysisDirectiveDiagnosticStatus::Accepted,
                              katana::analysis::AnalysisDirectiveDiagnosticStatus::Confirmed,
                              katana::analysis::AnalysisDirectiveDiagnosticStatus::Rejected,
                              katana::analysis::AnalysisDirectiveDiagnosticStatus::Stale}) {
        require(
            std::any_of(hinted.directive_diagnostics.begin(),
                        hinted.directive_diagnostics.end(),
                        [status](const auto& diagnostic) { return diagnostic.status == status; }),
            "Hintdiagnostik verliert eine der vier stabilen Statusklassen.");
    }
    const auto hint_json = katana::analysis::format_control_flow_analysis_json(hinted);
    require(hint_json.find("\"status\":\"accepted\"") != std::string::npos &&
                hint_json.find("\"status\":\"confirmed\"") != std::string::npos &&
                hint_json.find("\"status\":\"rejected\"") != std::string::npos &&
                hint_json.find("\"status\":\"stale\"") != std::string::npos &&
                hint_json.find("\"line\":4") != std::string::npos,
            "Analyse-JSON serialisiert Hintstatus, Zeile oder Grund nicht vollstaendig.");

    auto call_image = code_image({0x0Cu,
                                  0xE1u,
                                  0x0Bu,
                                  0x41u,
                                  0x09u,
                                  0x00u,
                                  0x09u,
                                  0x00u,
                                  0x0Bu,
                                  0x00u,
                                  0x09u,
                                  0x00u,
                                  0x0Bu,
                                  0x00u,
                                  0x09u,
                                  0x00u});
    const auto call = katana::analysis::analyze_control_flow(call_image);
    require(has_instruction(call, 12u), "Aufgeloestes indirektes JSR-Ziel wurde nicht entdeckt.");
    require(has_instruction(call, 6u), "Rueckkehrpfad des indirekten JSR fehlt.");
    require(has_instruction(call, 4u), "Delay Slot des indirekten JSR fehlt.");
    const auto* call_function = find_function(call, 12u);
    if (call_function == nullptr) {
        throw std::runtime_error("Indirektes JSR-Ziel ist kein Funktionskandidat.");
    }
    require(call_function->origins ==
                std::vector<katana::analysis::FunctionOrigin>{
                    katana::analysis::FunctionOrigin::IndirectCall},
            "Symbolfreies automatisches JSR-Ziel wurde nicht allein als indirekter Call entdeckt.");
    call_image.add_symbol({"indirect_target",
                           12u,
                           4u,
                           katana::io::SymbolKind::Function,
                           katana::io::SymbolBinding::Global});
    const auto call_with_symbol = katana::analysis::analyze_control_flow(call_image);
    const auto* merged_function = find_function(call_with_symbol, 12u);
    require(merged_function != nullptr && merged_function->origins ==
                                              std::vector<katana::analysis::FunctionOrigin>{
                                                  katana::analysis::FunctionOrigin::IndirectCall,
                                                  katana::analysis::FunctionOrigin::Symbol},
            "Indirekte Call- und Symbolherkunft wurden nicht zusammengefuehrt.");

    auto chain_image =
        code_image({0x08u, 0xE1u, 0x2Bu, 0x41u, 0x09u, 0x00u, 0x09u, 0x00u, 0x10u, 0xE2u,
                    0x2Bu, 0x42u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    const auto chain = katana::analysis::analyze_control_flow(chain_image);
    require(has_instruction(chain, 16u), "Kette indirekter Ziele erreichte den Fixpunkt nicht.");
    require(chain.fixpoint_iterations > 0u && chain.fixpoint_iterations <= 16u,
            "Kontrollflussanalyse terminiert nicht innerhalb des unabhaengigen Budgets.");
    require(chain.indirect_control_flow.size() == 2u,
            "Fixpunkt duplizierte oder verlor Aufloesungen.");

    auto cycle_image = code_image({0x00u, 0xE1u, 0x2Bu, 0x41u, 0x09u, 0x00u});
    const auto cycle = katana::analysis::analyze_control_flow(cycle_image);
    require(cycle.fixpoint_iterations > 0u && cycle.fixpoint_iterations <= 16u &&
                cycle.recursive.instructions.size() == 3u,
            "Identisches indirektes Quell- und Zielgebiet terminiert nicht im Ergebnisbudget.");

    auto override_image = code_image(
        {0x2Bu, 0x41u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    katana::analysis::AnalysisOverrides jump_hint;
    jump_hint.version = 2u;
    jump_hint.mode = katana::analysis::AnalysisDirectiveMode::Hint;
    jump_hint.source_path = "jump-hint.txt";
    jump_hint.functions.push_back({8u, 1u});
    jump_hint.jumps.push_back({0u, 8u, 2u});
    const auto hinted_jump = katana::analysis::analyze_control_flow(override_image, &jump_hint);
    require(hinted_jump.indirect_control_flow.size() == 1u &&
                hinted_jump.indirect_control_flow[0].status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                hinted_jump.indirect_control_flow[0].evidence ==
                    katana::analysis::ControlFlowEvidence::HintCandidate &&
                has_instruction(hinted_jump, 8u),
            "Ein Hint wurde als Beweis behandelt oder nicht als Kandidat decodiert.");
    const auto hinted_jump_ir = katana::ir::lower_program(hinted_jump);
    require(hinted_jump_ir.size() == 1u &&
                hinted_jump_ir.front().blocks.front().has_indirect_successor,
            "Hint erzeugt eine harte Funktionsgrenze oder entfernt den Runtime-Default.");
    const auto hinted_detail = katana::analysis::format_control_flow_analysis_json(hinted_jump);
    const auto hinted_frontier = katana::analysis::format_control_flow_frontier_json(hinted_jump);
    const auto hinted_summary = katana::analysis::summarize_control_flow_analysis(hinted_jump);
    require(hinted_summary.indirect_total == 1u && hinted_summary.guarded_partial == 1u &&
                hinted_summary.unresolved == 0u &&
                hinted_summary.resolved + hinted_summary.guarded_complete +
                        hinted_summary.guarded_partial + hinted_summary.runtime_only +
                        hinted_summary.unresolved ==
                    hinted_summary.indirect_total &&
                hinted_detail.find("\"status\":\"guarded_partial\"") != std::string::npos &&
                hinted_detail.find("\"evidence\":\"hint-candidate\"") != std::string::npos &&
                hinted_detail.find("\"targets\":[\"0x00000008\"]") != std::string::npos &&
                hinted_frontier.find("\"guarded_partial\":1") != std::string::npos &&
                hinted_frontier.find("0x00000008") == std::string::npos &&
                hinted_frontier.find("jump-hint.txt") == std::string::npos,
            "Validierter Hint verletzt Detail-, Aggregat- oder Summenvertrag.");

    katana::analysis::AnalysisOverrides jump_override;
    jump_override.source_path = "override-test.txt";
    jump_override.jumps.push_back({0u, 8u, 7u});
    jump_override.functions.push_back({8u, 6u});
    const auto overridden = katana::analysis::analyze_control_flow(override_image, &jump_override);
    require(has_instruction(overridden, 8u),
            "Override-Jumpziel wurde nicht in die Worklist gespeist.");
    require(!has_instruction(overridden, 4u), "Override-JMP erzeugte falschen Fallthrough.");
    require(overridden.indirect_control_flow[0].reason == "user-override",
            "Override-Aufloesung ist im Berichtsdatenmodell nicht sichtbar.");
    require(overridden.resolved_edges.size() == 1u &&
                overridden.resolved_edges[0].instruction_address == 0u &&
                overridden.resolved_edges[0].target_address == 8u &&
                overridden.resolved_edges[0].evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride,
            "Override-Ziel wurde nicht als echte CFG-Kante materialisiert.");
    require(find_function(overridden, 8u) != nullptr &&
                find_function(overridden, 8u)->origins ==
                    std::vector<katana::analysis::FunctionOrigin>{
                        katana::analysis::FunctionOrigin::UserOverride},
            "Function-Override ist nicht als Nutzerherkunft sichtbar.");
    const auto overridden_ir = katana::ir::lower_program(overridden);
    require(overridden.indirect_control_flow[0].status ==
                    katana::analysis::ResolutionStatus::Guarded &&
                overridden.indirect_control_flow[0].evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride &&
                overridden_ir.size() == 1u &&
                overridden_ir.front().blocks.front().has_indirect_successor,
            "Forced Override entfernt den dynamischen Runtime-Default.");

    auto override_call_image = code_image(
        {0x0Bu, 0x41u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    katana::analysis::AnalysisOverrides call_override;
    call_override.source_path = "call-override.txt";
    call_override.jumps.push_back({0u, 8u, 2u});
    const auto overridden_call =
        katana::analysis::analyze_control_flow(override_call_image, &call_override);
    require(has_instruction(overridden_call, 4u) && has_instruction(overridden_call, 8u),
            "JSR-Override verlor Rueckkehrpfad oder Callziel.");
    require(find_function(overridden_call, 8u) != nullptr &&
                find_function(overridden_call, 8u)->origins ==
                    std::vector<katana::analysis::FunctionOrigin>{
                        katana::analysis::FunctionOrigin::IndirectCall,
                        katana::analysis::FunctionOrigin::UserOverride},
            "JSR-Override wurde als Jump statt Call klassifiziert.");

    katana::io::ExecutableImage table_jump_image;
    table_jump_image.add_segment({".text",
                                  0u,
                                  0u,
                                  16u,
                                  katana::io::SegmentKind::Code,
                                  {true, false, true},
                                  {0x2Bu,
                                   0x41u,
                                   0x09u,
                                   0x00u,
                                   0x09u,
                                   0x00u,
                                   0x09u,
                                   0x00u,
                                   0x0Bu,
                                   0x00u,
                                   0x09u,
                                   0x00u,
                                   0x0Bu,
                                   0x00u,
                                   0x09u,
                                   0x00u}});
    table_jump_image.add_segment({".table",
                                  0x100u,
                                  16u,
                                  8u,
                                  katana::io::SegmentKind::Data,
                                  {true, false, false},
                                  {0x08u, 0x00u, 0x00u, 0x00u, 0x0Cu, 0x00u, 0x00u, 0x00u}});
    table_jump_image.add_entry_point(0u);
    katana::analysis::AnalysisOverrides table_override;
    table_override.source_path = "table-test.txt";
    table_override.jump_tables.push_back({0u, 0x100u, 2u, 3u});
    const auto table_jump =
        katana::analysis::analyze_control_flow(table_jump_image, &table_override);
    require(has_instruction(table_jump, 8u) && has_instruction(table_jump, 12u),
            "Gueltige Jump-Table-Ziele wurden nicht vollstaendig entdeckt.");
    require(find_function(table_jump, 8u) == nullptr && find_function(table_jump, 12u) == nullptr,
            "JMP-Tabelle erzeugte falsche Call-Kandidaten.");
    const auto table_jump_ir = katana::ir::lower_program(table_jump);
    require(table_jump_ir.size() == 1u &&
                table_jump_ir.front().blocks.front().successors ==
                    std::vector<std::uint32_t>{8u, 12u} &&
                katana::ir::verify_program(table_jump_ir).empty(),
            "Jump-Table-Ziele erreichen CFG oder Lowering nicht konsistent.");

    auto partial_table_image = table_jump_image;
    partial_table_image.write_u32_le(0x104u, 0x200u);
    const auto partial_table =
        katana::analysis::analyze_control_flow(partial_table_image, &table_override);
    require(partial_table.jump_tables.size() == 1u && !partial_table.jump_tables[0].resolved &&
                !has_instruction(partial_table, 8u) &&
                partial_table.indirect_control_flow[0].origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Table &&
                partial_table.indirect_control_flow[0].evidence_origins ==
                    std::vector{katana::analysis::AnalysisEvidenceOrigin::UserOverride},
            "Teilweise ungueltige Jump Table speiste sichere Teilziele in die Worklist.");

    katana::io::ExecutableImage writable_table_image;
    writable_table_image.add_segment({".text",
                                      0u,
                                      0u,
                                      16u,
                                      katana::io::SegmentKind::Code,
                                      {true, false, true},
                                      {0x2Bu,
                                       0x41u,
                                       0x09u,
                                       0x00u,
                                       0x09u,
                                       0x00u,
                                       0x09u,
                                       0x00u,
                                       0x0Bu,
                                       0x00u,
                                       0x09u,
                                       0x00u,
                                       0x0Bu,
                                       0x00u,
                                       0x09u,
                                       0x00u}});
    writable_table_image.add_segment({".ram-table",
                                      0x100u,
                                      16u,
                                      8u,
                                      katana::io::SegmentKind::Data,
                                      {true, true, false},
                                      {0x08u, 0x00u, 0x00u, 0x00u, 0x0Cu, 0x00u, 0x00u, 0x00u}});
    writable_table_image.add_entry_point(0u);
    const auto writable_table =
        katana::analysis::analyze_control_flow(writable_table_image, &table_override);
    require(
        writable_table.indirect_control_flow.size() == 1u &&
            writable_table.indirect_control_flow[0].origin_class ==
                katana::analysis::IndirectControlFlowOriginClass::Table &&
            writable_table.indirect_control_flow[0].evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            writable_table.indirect_control_flow[0].reason == "dynamic-writable-table" &&
            writable_table.indirect_control_flow[0].targets.empty() &&
            katana::analysis::control_flow_report_status(writable_table.indirect_control_flow[0]) ==
                katana::analysis::ControlFlowReportStatus::RuntimeOnly,
        "Beschreibbare Jump Table wurde eingefroren oder blieb ohne sicheren Runtimevertrag.");
    const auto writable_table_ir = katana::ir::lower_program(writable_table);
    require(
        writable_table_ir.size() == 1u &&
            writable_table_ir.front().blocks.front().instructions.front().dynamic_target_class ==
                katana::ir::DynamicTargetClass::RuntimeOnly &&
            writable_table_ir.front()
                .blocks.front()
                .instructions.front()
                .resolved_targets.empty() &&
            katana::ir::verify_program(writable_table_ir).empty(),
        "Beschreibbare Jump Table erreicht nicht kandidatenfrei den Runtime-only-Dispatcher.");

    auto table_call_image = code_image({0x0Bu, 0x41u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u,
                                        0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u,
                                        0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u});
    table_call_image.add_segment({".table",
                                  0x100u,
                                  24u,
                                  8u,
                                  katana::io::SegmentKind::Data,
                                  {true, false, false},
                                  {0x0Cu, 0x00u, 0x00u, 0x00u, 0x14u, 0x00u, 0x00u, 0x00u}});
    const auto table_call =
        katana::analysis::analyze_control_flow(table_call_image, &table_override);
    const auto* table_function = find_function(table_call, 12u);
    if (table_function == nullptr) {
        throw std::runtime_error("JSR-Tabelle erzeugte keinen Funktionskandidaten.");
    }
    require(table_function->origins ==
                std::vector<katana::analysis::FunctionOrigin>{
                    katana::analysis::FunctionOrigin::JumpTableCall,
                    katana::analysis::FunctionOrigin::UserOverride},
            "Call-Tabellen-Herkunft wurde nicht deterministisch zusammengefuehrt.");
    const auto table_call_ir = katana::ir::lower_program(table_call);
    const auto main_ir =
        std::find_if(table_call_ir.begin(), table_call_ir.end(), [](const auto& function) {
            return function.entry_address == 0u;
        });
    require(main_ir != table_call_ir.end(), "Call-Tabelle besitzt keine IR-Hauptfunktion.");
    require(main_ir->direct_callees == std::vector<std::uint32_t>{12u, 20u},
            "Call-Tabelle liefert falsche direkte Callee-Metadaten.");
    require(main_ir->indirect_call_sites == std::vector<std::uint32_t>{0u},
            "Call-Tabelle liefert falsche indirekte Callsite-Metadaten.");
    const auto table_call_issues = katana::ir::verify_program(table_call_ir);
    for (const auto& issue : table_call_issues) {
        std::cerr << "IR-VERIFIER: " << issue.address << ": " << issue.message << '\n';
    }
    require(table_call_issues.empty(), "Call-Tabellen-IR ist laut Verifier inkonsistent.");

    katana::analysis::AnalysisOverrides bad_dispatch;
    bad_dispatch.source_path = "bad-overrides.txt";
    bad_dispatch.jump_tables.push_back({4u, 0x100u, 1u, 11u});
    auto error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(table_jump_image, &bad_dispatch));
    });
    require(error.find("bad-overrides.txt") != std::string::npos &&
                error.find("Zeile 11") != std::string::npos &&
                error.find("0x00000004") != std::string::npos &&
                error.find("dispatch-not-discovered") != std::string::npos,
            "Nicht entdeckter Dispatch wurde nicht grundgenau diagnostiziert.");

    bad_dispatch.jump_tables[0] = {2u, 0x100u, 1u, 12u};
    error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(table_jump_image, &bad_dispatch));
    });
    require(error.find("dispatch-not-jmp-or-jsr") != std::string::npos,
            "Normaler Befehl wurde als Jump-Table-Dispatch akzeptiert.");

    katana::io::ExecutableImage zero_fill;
    zero_fill.add_segment({".text",
                           0u,
                           0u,
                           16u,
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {0x2Bu, 0x41u, 0x09u, 0x00u}});
    zero_fill.add_entry_point(0u);
    katana::analysis::AnalysisOverrides zero_override;
    zero_override.source_path = "zero-overrides.txt";
    zero_override.functions.push_back({8u, 4u});
    error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(zero_fill, &zero_override));
    });
    require(error.find("outside-committed-data") != std::string::npos &&
                error.find("Zeile 4") != std::string::npos,
            "Function-Override im Zero-Fill wurde nicht grundgenau abgelehnt.");
    zero_override.functions.clear();
    zero_override.jumps.push_back({0u, 8u, 5u});
    error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(zero_fill, &zero_override));
    });
    require(error.find("outside-committed-data") != std::string::npos &&
                error.find("Zeile 5") != std::string::npos,
            "Jump-Override im Zero-Fill wurde nicht grundgenau abgelehnt.");
    zero_override.jumps.clear();
    zero_override.jump_tables.push_back({8u, 0x100u, 1u, 6u});
    error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(zero_fill, &zero_override));
    });
    require(error.find("outside-committed-data") != std::string::npos &&
                error.find("Zeile 6") != std::string::npos,
            "Jump-Table-Dispatch im Zero-Fill wurde akzeptiert.");
    zero_override.jump_tables[0] = {0x200u, 0x100u, 1u, 7u};
    error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(zero_fill, &zero_override));
    });
    require(error.find("outside-segments") != std::string::npos,
            "Jump-Table-Dispatch ausserhalb aller Segmente wurde akzeptiert.");

    std::cout << "v0.18 Kontrollfluss-Fixpunkt erfolgreich.\n";
    return EXIT_SUCCESS;
}
