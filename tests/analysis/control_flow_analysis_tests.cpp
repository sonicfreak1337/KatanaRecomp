#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>

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

#ifdef _MSC_VER
#pragma warning(suppress : 6262) // Deliberately comprehensive analysis-regression driver.
#endif
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

    const auto absolute_snapshot_image = [](const katana::io::ImageSourceKind source_kind,
                                            const katana::io::ImageLoadPhase load_phase,
                                            const katana::io::InitialSnapshotPolicy policy) {
        constexpr std::uint32_t base = 0xAC100000u;
        std::vector<std::uint8_t> bytes(0x40u, 0u);
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        bytes[0x00u] = 0x03u;
        bytes[0x01u] = 0xD3u;
        bytes[0x02u] = 0x3Eu;
        bytes[0x03u] = 0x03u;
        bytes[0x04u] = 0xFCu;
        bytes[0x05u] = 0x70u;
        bytes[0x06u] = 0x2Bu;
        bytes[0x07u] = 0x43u;
        bytes[0x08u] = 0x09u;
        bytes[0x09u] = 0x00u;
        put_u32(0x10u, 0x8C100020u);
        put_u32(0x20u, 0x8C100030u);
        put_u32(0x24u, 0x8C100034u);
        bytes[0x30u] = 0x09u;
        bytes[0x31u] = 0x00u;
        bytes[0x32u] = 0x09u;
        bytes[0x33u] = 0x00u;
        bytes[0x34u] = 0x0Bu;
        bytes[0x35u] = 0x00u;
        bytes[0x36u] = 0x09u;
        bytes[0x37u] = 0x00u;
        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(policy);
        image.set_address_model(katana::io::ImageAddressModel::Sh4DirectMapped);
        image.add_segment({".synthetic-disc-bootstrap",
                           base,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           source_kind,
                           load_phase,
                           "synthetic-disc-bootstrap"});
        image.add_entry_point(base);
        return image;
    };
    const auto absolute_snapshot = katana::analysis::analyze_control_flow(
        absolute_snapshot_image(
            katana::io::ImageSourceKind::DiscBootFile,
            katana::io::ImageLoadPhase::Initial,
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent));
    constexpr std::uint32_t absolute_dispatch = 0xAC100006u;
    const std::vector<std::uint32_t> absolute_targets{0xAC100030u, 0xAC100034u};
    const auto absolute_table = std::find_if(
        absolute_snapshot.jump_tables.begin(),
        absolute_snapshot.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == absolute_dispatch; });
    require(absolute_table != absolute_snapshot.jump_tables.end() && absolute_table->resolved &&
                absolute_table->aot_candidates_only &&
                absolute_table->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                absolute_table->reason == "snapshot-absolute-pointer-candidates" &&
                absolute_table->entries.size() == absolute_targets.size(),
            "RWX-Disc-Snapshotpointer wurden nicht automatisch als bewachte Tabelle erkannt.");
    for (std::size_t index = 0u; index < absolute_targets.size(); ++index) {
        require(absolute_table->entries[index].target == absolute_targets[index] &&
                    has_instruction(absolute_snapshot, absolute_targets[index]) &&
                    std::binary_search(
                        absolute_snapshot.recursive.guarded_candidate_instruction_addresses.begin(),
                        absolute_snapshot.recursive.guarded_candidate_instruction_addresses.end(),
                        absolute_targets[index]) &&
                    !std::binary_search(
                        absolute_snapshot.recursive.proven_instruction_addresses.begin(),
                        absolute_snapshot.recursive.proven_instruction_addresses.end(),
                        absolute_targets[index]),
                "Bewachter Snapshotkandidat wurde nicht kanonisch in den CFG-Fixpunkt gespeist.");
    }
    const auto absolute_resolution = std::find_if(
        absolute_snapshot.indirect_control_flow.begin(),
        absolute_snapshot.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == absolute_dispatch;
        });
    require(absolute_resolution != absolute_snapshot.indirect_control_flow.end() &&
                absolute_resolution->status == katana::analysis::ResolutionStatus::Unresolved &&
                absolute_resolution->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                absolute_resolution->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Table &&
                absolute_resolution->targets.empty() &&
                absolute_resolution->analysis_candidates == absolute_targets,
            "Snapshotpointer verloren Runtimevertrag, Tabellenherkunft oder AOT-Kandidaten.");
    const auto absolute_edge_count = std::count_if(
        absolute_snapshot.resolved_edges.begin(),
        absolute_snapshot.resolved_edges.end(),
        [](const auto& edge) {
            return edge.instruction_address == absolute_dispatch && edge.guarded &&
                   edge.evidence == katana::analysis::ControlFlowEvidence::GuardedPartial &&
                   edge.evidence_origins ==
                       std::vector<katana::analysis::AnalysisEvidenceOrigin>{
                           katana::analysis::AnalysisEvidenceOrigin::JumpTable};
        });
    require(absolute_edge_count == 0,
            "Partielle Snapshotkandidaten wurden faelschlich zu statischen CFG-Kanten.");
    const auto absolute_ir = katana::ir::lower_program(absolute_snapshot);
    bool guarded_dispatch_found = false;
    std::size_t native_candidate_entries = 0u;
    std::size_t native_candidate_blocks = 0u;
    for (const auto& function : absolute_ir) {
        if (std::binary_search(absolute_targets.begin(),
                               absolute_targets.end(),
                               function.entry_address))
            ++native_candidate_entries;
        for (const auto& block : function.blocks) {
            if (std::binary_search(absolute_targets.begin(),
                                   absolute_targets.end(),
                                   block.start_address))
                ++native_candidate_blocks;
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address != absolute_dispatch) continue;
                guarded_dispatch_found = block.has_indirect_successor &&
                                         instruction.resolved_targets.empty() &&
                                         instruction.dynamic_target_class ==
                                             katana::ir::DynamicTargetClass::RuntimeOnly;
            }
        }
    }
    require(guarded_dispatch_found,
            "IR fror den RWX-Snapshot ein oder verlor den validierenden Runtime-Default.");
    require(native_candidate_entries == 1u && native_candidate_blocks == absolute_targets.size(),
            "Snapshotziele wurden nicht als dispatchbare AOT-Bloecke mit erhaltenem "
            "Funktionsfallthrough materialisiert.");

    [] {
    constexpr std::uint32_t relative_table_base = 0x00600000u;
    constexpr std::uint32_t relative_table_dispatch = relative_table_base + 0x0Eu;
    const auto relative_table_image = [](const katana::io::ImageSourceKind source_kind,
                                         const katana::io::ImageLoadPhase load_phase,
                                         const katana::io::InitialSnapshotPolicy policy) {
        std::vector<std::uint8_t> bytes(0x70u, 0u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };

        put_u16(0x00u, 0xE102u); // mov #2,r1
        put_u16(0x02u, 0x3212u); // cmp/hs r1,r2
        put_u16(0x04u, 0x8924u); // bt 0x50
        put_u16(0x06u, 0x4200u); // shll r2
        put_u16(0x08u, 0x6323u); // mov r2,r3
        put_u16(0x0Au, 0xC705u); // mova @(0x20,pc),r0
        put_u16(0x0Cu, 0x043Du); // mov.w @(r0,r3),r4
        put_u16(0x0Eu, 0x0423u); // braf r4
        put_u16(0x10u, 0x0009u); // delay slot

        // Beide Eintraege sind signed offsets relativ zu BRAF+4 (base+0x12).
        put_u16(0x20u, 0x004Eu); // base+0x60
        put_u16(0x22u, 0x0052u); // base+0x64
        put_u16(0x50u, 0x000Bu);
        put_u16(0x52u, 0x0009u);
        put_u16(0x60u, 0x000Bu);
        put_u16(0x62u, 0x0009u);
        put_u16(0x64u, 0x000Bu);
        put_u16(0x66u, 0x0009u);

        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(policy);
        image.add_segment({".synthetic-relative16-table",
                           relative_table_base,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           source_kind,
                           load_phase,
                           "synthetic-relative16-table"});
        image.add_entry_point(relative_table_base);
        return image;
    };
    const std::vector<std::uint32_t> relative_table_targets{
        relative_table_base + 0x60u, relative_table_base + 0x64u};
    const auto relative_table = katana::analysis::analyze_control_flow(relative_table_image(
        katana::io::ImageSourceKind::DiscBootFile,
        katana::io::ImageLoadPhase::Initial,
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent));
    const auto relative_table_analysis = std::find_if(
        relative_table.jump_tables.begin(),
        relative_table.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == relative_table_dispatch; });
    require(relative_table_analysis != relative_table.jump_tables.end() &&
                relative_table_analysis->resolved &&
                relative_table_analysis->aot_candidates_only &&
                relative_table_analysis->encoding ==
                    katana::analysis::JumpTableEncoding::SignedRelative16 &&
                relative_table_analysis->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                relative_table_analysis->reason ==
                    "snapshot-signed-relative16-candidates",
            "RWX-Relative16-Tabelle wurde nicht als bewachte Snapshot-AOT-Menge erkannt.");
    const auto relative_table_resolution = std::find_if(
        relative_table.indirect_control_flow.begin(),
        relative_table.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == relative_table_dispatch;
        });
    require(relative_table_resolution != relative_table.indirect_control_flow.end() &&
                relative_table_resolution->kind ==
                    katana::analysis::IndirectControlFlowKind::Jump &&
                relative_table_resolution->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                relative_table_resolution->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                relative_table_resolution->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Table &&
                !relative_table_resolution->target.has_value() &&
                relative_table_resolution->targets.empty() &&
                relative_table_resolution->analysis_candidates == relative_table_targets &&
                relative_table_resolution->reason ==
                    "runtime-contract-snapshot-signed-relative16-candidates",
            "Relative16-Snapshotkandidaten haben den lebenden MOV.W/BRAF-Vertrag ersetzt.");
    for (const auto target : relative_table_targets) {
        require(has_instruction(relative_table, target) &&
                    std::binary_search(
                        relative_table.recursive.guarded_candidate_instruction_addresses.begin(),
                        relative_table.recursive.guarded_candidate_instruction_addresses.end(),
                        target) &&
                    !std::binary_search(
                        relative_table.recursive.proven_instruction_addresses.begin(),
                        relative_table.recursive.proven_instruction_addresses.end(),
                        target) &&
                    find_function(relative_table, target) == nullptr,
                "Relative16-Snapshotziel wurde als CFG- oder Funktionsbeweis behandelt.");
    }
    require(std::none_of(relative_table.resolved_edges.begin(),
                         relative_table.resolved_edges.end(),
                         [](const auto& edge) {
                             return edge.instruction_address == relative_table_dispatch;
                         }),
            "Relative16-Snapshotkandidaten erzeugten feste CFG-Kanten.");

    const auto relative_table_ir = katana::ir::lower_program(relative_table);
    bool runtime_relative_jump_found = false;
    std::size_t relative_table_native_blocks = 0u;
    for (const auto& function : relative_table_ir) {
        for (const auto& block : function.blocks) {
            if (std::binary_search(relative_table_targets.begin(),
                                   relative_table_targets.end(),
                                   block.start_address))
                ++relative_table_native_blocks;
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address != relative_table_dispatch) continue;
                runtime_relative_jump_found =
                    block.has_indirect_successor &&
                    instruction.operation == katana::ir::Operation::JumpRegister &&
                    instruction.branch_register_relative && instruction.branch_register == 4u &&
                    !instruction.target_address.has_value() &&
                    instruction.resolved_targets.empty() &&
                    instruction.dynamic_target_class ==
                        katana::ir::DynamicTargetClass::RuntimeOnly;
            }
        }
    }
    require(runtime_relative_jump_found,
            "Die IR ersetzte das lebende BRAF-Ziel durch Snapshotwerte.");
    require(relative_table_native_blocks == relative_table_targets.size(),
            "Nicht jedes Relative16-Snapshotziel erhielt einen nativen Blockleader.");

    for (const auto& [source_kind, load_phase, policy] :
         std::array{std::tuple{katana::io::ImageSourceKind::DiscBootFile,
                              katana::io::ImageLoadPhase::Initial,
                              katana::io::InitialSnapshotPolicy::ImmutableOnly},
                    std::tuple{katana::io::ImageSourceKind::RuntimeMemory,
                              katana::io::ImageLoadPhase::Initial,
                              katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent},
                    std::tuple{katana::io::ImageSourceKind::DiscBootFile,
                              katana::io::ImageLoadPhase::RuntimeModule,
                              katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent}}) {
        const auto rejected = katana::analysis::analyze_control_flow(
            relative_table_image(source_kind, load_phase, policy));
        const auto resolution = std::find_if(
            rejected.indirect_control_flow.begin(),
            rejected.indirect_control_flow.end(),
            [](const auto& candidate) {
                return candidate.instruction_address == relative_table_dispatch;
            });
        require(resolution != rejected.indirect_control_flow.end() &&
                    resolution->status == katana::analysis::ResolutionStatus::Unresolved &&
                    resolution->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                    resolution->targets.empty() && resolution->analysis_candidates.empty() &&
                    resolution->reason == "dynamic-writable-table" &&
                    std::none_of(rejected.resolved_edges.begin(),
                                 rejected.resolved_edges.end(),
                                 [](const auto& edge) {
                                     return edge.instruction_address == relative_table_dispatch;
                                 }),
                "Relative16-Runtimebytes wurden ohne Initial-Snapshotvertrag eingefroren.");
    }

    }();

    constexpr std::uint32_t relative_call_base = 0x00500000u;
    constexpr std::uint32_t relative_call_dispatch = relative_call_base + 4u;
    const auto relative_call_island_image = [] {
        constexpr std::uint32_t base = relative_call_base;
        constexpr std::size_t first_handler = 0x20u;
        constexpr std::size_t stride = 6u;
        constexpr std::size_t return_handler_count = 4u;
        std::vector<std::uint8_t> bytes(0x60u, 0u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };

        // Der BSRF-Offset bleibt zur Laufzeit autoritativ: Die Analyse beweist nur eine
        // begrenzte, gleichfoermige Menge nativ vorzubereitender Handler.
        put_u16(0x00u, 0x6305u); // mov.w @r0+,r3
        put_u16(0x02u, 0x730Cu); // add #12,r3
        put_u16(0x04u, 0x0303u); // bsrf r3
        put_u16(0x06u, 0x0009u); // delay slot
        put_u16(0x08u, 0x000Bu); // caller continuation
        put_u16(0x0Au, 0x0009u);
        for (std::size_t index = 0u; index < return_handler_count; ++index) {
            const auto offset = first_handler + index * stride;
            put_u16(offset, 0xE100u);      // mov #0,r1
            put_u16(offset + 2u, 0x000Bu); // rts
            put_u16(offset + 4u, 0x2212u); // mov.l r1,@r2 (delay slot)
        }
        constexpr std::size_t terminal = first_handler + return_handler_count * stride;
        constexpr std::size_t terminal_target = 0x50u;
        constexpr auto terminal_displacement =
            static_cast<std::uint16_t>((terminal_target - (terminal + 4u)) / 2u);
        put_u16(terminal, static_cast<std::uint16_t>(0xA000u | terminal_displacement));
        put_u16(terminal + 2u, 0x0009u);
        put_u16(terminal + 4u, 0x0009u);
        put_u16(terminal_target, 0x000Bu);
        put_u16(terminal_target + 2u, 0x0009u);

        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".synthetic-relative-call-island",
                           base,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-relative-call-island"});
        image.add_entry_point(base);
        return image;
    };
    const std::vector<std::uint32_t> relative_call_targets{
        relative_call_base + 0x20u,
        relative_call_base + 0x26u,
        relative_call_base + 0x2Cu,
        relative_call_base + 0x32u,
        relative_call_base + 0x38u};
    const auto relative_call =
        katana::analysis::analyze_control_flow(relative_call_island_image());
    const auto relative_call_resolution = std::find_if(
        relative_call.indirect_control_flow.begin(),
        relative_call.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == relative_call_dispatch;
        });
    require(relative_call_resolution != relative_call.indirect_control_flow.end() &&
                relative_call_resolution->kind ==
                    katana::analysis::IndirectControlFlowKind::Call &&
                relative_call_resolution->register_index == 3u &&
                relative_call_resolution->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                relative_call_resolution->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                relative_call_resolution->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Table &&
                !relative_call_resolution->target.has_value() &&
                relative_call_resolution->targets.empty() &&
                relative_call_resolution->analysis_candidates == relative_call_targets,
            "BSRF-Handlerkandidaten verloren den RuntimeOnly-Tabellenvertrag.");
    for (const auto target : relative_call_targets) {
        require(has_instruction(relative_call, target) &&
                    std::binary_search(
                        relative_call.recursive.guarded_candidate_instruction_addresses.begin(),
                        relative_call.recursive.guarded_candidate_instruction_addresses.end(),
                        target) &&
                    !std::binary_search(
                        relative_call.recursive.proven_instruction_addresses.begin(),
                        relative_call.recursive.proven_instruction_addresses.end(),
                        target) &&
                    find_function(relative_call, target) == nullptr,
                "Ein BSRF-Inselziel wurde nicht ausschliesslich bewacht decodiert.");
    }
    require(std::none_of(relative_call.resolved_edges.begin(),
                         relative_call.resolved_edges.end(),
                         [](const auto& edge) {
                             return edge.instruction_address == relative_call_dispatch;
                         }),
            "BSRF-Handlerkandidaten wurden faelschlich zu statischen CFG-Kanten.");

    const auto relative_call_ir = katana::ir::lower_program(relative_call);
    bool runtime_relative_call_found = false;
    std::size_t relative_call_native_blocks = 0u;
    for (const auto& function : relative_call_ir) {
        for (const auto& block : function.blocks) {
            if (std::binary_search(relative_call_targets.begin(),
                                   relative_call_targets.end(),
                                   block.start_address))
                ++relative_call_native_blocks;
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address != relative_call_dispatch) continue;
                runtime_relative_call_found =
                    block.has_indirect_successor &&
                    instruction.operation == katana::ir::Operation::CallRegister &&
                    instruction.branch_register_relative && instruction.branch_register == 3u &&
                    !instruction.target_address.has_value() &&
                    instruction.resolved_targets.empty() &&
                    instruction.dynamic_target_class ==
                        katana::ir::DynamicTargetClass::RuntimeOnly;
            }
        }
    }
    require(runtime_relative_call_found,
            "Die IR hat den lebenden BSRF-Zielwert durch Snapshotkandidaten ersetzt.");
    require(relative_call_native_blocks == relative_call_targets.size(),
            "Nicht jedes bewachte BSRF-Inselziel erhielt einen nativen Blockleader.");

    const auto static_pr_image = [](const katana::io::ImageSourceKind source_kind,
                                    const katana::io::InitialSnapshotPolicy policy,
                                    const bool writable) {
        constexpr std::uint32_t base = 0xAC200000u;
        constexpr std::uint32_t lower_symbol = 0x8C010000u;
        constexpr std::uint32_t init_target = base + 0x20u;
        constexpr std::uint32_t continuation_target = base + 0x30u;
        std::vector<std::uint8_t> bytes(0x40u, 0u);
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        // mov.l continuation,r1; lds r1,pr; mov.l init,r0; jmp @r0; nop
        bytes[0x00u] = 0x03u;
        bytes[0x01u] = 0xD1u;
        bytes[0x02u] = 0x2Au;
        bytes[0x03u] = 0x41u;
        bytes[0x04u] = 0x03u;
        bytes[0x05u] = 0xD0u;
        bytes[0x06u] = 0x2Bu;
        bytes[0x07u] = 0x40u;
        bytes[0x08u] = 0x09u;
        bytes[0x09u] = 0x00u;
        put_u32(0x10u, continuation_target);
        put_u32(0x14u, init_target);
        bytes[0x20u] = 0x0Bu;
        bytes[0x21u] = 0x00u;
        bytes[0x22u] = 0x09u;
        bytes[0x23u] = 0x00u;
        bytes[0x30u] = 0x09u;
        bytes[0x31u] = 0x00u;
        bytes[0x32u] = 0x0Bu;
        bytes[0x33u] = 0x00u;
        bytes[0x34u] = 0x09u;
        bytes[0x35u] = 0x00u;
        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(policy);
        image.set_address_model(katana::io::ImageAddressModel::Sh4DirectMapped);
        image.add_segment({".lower-symbol",
                           lower_symbol,
                           0u,
                           4u,
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {0x0Bu, 0x00u, 0x09u, 0x00u},
                           katana::io::ImageSourceKind::RawBinary,
                           katana::io::ImageLoadPhase::Initial,
                           "lower-symbol"});
        image.add_segment({".synthetic-pr-bootstrap",
                           base,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, writable, true},
                           std::move(bytes),
                           source_kind,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-pr-bootstrap"});
        image.add_entry_point(lower_symbol);
        image.add_entry_point(base);
        image.set_initial_snapshot_entry(base);
        return image;
    };
    constexpr std::uint32_t static_pr_instruction = 0xAC200002u;
    constexpr std::uint32_t static_pr_target = 0xAC200030u;
    const auto static_pr = katana::analysis::analyze_control_flow(static_pr_image(
        katana::io::ImageSourceKind::DiscBootFile,
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent,
        true));
    require(static_pr.static_return_continuations.size() == 1u &&
                static_pr.static_return_continuations[0].instruction_address ==
                    static_pr_instruction &&
                static_pr.static_return_continuations[0].register_index == 1u &&
                static_pr.static_return_continuations[0].target_address == static_pr_target &&
                static_pr.static_return_continuations[0].evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                static_pr.static_return_continuations[0].evidence_origins ==
                    std::vector<katana::analysis::AnalysisEvidenceOrigin>{
                        katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot} &&
                static_pr.static_return_continuations[0].reason ==
                    "runtime-contract-static-pr-continuation",
            "Statische PR-Fortsetzung wurde nicht als begrenzter Runtime-AOT-Kandidat erkannt.");
    require(has_instruction(static_pr, static_pr_target) &&
                std::binary_search(
                    static_pr.recursive.guarded_candidate_instruction_addresses.begin(),
                    static_pr.recursive.guarded_candidate_instruction_addresses.end(),
                    static_pr_target) &&
                !std::binary_search(static_pr.recursive.proven_instruction_addresses.begin(),
                                    static_pr.recursive.proven_instruction_addresses.end(),
                                    static_pr_target) &&
                find_function(static_pr, static_pr_target) == nullptr,
            "PR-Fortsetzung wurde nicht bewacht decodiert oder zur Gastfunktion erfunden.");
    require(std::none_of(static_pr.resolved_edges.begin(),
                         static_pr.resolved_edges.end(),
                         [](const auto& edge) {
                             return edge.instruction_address == static_pr_instruction ||
                                    edge.target_address == static_pr_target;
                         }),
            "PR-AOT-Kandidat wurde faelschlich zu einer statischen CFG-Kante.");
    const auto static_pr_ir = katana::ir::lower_program(static_pr);
    std::size_t static_pr_native_blocks = 0u;
    for (const auto& function : static_pr_ir) {
        for (const auto& block : function.blocks) {
            if (block.start_address == static_pr_target) ++static_pr_native_blocks;
        }
    }
    require(static_pr_native_blocks == 1u,
            "Bewachte PR-Fortsetzung erhielt keinen eindeutigen nativen Blockeinstieg.");
    const auto static_pr_json = katana::analysis::format_control_flow_analysis_json(static_pr);
    require(static_pr_json.find("\"static_return_continuations\":1") !=
                std::string::npos &&
                static_pr_json.find("\"target_address\":\"0xAC200030\"") !=
                    std::string::npos,
            "Kontrollflussbericht verschweigt statische PR-AOT-Kandidaten.");
    const auto runtime_pr = katana::analysis::analyze_control_flow(static_pr_image(
        katana::io::ImageSourceKind::RuntimeMemory,
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent,
        false));
    require(runtime_pr.static_return_continuations.empty() &&
                !has_instruction(runtime_pr, static_pr_target),
            "Nicht beschreibbarer Laufzeitmodulspeicher wurde als statische PR-Fortsetzung "
            "vorkompiliert.");
    const auto no_snapshot_pr = katana::analysis::analyze_control_flow(static_pr_image(
        katana::io::ImageSourceKind::DiscBootFile,
        katana::io::InitialSnapshotPolicy::ImmutableOnly,
        true));
    require(no_snapshot_pr.static_return_continuations.empty() &&
                !has_instruction(no_snapshot_pr, static_pr_target),
            "Beschreibbare PR-Fortsetzung wurde ohne Initial-Snapshotvertrag vorkompiliert.");

    const auto runtime_snapshot = katana::analysis::analyze_control_flow(
        absolute_snapshot_image(katana::io::ImageSourceKind::RuntimeMemory,
                                katana::io::ImageLoadPhase::RuntimeModule,
                                katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent));
    require(runtime_snapshot.jump_tables.empty() &&
                !has_instruction(runtime_snapshot, absolute_targets[0]) &&
                !has_instruction(runtime_snapshot, absolute_targets[1]),
            "Laufzeitmodulspeicher wurde faelschlich als Initial-Snapshotquelle verwendet.");

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
