#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/analysis/owner_semantic_summary.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
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

const katana::analysis::GuardedAotEntry*
find_guarded_aot_entry(
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const std::uint32_t address) {
    const auto iterator =
        std::find_if(analysis.guarded_aot_entries.begin(),
                     analysis.guarded_aot_entries.end(),
                     [address](const auto& entry) {
                         return entry.guest_address == address;
                     });
    return iterator == analysis.guarded_aot_entries.end() ? nullptr
                                                           : &*iterator;
}

const katana::analysis::GuardedAotEntryRejection*
find_guarded_aot_entry_rejection(
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const std::uint32_t address) {
    const auto iterator =
        std::find_if(analysis.guarded_aot_entry_rejections.begin(),
                     analysis.guarded_aot_entry_rejections.end(),
                     [address](const auto& rejection) {
                         return rejection.guest_address == address;
                     });
    return iterator == analysis.guarded_aot_entry_rejections.end()
               ? nullptr
               : &*iterator;
}

bool has_ir_block(const std::span<const katana::ir::Function> program,
                  const std::uint32_t address) {
    return std::any_of(
        program.begin(), program.end(), [address](const auto& function) {
            return std::any_of(
                function.blocks.begin(),
                function.blocks.end(),
                [address](const auto& block) {
                    return block.start_address == address;
                });
        });
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
    {
        const auto bounded_image = code_image(
            {0x09u, 0x00u, // nop
             0x0Bu, 0x00u, // rts
             0x09u, 0x00u}); // delay-slot nop

        katana::analysis::ControlFlowAnalysisOptions iteration_options;
        iteration_options.maximum_fixpoint_iterations = 0u;
        const auto iteration_bounded =
            katana::analysis::analyze_control_flow(
                bounded_image,
                nullptr,
                katana::analysis::
                    ControlFlowAnalysisProgressCallback{},
                iteration_options);
        require(
            iteration_bounded.termination_reason ==
                    katana::analysis::
                        ControlFlowAnalysisTerminationReason::
                            AnalysisIterationBudgetExceeded &&
                iteration_bounded.fixpoint_iterations == 0u &&
                iteration_bounded.recursive.instructions.empty() &&
                !katana::analysis::guarded_aot_inventory_complete(
                    iteration_bounded),
            "Explizites CFA-Iterationsbudget wurde nicht vor der "
            "Produktarbeit typisiert beendet.");

        katana::analysis::ControlFlowAnalysisOptions instruction_options;
        instruction_options.maximum_instructions = 1u;
        const auto instruction_bounded =
            katana::analysis::analyze_control_flow(
                bounded_image,
                nullptr,
                [](const katana::analysis::
                       ControlFlowAnalysisProgress&) {
                    throw std::runtime_error(
                        "synthetic-bounded-observer-failure");
                },
                instruction_options);
        require(
            instruction_bounded.termination_reason ==
                    katana::analysis::
                        ControlFlowAnalysisTerminationReason::
                            InstructionBudgetExceeded &&
                instruction_bounded.recursive.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        InstructionBudgetExceeded &&
                instruction_bounded.recursive.instructions.size() == 1u &&
                instruction_bounded.progress_callback_failed,
            "CFA-Instruktionsbudget und werfender Beobachter wurden "
            "vermischt oder liessen Arbeit ueber das Limit zu.");

        katana::analysis::ControlFlowAnalysisOptions context_options;
        context_options.maximum_contexts = 1u;
        const auto context_bounded =
            katana::analysis::analyze_control_flow(
                bounded_image,
                nullptr,
                katana::analysis::
                    ControlFlowAnalysisProgressCallback{},
                context_options);
        const auto retains_initial_seed_ledger = [](const auto& result) {
            if (result.seed_facts.size() != 1u ||
                result.seed_targets_added != 1u ||
                result.seed_causes_added != 1u ||
                result.seed_decode_targets != 1u ||
                result.seed_metadata_targets != 1u)
                return false;
            const auto& fact = result.seed_facts.front();
            if (fact.target_address != 0u || fact.causes.size() != 1u)
                return false;
            const auto& cause = fact.causes.front();
            return cause.kind ==
                       katana::analysis::ControlFlowAnalysisResult::
                           SeedCauseKind::EntryPoint &&
                   cause.source_address == 0u &&
                   !cause.source_object.has_value() &&
                   cause.owner_address == 0u;
        };
        require(
            context_bounded.termination_reason ==
                    katana::analysis::
                        ControlFlowAnalysisTerminationReason::
                            AnalysisContextBudgetExceeded &&
                context_bounded.recursive.limit ==
                    katana::analysis::RecursiveAnalysisLimit::
                        ContextBudgetExceeded &&
                context_bounded.recursive.contextual_instructions.size() ==
                    1u &&
                retains_initial_seed_ledger(iteration_bounded) &&
                retains_initial_seed_ledger(instruction_bounded) &&
                retains_initial_seed_ledger(context_bounded),
            "CFA-Kontextbudget stoppte den rekursiven Decodepfad nicht "
            "am exakten Limit oder verlor dabei seine Seed-Provenienz.");
        const auto iteration_json =
            katana::analysis::format_control_flow_analysis_json(
                iteration_bounded);
        const auto context_frontier_json =
            katana::analysis::format_control_flow_frontier_json(
                context_bounded);
        auto observability = iteration_bounded;
        observability.recursive_incremental_passes = 7u;
        observability.recursive_full_recompute_fallbacks = 3u;
        const auto observability_json =
            katana::analysis::format_control_flow_analysis_json(
                observability);
        const auto observability_frontier_json =
            katana::analysis::format_control_flow_frontier_json(
                observability);
        require(
            iteration_json.find(
                "\"termination_reason\":"
                "\"analysis-iteration-budget-exceeded\"") !=
                    std::string::npos &&
                iteration_json.find(
                    "\"recursive_baseline_status\":"
                    "\"not-requested\"") != std::string::npos &&
                context_frontier_json.find(
                    "\"termination_reason\":"
                    "\"analysis-context-budget-exceeded\"") !=
                    std::string::npos &&
                context_frontier_json.find(
                    "\"recursive_baseline_status\":"
                    "\"not-requested\"") != std::string::npos &&
                observability_json.find(
                    "\"recursive_incremental_passes\":7") !=
                    std::string::npos &&
                observability_json.find(
                    "\"recursive_full_recompute_fallbacks\":3") !=
                    std::string::npos &&
                observability_frontier_json.find(
                    "\"recursive_incremental_passes\":7") !=
                    std::string::npos &&
                observability_frontier_json.find(
                    "\"recursive_full_recompute_fallbacks\":3") !=
                    std::string::npos &&
                katana::analysis::
                    control_flow_analysis_termination_reason_name(
                        instruction_bounded.termination_reason) ==
                    "instruction-budget-exceeded",
            "Der typisierte CFA-Abbruch ging im oeffentlichen Voll-/"
            "Frontier-JSON oder seiner Namensabbildung verloren.");
    }
    {
        std::vector<std::uint8_t> bytes(0x80u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        put_u16(0x00u, 0xE470u); // mov #0x70,r4 (ordinary data pointer)
        put_u16(0x02u, 0xE560u); // mov #0x60,r5 (non-stack object field)
        put_u16(0x04u, 0xB00Cu); // bsr 0x20
        put_u16(0x06u, 0x0009u);
        put_u16(0x08u, 0x000Bu);
        put_u16(0x0Au, 0x0009u);
        put_u16(0x20u, 0x2542u); // mov.l r4,@r5
        put_u16(0x22u, 0x000Bu);
        put_u16(0x24u, 0x0009u);
        // Executable-range data can begin with several valid opcodes. Its
        // local shape is nevertheless not native code because the BRA delay
        // slot contains another control-flow instruction.
        put_u16(0x70u, 0xE2DAu);
        put_u16(0x72u, 0xBF0Eu);
        put_u16(0x74u, 0x5FDCu);
        put_u16(0x76u, 0x3E0Du);
        put_u16(0x78u, 0xA1BEu);
        put_u16(0x7Au, 0xBF4Cu);

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.add_segment({".mixed-code-and-data",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes)});
        image.add_entry_point(0u);
        const auto rejected_data_entry =
            katana::analysis::analyze_control_flow(image);
        require(!has_instruction(rejected_data_entry, 0x70u) &&
                    find_guarded_aot_entry(rejected_data_entry, 0x70u) == nullptr &&
                    std::none_of(
                        rejected_data_entry.seed_facts.begin(),
                        rejected_data_entry.seed_facts.end(),
                        [](const auto& seed) {
                            return seed.target_address == 0x70u;
                        }) &&
                    rejected_data_entry.guarded_code_inventory_candidates == 0u &&
                    !rejected_data_entry.candidate_inventory_truncated &&
                    rejected_data_entry
                            .guarded_code_shape_budget_exceeded_candidates ==
                        0u,
                "Strukturell ungueltige Mixed-Segment-Daten wurden als "
                "Guarded-AOT-Einstieg akzeptiert oder als Budgetabbruch "
                "fehlklassifiziert.");
        const auto rejected_data_ir =
            katana::ir::lower_program(rejected_data_entry);
        require(katana::ir::verify_program(rejected_data_ir).empty(),
                "Abgelehnter Guarded-Dateneinstieg hinterliess ungueltige "
                "Callee-Metadaten im IR.");
    }
    {
        constexpr std::uint32_t candidate_address = 0x100u;
        constexpr std::size_t shape_instruction_budget = 4'096u;
        std::vector<std::uint8_t> bytes(
            candidate_address + (shape_instruction_budget + 1u) * 2u,
            0x00u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        put_u16(0x00u, 0xD403u); // mov.l @(0x10,pc),r4
        put_u16(0x02u, 0xE560u); // mov #0x60,r5 (non-stack object)
        put_u16(0x04u, 0xB00Cu); // bsr 0x20
        put_u16(0x06u, 0x0009u); // nop (delay)
        put_u16(0x08u, 0x000Bu); // rts
        put_u16(0x0Au, 0x0009u); // nop (delay)
        put_u32(0x10u, candidate_address);
        put_u16(0x20u, 0x2542u); // mov.l r4,@r5
        put_u16(0x22u, 0x000Bu); // rts
        put_u16(0x24u, 0x0009u); // nop (delay)
        for (std::size_t index = 0u;
             index <= shape_instruction_budget;
             ++index)
            put_u16(candidate_address + index * 2u, 0x0009u);

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.add_segment({".shape-budget",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes)});
        image.add_entry_point(0u);
        const auto truncated = katana::analysis::analyze_control_flow(image);
        const auto report =
            katana::analysis::format_control_flow_analysis_json(truncated);
        require(!has_instruction(truncated, candidate_address) &&
                    find_guarded_aot_entry(truncated, candidate_address) ==
                        nullptr &&
                    truncated.guarded_code_inventory_candidates == 0u &&
                    truncated.candidate_inventory_truncated &&
                    truncated.guarded_code_shape_validation_work ==
                        shape_instruction_budget &&
                    truncated.guarded_code_shape_validation_work_budget >=
                        truncated.guarded_code_shape_validation_work &&
                    truncated
                            .guarded_code_shape_budget_exceeded_candidates ==
                        1u &&
                    report.find(
                        "\"guarded_code_shape_budget_exceeded_candidates\":1,"
                        "\"candidate_inventory_truncated\":true") !=
                        std::string::npos,
                "Shape-Budgetabbruch wurde als strukturell ungueltiger "
                "vollstaendiger Inventarlauf verschluckt oder belegte das "
                "Ergebnisbudget.");

        katana::analysis::ControlFlowAnalysisResult completeness_contract;
        require(katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Leerer Guarded-AOT-Inventarvertrag ist unvollstaendig.");
        completeness_contract.function_budget_exhausted = true;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Function-Budgetverlust blieb exportfaehig.");
        completeness_contract.function_budget_exhausted = false;
        completeness_contract.raw_stored_code_inventory_truncated = true;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Rohes Store-Inventarbudget blieb exportfaehig.");
        completeness_contract.raw_stored_code_inventory_truncated = false;
        completeness_contract.guarded_code_inventory_walk
            .forwarded_store_context_limited_functions = 1u;
        require(!katana::analysis::guarded_aot_inventory_complete(completeness_contract),
                "Forwarding-Walkverlust blieb exportfaehig.");
        completeness_contract.guarded_code_inventory_walk
            .forwarded_store_context_limited_functions = 0u;
        completeness_contract.guarded_code_inventory_walk
            .abi_stack_argument_projection_truncated_functions = 1u;
        require(!katana::analysis::guarded_aot_inventory_complete(completeness_contract),
                "ABI-Stackargumentverlust blieb exportfaehig.");
        completeness_contract.guarded_code_inventory_walk
            .abi_stack_argument_projection_truncated_functions = 0u;
        completeness_contract.guarded_code_inventory_walk
            .local_fixpoint_limited_evaluations = 1u;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Lokaler Function-Fixpunktabbruch blieb exportfaehig.");
        completeness_contract.guarded_code_inventory_walk
            .local_fixpoint_limited_evaluations = 0u;
        completeness_contract.guarded_code_inventory_walk
            .inventory_candidate_values_truncated = true;
        require(!katana::analysis::guarded_aot_inventory_complete(completeness_contract),
                "Inventory-Kandidatendomänenverlust blieb exportfaehig.");
        completeness_contract.guarded_code_inventory_walk
            .inventory_candidate_values_truncated = false;
        completeness_contract.guarded_code_inventory_walk
            .abi_stack_base_unresolved = true;
        require(!katana::analysis::guarded_aot_inventory_complete(completeness_contract),
                "Unaufgeloeste ABI-Stackbasis blieb exportfaehig.");
        completeness_contract.guarded_code_inventory_walk
            .abi_stack_base_unresolved = false;
        completeness_contract.guarded_code_inventory_walk
            .inventory_tail_target_unresolved = true;
        const auto unresolved_tail_json =
            katana::analysis::format_control_flow_analysis_json(
                completeness_contract);
        require(
            !katana::analysis::guarded_aot_inventory_complete(
                completeness_contract) &&
                unresolved_tail_json.find(
                    "\"guarded_inventory_tail_target_unresolved\":true") !=
                    std::string::npos,
            "Ungebundenes typisiertes Tail-Ziel blieb exportfaehig oder "
            "verschwand aus der oeffentlichen Diagnose.");
        completeness_contract.guarded_code_inventory_walk
            .inventory_tail_target_unresolved = false;
        completeness_contract.candidate_inventory_truncated = true;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Candidate-Inventarverlust blieb exportfaehig.");
        completeness_contract.candidate_inventory_truncated = false;
        completeness_contract.returned_table_scan_truncated = true;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Returned-Table-Scanverlust blieb exportfaehig.");
        completeness_contract.returned_table_scan_truncated = false;
        completeness_contract
            .guarded_code_shape_budget_exceeded_candidates = 1u;
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Direkter Shape-Budgetverlust blieb exportfaehig.");
        completeness_contract
            .guarded_code_shape_budget_exceeded_candidates = 0u;
        completeness_contract.guarded_aot_entry_rejections.push_back(
            {0x100u,
             0x100u,
             katana::analysis::GuardedAotEntryRejectionReason::
                 InstructionNotAnalyzed,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::GuardedAotEntryOrigin::TailIngress},
             {0x20u},
             {}});
        require(!katana::analysis::guarded_aot_inventory_complete(
                    completeness_contract),
                "Typisiert abgelehnter Guarded-AOT-Einstieg blieb "
                "exportfaehig.");
    }

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

    {
        std::vector<std::uint8_t> bytes(0x12u, 0x09u);
        bytes[0x00u] = 0x2Bu;
        bytes[0x01u] = 0x41u; // jmp @r1
        bytes[0x10u] = 0x0Bu;
        bytes[0x11u] = 0x00u; // rts without committed delay-slot bytes
        auto rejected_entry_image = code_image(std::move(bytes));
        katana::analysis::AnalysisOverrides rejected_entry_hint;
        rejected_entry_hint.mode =
            katana::analysis::AnalysisDirectiveMode::Hint;
        rejected_entry_hint.source_path =
            "guarded-entry-rejection-hint.txt";
        rejected_entry_hint.jumps.push_back({0u, 0x10u, 1u});
        const auto rejected_entry =
            katana::analysis::analyze_control_flow(
                rejected_entry_image, &rejected_entry_hint);
        const auto* rejection =
            find_guarded_aot_entry_rejection(rejected_entry, 0x10u);
        const auto rejection_json =
            katana::analysis::format_control_flow_analysis_json(
                rejected_entry);
        require(
            find_guarded_aot_entry(rejected_entry, 0x10u) == nullptr &&
                rejection != nullptr &&
                rejection->resolved_address == 0x10u &&
                rejection->reason ==
                    katana::analysis::
                        GuardedAotEntryRejectionReason::
                            EntryExtentUnavailable &&
                rejection->origins ==
                    std::vector{
                        katana::analysis::GuardedAotEntryOrigin::
                            TailIngress} &&
                rejection->source_sites ==
                    std::vector<std::uint32_t>{0u} &&
                !katana::analysis::guarded_aot_inventory_complete(
                    rejected_entry) &&
                rejection_json.find(
                    "\"guarded_aot_entry_rejections\":1") !=
                    std::string::npos &&
                rejection_json.find(
                    "\"guest_address\":\"0x00000010\","
                    "\"resolved_address\":\"0x00000010\","
                    "\"reason\":\"entry-extent-unavailable\"") !=
                    std::string::npos,
            "Akzeptierter, aber nicht materialisierbarer Guarded-AOT-"
            "Kandidat verlor typisierten Grund, Herkunft oder "
            "Fail-closed-Vertrag.");
    }

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

    const auto stored_callback_image = [] {
        std::vector<std::uint8_t> bytes(0x50u, 0x09u);
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        const std::array<std::uint8_t, 12u> caller{
            0x03u, 0xD4u, // mov.l @(0x10,pc),r4 -> handler 0x30
            0x04u, 0xD3u, // mov.l @(0x14,pc),r3 -> registrar 0x20
            0x0Bu, 0x43u, // jsr @r3
            0x09u, 0x00u, // nop (delay)
            0x0Bu, 0x00u, // rts
            0x09u, 0x00u  // nop (delay)
        };
        std::copy(caller.begin(), caller.end(), bytes.begin());
        put_u32(0x10u, 0x30u);
        put_u32(0x14u, 0x20u);
        bytes[0x20u] = 0x3Cu;
        bytes[0x21u] = 0xE2u; // mov #0x3c,r2 (proven non-stack destination)
        bytes[0x22u] = 0x42u;
        bytes[0x23u] = 0x22u; // mov.l r4,@r2
        bytes[0x24u] = 0x0Bu;
        bytes[0x25u] = 0x00u; // rts
        bytes[0x26u] = 0x09u;
        bytes[0x27u] = 0x00u; // nop (delay)
        bytes[0x30u] = 0x0Bu;
        bytes[0x31u] = 0x00u; // handler: rts
        bytes[0x32u] = 0x09u;
        bytes[0x33u] = 0x00u; // nop (delay)
        bytes[0x40u] = 0x2Eu;
        bytes[0x41u] = 0x0Eu; // mov.l @(r0,r2),r14
        bytes[0x42u] = 0x0Bu;
        bytes[0x43u] = 0x4Eu; // jsr @r14
        bytes[0x44u] = 0x09u;
        bytes[0x45u] = 0x00u; // nop (delay)
        bytes[0x46u] = 0x0Bu;
        bytes[0x47u] = 0x00u; // rts
        bytes[0x48u] = 0x09u;
        bytes[0x49u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".stored-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-stored-callback"});
        image.add_entry_point(0u);
        image.add_entry_point(0x40u);
        return image;
    }();
    const auto stored_callback =
        katana::analysis::analyze_control_flow(stored_callback_image);
    const auto* stored_handler = find_function(stored_callback, 0x30u);
    require(stored_handler != nullptr &&
                stored_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                has_instruction(stored_callback, 0x30u) &&
                std::binary_search(
                    stored_callback.recursive.guarded_candidate_instruction_addresses.begin(),
                    stored_callback.recursive.guarded_candidate_instruction_addresses.end(),
                    0x30u),
            "Gespeicherter endlicher Codepointer erreichte das bewachte AOT-Inventar nicht.");
    const auto callback_registrar_call =
        std::find_if(stored_callback.indirect_control_flow.begin(),
                     stored_callback.indirect_control_flow.end(),
                     [](const auto& resolution) {
                         return resolution.instruction_address == 0x04u;
                     });
    const auto callback_dispatch =
        std::find_if(stored_callback.indirect_control_flow.begin(),
                     stored_callback.indirect_control_flow.end(),
                     [](const auto& resolution) {
                         return resolution.instruction_address == 0x42u;
                     });
    require(callback_registrar_call != stored_callback.indirect_control_flow.end() &&
                callback_registrar_call->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                callback_registrar_call->targets.empty() &&
                callback_registrar_call->analysis_candidates ==
                    std::vector<std::uint32_t>{0x20u} &&
                callback_dispatch != stored_callback.indirect_control_flow.end() &&
                callback_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                callback_dispatch->targets.empty() &&
                callback_dispatch->analysis_candidates.empty() &&
                std::none_of(stored_callback.resolved_edges.begin(),
                             stored_callback.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.target_address == 0x30u ||
                                        (edge.instruction_address == 0x04u &&
                                         edge.target_address == 0x20u);
                             }),
            "Kandidat-only ABI-Fluss fror einen Runtime-Dispatcher als statische Kante ein.");

    auto exact_stored_callback_image = stored_callback_image;
    exact_stored_callback_image.add_immutable_range(
        {0u, 0x50u, "synthetic-stored-callback-v1", 0u});
    const auto exact_stored_callback =
        katana::analysis::analyze_control_flow(exact_stored_callback_image);
    const auto exact_registrar_call = std::find_if(
        exact_stored_callback.indirect_control_flow.begin(),
        exact_stored_callback.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x04u;
        });
    require(
        exact_registrar_call !=
                exact_stored_callback.indirect_control_flow.end() &&
            exact_registrar_call->status ==
                katana::analysis::ResolutionStatus::Guarded &&
            exact_registrar_call->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedComplete &&
            exact_registrar_call->target == 0x20u &&
            exact_registrar_call->exact_target_guard &&
            exact_registrar_call->exact_guard_rejection_reason ==
                katana::analysis::ExactGuardRejectionReason::None,
        "Identitaetsgebundene unveraenderliche Literal-Kette wurde nicht "
        "als exakte fail-closed AOT-Kante zugelassen.");

    auto partial_literal_image = stored_callback_image;
    partial_literal_image.add_immutable_range(
        {0u, 0x16u, "synthetic-stored-callback-v1", 0u});
    partial_literal_image.add_immutable_range(
        {0x20u, 0x30u, "synthetic-stored-callback-v1", 0u});
    const auto partial_literal_callback =
        katana::analysis::analyze_control_flow(partial_literal_image);
    const auto partial_literal_call = std::find_if(
        partial_literal_callback.indirect_control_flow.begin(),
        partial_literal_callback.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x04u;
        });
    require(
        partial_literal_call !=
                partial_literal_callback.indirect_control_flow.end() &&
            !partial_literal_call->exact_target_guard &&
            partial_literal_call->exact_guard_rejection_reason ==
                katana::analysis::ExactGuardRejectionReason::
                    LiteralImmutableProofMissing,
        "Eine nur halb gebundene 32-Bit-Literalzelle wurde als exakte "
        "AOT-Kante akzeptiert oder falsch klassifiziert: exact=" +
            std::to_string(
                partial_literal_call !=
                        partial_literal_callback.indirect_control_flow.end()
                    ? partial_literal_call->exact_target_guard
                    : false) +
            " reason=" +
            (partial_literal_call !=
                     partial_literal_callback.indirect_control_flow.end()
                 ? katana::analysis::exact_guard_rejection_reason_name(
                       partial_literal_call->exact_guard_rejection_reason)
                 : "missing"));

    const auto stored_callback_ir = katana::ir::lower_program(stored_callback);
    require(std::any_of(stored_callback_ir.begin(),
                        stored_callback_ir.end(),
                        [](const auto& function) { return function.entry_address == 0x30u; }) &&
                katana::ir::verify_program(stored_callback_ir).empty(),
            "Gespeicherter Codepointer erreichte das native IR-Inventar nicht.");

    auto stored_delay_slot_callback_image = stored_callback_image;
    stored_delay_slot_callback_image.write_u32_le(0x10u, 0x32u);
    stored_delay_slot_callback_image.write_u32_le(
        0x30u, 0xE400430Bu); // jsr @r3; mov #0,r4 (physical delay slot)
    stored_delay_slot_callback_image.write_u32_le(
        0x34u, 0x0009000Bu); // rts; nop
    const auto stored_delay_slot_callback =
        katana::analysis::analyze_control_flow(
            stored_delay_slot_callback_image);
    require(
        find_function(stored_delay_slot_callback, 0x32u) == nullptr &&
            !has_instruction(stored_delay_slot_callback, 0x32u) &&
            find_guarded_aot_entry(stored_delay_slot_callback, 0x32u) ==
                nullptr,
        "Ein gespeicherter Codepointer promovierte einen physischen "
        "Delay Slot ohne unabhaengigen Normal-Entry-Beweis.");

    auto dual_context_image = code_image(
        {0x2Bu, 0x41u, // jmp @r1
         0x09u, 0x00u, // delay-slot nop
         0x09u, 0x00u, 0x09u, 0x00u,
         0x09u, 0x00u, 0x09u, 0x00u,
         0x09u, 0x00u, 0x09u, 0x00u,
         0x0Bu, 0x00u, // physical owner: rts
         0x09u, 0x00u, // candidate and physical delay slot
         0x0Bu, 0x00u, // candidate normal continuation: rts
         0x09u, 0x00u}); // delay-slot nop
    const auto image_delay_slot =
        katana::analysis::prove_sh4_physical_delay_slot(
            dual_context_image, 0x12u);
    const auto byte_delay_slot =
        katana::analysis::prove_sh4_physical_delay_slot(
            dual_context_image.segments().front().bytes, 0u, 0x12u);
    require(
        image_delay_slot.has_value() &&
            image_delay_slot->owner_address == 0x10u &&
            byte_delay_slot.has_value() &&
            byte_delay_slot->owner_address == 0x10u &&
            !katana::analysis::prove_sh4_physical_delay_slot(
                 dual_context_image, 0x14u).has_value(),
        "Die zentrale SH-4-Delay-Slot-Proof-Primitive erkannte die "
        "physische Owner/Slot-Beziehung nicht exakt.");

    katana::io::ExecutableImage split_delay_slot_image;
    split_delay_slot_image.add_segment(
        {".owner", 0x10u, 0u, 2u,
         katana::io::SegmentKind::Code, {true, false, true},
         {0x0Bu, 0x00u}});
    split_delay_slot_image.add_segment(
        {".entry", 0x12u, 2u, 4u,
         katana::io::SegmentKind::Code, {true, false, true},
         {0x09u, 0x00u, 0x0Bu, 0x00u}});
    require(
        !katana::analysis::prove_sh4_physical_delay_slot(
             split_delay_slot_image, 0x12u).has_value(),
        "Numerisch benachbarte Woerter aus getrennten Segmenten wurden "
        "als SH-4-Owner/Delay-Slot-Paar akzeptiert.");
    katana::analysis::AnalysisOverrides dual_context_hint;
    dual_context_hint.mode =
        katana::analysis::AnalysisDirectiveMode::Hint;
    dual_context_hint.source_path = "dual-context-hint.txt";
    dual_context_hint.jumps.push_back({0u, 0x12u, 1u});
    const auto candidate_only_dual_context =
        katana::analysis::analyze_control_flow(
            dual_context_image, &dual_context_hint);
    const auto* candidate_only_rejection =
        find_guarded_aot_entry_rejection(
            candidate_only_dual_context, 0x12u);
    require(
        find_guarded_aot_entry(
            candidate_only_dual_context, 0x12u) == nullptr &&
            candidate_only_rejection != nullptr &&
            candidate_only_rejection->reason ==
                katana::analysis::GuardedAotEntryRejectionReason::
                    DelaySlotEntry,
        "Ein Candidate-only-Hint promotierte einen physischen Delay Slot "
        "ohne unabhaengigen Normal-Entry-Beweis.");

    dual_context_hint.function_entry_hints.push_back(
        {0x12u, 2u});
    const auto dual_context_callback =
        katana::analysis::analyze_control_flow(
            dual_context_image, &dual_context_hint);
    const auto* dual_context_entry =
        find_guarded_aot_entry(dual_context_callback, 0x12u);
    const auto* dual_context_rejection =
        find_guarded_aot_entry_rejection(
            dual_context_callback, 0x12u);
    require(
        find_function(dual_context_callback, 0x12u) == nullptr &&
            dual_context_entry != nullptr &&
            dual_context_rejection == nullptr,
        "Ein Non-Root-Normal-Entry wurde nicht als Guarded-AOT-Kontext "
        "zugelassen oder faelschlich zur Funktion erhoben: function=" +
            std::to_string(
                find_function(dual_context_callback, 0x12u) != nullptr) +
            " instruction=" +
            std::to_string(has_instruction(dual_context_callback, 0x12u)) +
            " entry=" + std::to_string(dual_context_entry != nullptr) +
            " rejection=" +
            (dual_context_rejection != nullptr
                 ? katana::analysis::guarded_aot_entry_rejection_reason_name(
                       dual_context_rejection->reason)
                 : "none"));

    const auto caller_bounded_indexed_literal_image = [](
        const bool complete_second_caller,
        const bool exact_literal_base,
        const std::uint16_t indexed_store_opcode,
        const bool unknown_store_source_path = false) {
        constexpr std::uint32_t image_base = 0x8C010000u;
        std::vector<std::uint8_t> bytes(0x64u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] =
                static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] =
                static_cast<std::uint8_t>(value >> 24u);
        };
        put_u16(0x00u, 0xE500u); // caller 0: mov #0,r5
        put_u16(0x02u, 0xB00Du); // bsr 0x20
        put_u16(0x04u, 0x0009u); // delay
        put_u16(0x06u, 0x000Bu); // rts
        put_u16(0x08u, 0x0009u); // delay

        put_u16(0x10u, complete_second_caller
                             ? 0xE501u   // caller 1: mov #1,r5
                             : 0x0009u); // unknown incoming r5
        put_u16(0x12u, 0xB005u); // bsr 0x20
        put_u16(0x14u, 0x0009u); // delay
        put_u16(0x16u, 0x000Bu); // rts
        put_u16(0x18u, 0x0009u); // delay

        put_u16(0x20u, 0x6653u); // callee: mov r5,r6
        put_u16(0x22u, 0x4608u); // shll2 r6
        put_u16(0x24u, unknown_store_source_path
                             ? 0x8901u  // bt 0x2a, retaining unknown r5
                             : 0x0009u);
        put_u16(0x26u, 0x0009u);
        put_u16(0x28u, 0xD505u); // target literal at +0x40 -> r5
        put_u16(0x2Au, exact_literal_base
                             ? 0xD006u   // base literal at +0x44 -> r0
                             : 0x6043u); // mov r4,r0: no literal identity
        put_u16(0x2Cu, indexed_store_opcode);
        put_u16(0x2Eu, 0x000Bu); // rts
        put_u16(0x30u, 0x0009u); // delay
        put_u32(0x40u, image_base + 0x60u);
        put_u32(0x44u, 0x8C88FDD8u);
        put_u16(0x60u, 0x000Bu); // guarded callback target: rts
        put_u16(0x62u, 0x0009u); // delay

        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::
                EntryPointStraightLineQuiescent);
        image.add_segment({".persistent-indexed-literal",
                           image_base,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-caller-bounded-indexed-literal"});
        image.add_entry_point(image_base);
        image.add_entry_point(image_base + 0x10u);
        return image;
    };
    katana::analysis::AnalysisOverrides persistent_literal_hints;
    persistent_literal_hints.mode =
        katana::analysis::AnalysisDirectiveMode::Hint;
    persistent_literal_hints.function_entry_hints.push_back(
        {0x8C010060u, 1u});
    const auto persistent_indexed_literal =
        katana::analysis::analyze_control_flow(
            caller_bounded_indexed_literal_image(true, true, 0x0656u),
            &persistent_literal_hints);
    const auto* persistent_literal_handler =
        find_function(persistent_indexed_literal, 0x8C010060u);
    const auto* persistent_literal_guard =
        find_guarded_aot_entry(persistent_indexed_literal, 0x8C010060u);
    require(
        persistent_literal_handler != nullptr &&
            persistent_literal_handler->origins ==
                std::vector{
                    katana::analysis::FunctionOrigin::StoredCodeAddress} &&
            persistent_literal_guard != nullptr &&
            persistent_literal_guard->origins ==
                std::vector{
                    katana::analysis::GuardedAotEntryOrigin::
                        StoredCodeAddress} &&
            persistent_literal_guard->source_sites ==
                std::vector<std::uint32_t>{0x8C010002u,
                                           0x8C010012u,
                                           0x8C01002Cu} &&
            persistent_literal_guard->source_objects ==
                std::vector<std::uint32_t>{0x8C010020u} &&
            has_instruction(persistent_indexed_literal, 0x8C010060u) &&
            persistent_indexed_literal.static_callback_contracts_materialized &&
            katana::analysis::guarded_aot_inventory_complete(
                persistent_indexed_literal),
        "Callergebundener, global indexierter Funktionsliteral-Store "
        "erreichte die bewachte native AOT-Closure nicht.");

    const auto unknown_caller_indexed_literal =
        katana::analysis::analyze_control_flow(
            caller_bounded_indexed_literal_image(false, true, 0x0656u),
            &persistent_literal_hints);
    require(find_function(unknown_caller_indexed_literal, 0x8C010060u) ==
                nullptr &&
                find_guarded_aot_entry(unknown_caller_indexed_literal,
                                       0x8C010060u) == nullptr &&
                !has_instruction(unknown_caller_indexed_literal,
                                 0x8C010060u),
            "Unvollstaendige Caller-Domaene wurde als persistente "
            "Funktionszeiger-Closure akzeptiert.");

    const auto unproven_base_indexed_literal =
        katana::analysis::analyze_control_flow(
            caller_bounded_indexed_literal_image(true, false, 0x0656u),
            &persistent_literal_hints);
    require(find_function(unproven_base_indexed_literal, 0x8C010060u) ==
                nullptr &&
                find_guarded_aot_entry(unproven_base_indexed_literal,
                                       0x8C010060u) == nullptr,
            "Plausibler, aber nicht literal-identischer Tabellenzeiger wurde "
            "als persistente Funktionszeiger-Closure akzeptiert.");

    const auto narrow_store_indexed_literal =
        katana::analysis::analyze_control_flow(
            caller_bounded_indexed_literal_image(true, true, 0x0655u),
            &persistent_literal_hints);
    require(find_function(narrow_store_indexed_literal, 0x8C010060u) ==
                nullptr &&
                find_guarded_aot_entry(narrow_store_indexed_literal,
                                       0x8C010060u) == nullptr,
            "Nicht-32-bittiger Tabellenstore wurde als persistente "
            "Funktionszeiger-Closure akzeptiert.");

    const auto incomplete_source_indexed_literal =
        katana::analysis::analyze_control_flow(
            caller_bounded_indexed_literal_image(true, true, 0x0656u,
                                                  true),
            &persistent_literal_hints);
    require(find_function(incomplete_source_indexed_literal, 0x8C010060u) ==
                nullptr &&
                find_guarded_aot_entry(incomplete_source_indexed_literal,
                                       0x8C010060u) == nullptr,
            "Nur auf einem Storepfad vollstaendiger Code-Literalwert wurde "
            "als persistente Funktionszeiger-Closure akzeptiert.");

    const auto decode_boundary_phase_image = [] {
        std::vector<std::uint8_t> bytes(0x22u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] =
                static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] =
                static_cast<std::uint8_t>(value >> 24u);
        };
        put_u16(0x00u, 0xD303u); // immutable literal 0x10 -> r3
        put_u16(0x02u, 0xA001u); // bra 0x08
        put_u16(0x04u, 0x0009u);
        put_u16(0x08u, 0x430Bu); // jsr @r3 after local phase boundary
        put_u16(0x0Au, 0x0009u);
        put_u16(0x0Cu, 0x000Bu);
        put_u16(0x0Eu, 0x0009u);
        put_u32(0x10u, 0x20u);
        put_u16(0x20u, 0xFFFFu); // valid address, no instruction boundary

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.add_segment({".decode-boundary-phase",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           std::move(bytes)});
        image.add_entry_point(0u);
        return image;
    }();
    const auto decode_boundary_phase =
        katana::analysis::analyze_control_flow(
            decode_boundary_phase_image);
    const auto decode_boundary_call = std::find_if(
        decode_boundary_phase.indirect_control_flow.begin(),
        decode_boundary_phase.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x08u;
        });
    const auto decode_boundary_call_found =
        decode_boundary_call != decode_boundary_phase.indirect_control_flow.end();
    require(
        !decode_boundary_phase.function_budget_exhausted &&
            decode_boundary_call !=
                decode_boundary_phase.indirect_control_flow.end() &&
            decode_boundary_call->status ==
                katana::analysis::ResolutionStatus::Unresolved &&
            decode_boundary_call->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            decode_boundary_call->reason ==
                "runtime-contract-merged-contexts" &&
            decode_boundary_call->target == std::nullopt &&
            decode_boundary_call->targets.empty() &&
            decode_boundary_call->analysis_candidates ==
                std::vector<std::uint32_t>{0x20u} &&
            has_instruction(decode_boundary_phase, 0x20u) &&
            !std::binary_search(
                decode_boundary_phase.recursive
                    .proven_instruction_addresses.begin(),
                decode_boundary_phase.recursive
                    .proven_instruction_addresses.end(),
                0x20u),
        "Decode-Boundary-Phasenwechsel blieb zyklisch, exportierte einen "
        "stalen ProvenComplete-Vertrag oder verlor den Runtime-Kandidaten: "
        "found=" + std::to_string(decode_boundary_call_found) +
            " status=" +
            std::to_string(
                decode_boundary_call_found
                    ? static_cast<int>(decode_boundary_call->status)
                    : -1) +
            " evidence=" +
            std::to_string(
                decode_boundary_call_found
                    ? static_cast<int>(decode_boundary_call->evidence)
                    : -1) +
            " reason=" +
            (decode_boundary_call_found ? decode_boundary_call->reason
                                        : std::string{"missing"}) +
            " decoded=" +
            std::to_string(has_instruction(decode_boundary_phase, 0x20u)) +
            " proven=" +
            std::to_string(std::binary_search(
                decode_boundary_phase.recursive.proven_instruction_addresses.begin(),
                decode_boundary_phase.recursive.proven_instruction_addresses.end(),
                0x20u)));

    const auto reconciled_candidate_call_image = [] {
        std::vector<std::uint8_t> bytes(0x200u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };

        // A owns 65 outgoing stack facts. Only slot zero is a callback.
        put_u16(0x00u, 0xD045u); // mov.l @(0x118,pc),r0 -> callback 0x1E0
        put_u16(0x02u, 0xE201u); // mov #1,r2
        put_u16(0x04u, 0x61F3u); // mov r15,r1
        put_u16(0x06u, 0x2102u); // mov.l r0,@r1
        auto cursor = std::size_t{0x08u};
        for (std::size_t slot = 1u; slot <= 64u; ++slot) {
            static_cast<void>(slot);
            put_u16(cursor, 0x7104u); // add #4,r1
            put_u16(cursor + 2u, 0x2122u); // mov.l r2,@r1
            cursor += 4u;
        }
        put_u16(0x108u, 0xB01Au); // bsr 0x140
        put_u16(0x10Au, 0xE400u); // mov #0,r4 (delay)
        put_u16(0x10Cu, 0x000Bu);
        put_u16(0x10Eu, 0x0009u);
        put_u32(0x118u, 0x1E0u);

        // B's writable call literal is loaded before a basic-block boundary.
        // Local propagation drops it at 0x14C; the function CFG must recover
        // it and feed the resulting private candidate carrier back into ABI
        // contract construction.
        put_u16(0x140u, 0xD307u); // mov.l @(0x160,pc),r3 -> C 0x180
        put_u16(0x142u, 0xD708u); // mov.l @(0x164,pc),r7 -> fixed sink
        put_u16(0x144u, 0x2742u); // mov.l r4,@r7 (independent sink anchor)
        put_u16(0x146u, 0xA001u); // bra 0x14C
        put_u16(0x148u, 0x0009u);
        put_u16(0x14Au, 0x0009u);
        put_u16(0x14Cu, 0x430Bu); // jsr @r3
        put_u16(0x14Eu, 0x0009u);
        put_u16(0x150u, 0x000Bu);
        put_u16(0x152u, 0x0009u);
        put_u32(0x160u, 0x180u);
        put_u32(0x164u, 0x1F8u);

        // C consumes only ABI stack slot zero and persists that callback.
        put_u16(0x180u, 0x64F2u); // mov.l @r15,r4
        put_u16(0x182u, 0xD503u); // mov.l @(0x190,pc),r5
        put_u16(0x184u, 0x2542u); // mov.l r4,@r5
        put_u16(0x186u, 0x000Bu);
        put_u16(0x188u, 0x0009u);
        put_u32(0x190u, 0x1F0u);
        put_u16(0x1E0u, 0x000Bu); // persisted callback
        put_u16(0x1E2u, 0x0009u);

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".candidate-call-reconciliation",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-candidate-call-reconciliation"});
        image.add_entry_point(0u);
        return image;
    }();
    std::size_t candidate_contract_iteration = 0u;
    std::size_t candidate_contract_passes = 0u;
    std::size_t maximum_candidate_contract_passes = 0u;
    bool explicit_candidate_iterations = true;
    bool exact_round_seed_accounting = true;
    bool exact_cache_accounting = true;
    bool observed_multi_root_cfa_bridge = false;
    bool observed_incremental_seed_round = false;
    bool observed_root_artifact_progress = false;
    bool observed_function_value_terminal = false;
    std::vector<std::string> outer_terminal_phases;
    std::size_t terminal_result_index_copy_items = 0u;
    std::size_t terminal_result_index_sort_items = 0u;
    std::size_t terminal_result_index_materialized_items = 0u;
    const auto silent_progress_before =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    const auto silent_reconciled_candidate_call =
        katana::analysis::analyze_control_flow(
            reconciled_candidate_call_image, nullptr);
    const auto silent_progress_after =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    require(
        !silent_reconciled_candidate_call.function_budget_exhausted &&
            silent_progress_after.callback_activations ==
                silent_progress_before.callback_activations &&
            silent_progress_after.pulse_threads_started ==
                silent_progress_before.pulse_threads_started,
        "Deaktivierter CFA-Progress aktivierte trotzdem den internen "
        "Function-Value-Callback oder Heartbeat-Thread: budget=" +
            std::to_string(
                silent_reconciled_candidate_call.function_budget_exhausted) +
            " callback=" +
            std::to_string(silent_progress_before.callback_activations) +
            "->" +
            std::to_string(silent_progress_after.callback_activations) +
            " pulse=" +
            std::to_string(silent_progress_before.pulse_threads_started) +
            "->" +
            std::to_string(silent_progress_after.pulse_threads_started));
    const auto enabled_progress_before = silent_progress_after;
    const auto reconciled_candidate_call =
        katana::analysis::analyze_control_flow(
            reconciled_candidate_call_image,
            nullptr,
            [&](const katana::analysis::ControlFlowAnalysisProgress&
                    progress) {
                if (progress.phase ==
                        "analysis-terminal-materialization-start" ||
                    progress.phase == "analysis-terminal-materialized" ||
                    progress.phase == "complete")
                    outer_terminal_phases.push_back(progress.phase);
                observed_function_value_terminal =
                    observed_function_value_terminal ||
                    progress.phase ==
                        "function-values-terminal-materialized";
                if (progress.phase ==
                    "analysis-terminal-materialized") {
                    terminal_result_index_copy_items =
                        progress.result_index_copy_items;
                    terminal_result_index_sort_items =
                        progress.result_index_sort_items;
                    terminal_result_index_materialized_items =
                        progress.result_index_materialized_items;
                }
                exact_round_seed_accounting =
                    exact_round_seed_accounting &&
                    progress.seeds >=
                        progress.round_seed_baseline &&
                    progress.round_added_seeds ==
                        progress.seeds -
                            progress.round_seed_baseline &&
                    progress.growing_workset ==
                        (progress.round_seed_targets_changed != 0u ||
                         progress.round_decode_targets != 0u ||
                         progress.round_metadata_targets != 0u ||
                         progress.round_full_cpu_fallbacks != 0u);
                observed_incremental_seed_round =
                    observed_incremental_seed_round ||
                    (progress.iteration > 1u &&
                     progress.round_decode_targets != 0u &&
                     progress.growing_workset);
                if (progress.function_value_active) {
                    const auto explained_misses =
                        progress.function_value_session_cache_miss_cold +
                        progress.function_value_session_cache_miss_evicted +
                        progress.function_value_session_cache_miss_oversize_or_no_exact_replay +
                        progress.function_value_session_cache_miss_function_shape_changed +
                        progress.function_value_session_cache_miss_projected_ingress_changed +
                        progress.function_value_session_cache_miss_summary_dependency_changed +
                        progress.function_value_session_cache_miss_abi_contract_changed +
                        progress.function_value_session_cache_miss_resolution_lens_changed +
                        progress.function_value_session_cache_miss_inventory_sink_changed +
                        progress.function_value_session_cache_miss_isolation_partition_changed +
                        progress.function_value_session_cache_miss_contextual_summary_changed +
                        progress.function_value_session_cache_miss_tail_ingress_changed;
                    exact_cache_accounting =
                        exact_cache_accounting &&
                        progress.function_value_session_cache_lookups ==
                            progress.function_value_session_cache_ready_hits +
                                progress.function_value_session_cache_in_flight_coalesces +
                                progress.function_value_session_cache_misses &&
                        explained_misses ==
                            progress.function_value_session_cache_misses &&
                        progress.function_value_multi_root_context_requests ==
                            progress.function_value_multi_root_unique_contexts +
                                progress.function_value_multi_root_ready_reuses +
                                progress.function_value_multi_root_in_flight_reuses &&
                        progress.function_value_multi_root_retained_contexts ==
                            progress.function_value_multi_root_unique_contexts;
                    exact_cache_accounting =
                        exact_cache_accounting &&
                        progress
                                .function_value_resolution_root_artifacts_total ==
                            progress
                                    .function_value_resolution_root_artifacts_reused +
                                progress
                                    .function_value_resolution_root_artifacts_recomputed &&
                        progress.function_value_incremental_epochs_started >=
                            progress.function_value_analysis_epochs_published +
                                progress
                                    .function_value_analysis_epochs_discarded;
                    observed_multi_root_cfa_bridge =
                        observed_multi_root_cfa_bridge ||
                        (progress.function_value_multi_root_context_requests > 0u &&
                         progress.function_value_multi_root_unique_contexts > 0u &&
                         progress.function_value_multi_root_retained_contexts ==
                             progress.function_value_multi_root_unique_contexts &&
                         progress
                                 .function_value_multi_root_retained_payload_bytes >
                             0u);
                    observed_root_artifact_progress =
                        observed_root_artifact_progress ||
                        progress
                                .function_value_resolution_root_artifacts_total !=
                            0u;
                }
                if (progress.phase != "function-values-start" &&
                    progress.phase !=
                        "function-values-candidate-contract-reconcile")
                    return;
                if (candidate_contract_iteration !=
                    progress.iteration) {
                    candidate_contract_iteration =
                        progress.iteration;
                    candidate_contract_passes = 0u;
                }
                ++candidate_contract_passes;
                explicit_candidate_iterations =
                    explicit_candidate_iterations &&
                    progress.candidate_contract_iteration ==
                        candidate_contract_passes &&
                    progress.candidate_contract_iteration_budget ==
                        64u;
                maximum_candidate_contract_passes =
                    std::max(maximum_candidate_contract_passes,
                             candidate_contract_passes);
            });
    const auto enabled_progress_after =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    require(
        enabled_progress_after.callback_activations >
                enabled_progress_before.callback_activations &&
            enabled_progress_after.pulse_threads_started >
                enabled_progress_before.pulse_threads_started &&
            enabled_progress_after.detailed_cache_sessions_started ==
                enabled_progress_before.detailed_cache_sessions_started,
        "Aktiver CFA-Progress erreichte Callback-/Heartbeat-Bruecke nicht.");
    auto recursive_retry_image = reconciled_candidate_call_image;
    bool recursive_retry_mutation_applied = false;
    const auto recursive_retry_result =
        katana::analysis::analyze_control_flow(
            recursive_retry_image,
            nullptr,
            [&](const katana::analysis::ControlFlowAnalysisProgress&
                    progress) {
                if (recursive_retry_mutation_applied ||
                    !progress.growing_workset ||
                    (progress.phase != "seed-expansion" &&
                     progress.phase != "summary-seed-expansion"))
                    return;
                // Change only the revision, not the semantic bytes. The next
                // optimistic Recursive delta must return ColdRetry, CFA must
                // clear every dependent consumer and retry from the complete
                // authoritative seed contract.
                recursive_retry_image.write_u32_le(
                    0u, recursive_retry_image.read_u32_le(0u));
                recursive_retry_mutation_applied = true;
            });
    const auto retry_resolution = std::find_if(
        recursive_retry_result.indirect_control_flow.begin(),
        recursive_retry_result.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x14Cu;
        });
    require(
        recursive_retry_mutation_applied &&
            recursive_retry_result.recursive_full_recompute_fallbacks ==
                1u &&
            recursive_retry_result.persistent_analysis_bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::
                    RecursiveBaselineRejected &&
            retry_resolution !=
                recursive_retry_result.indirect_control_flow.end() &&
            retry_resolution->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            retry_resolution->analysis_candidates ==
                std::vector<std::uint32_t>{0x180u} &&
            recursive_retry_result.recursive.instructions.size() ==
                reconciled_candidate_call.recursive.instructions.size() &&
            recursive_retry_result.function_value_summaries.size() ==
                reconciled_candidate_call.function_value_summaries.size() &&
            recursive_retry_result.guarded_aot_entries.size() ==
                reconciled_candidate_call.guarded_aot_entries.size() &&
            recursive_retry_result.recursive_final_materializations == 1u,
        "Unerwarteter Recursive-Cold-Retry liess partiellen oder stale "
        "CFA-/FVA-Consumerstate im finalen Ergebnis zurueck.");
    const auto detailed_progress_before = enabled_progress_after;
    const auto detailed_reconciled_candidate_call =
        katana::analysis::analyze_control_flow(
            reconciled_candidate_call_image,
            nullptr,
            [](const katana::analysis::ControlFlowAnalysisProgress&) {},
            true);
    const auto detailed_progress_after =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    require(
        detailed_progress_after.detailed_cache_sessions_started ==
                detailed_progress_before.detailed_cache_sessions_started +
                    1u &&
            detailed_progress_after.callback_activations >
                detailed_progress_before.callback_activations &&
            !detailed_reconciled_candidate_call.function_budget_exhausted,
        "Explizite Detailed-Telemetrie blieb aus oder Basic-Progress "
        "aktivierte sie implizit.");
    const auto throwing_progress_analysis =
        katana::analysis::analyze_control_flow(
            reconciled_candidate_call_image,
            nullptr,
            [](const katana::analysis::ControlFlowAnalysisProgress&) {
                throw std::runtime_error(
                    "synthetic-cfa-progress-observer-failure");
            });
    require(
        throwing_progress_analysis.progress_callback_failed &&
            !throwing_progress_analysis.function_budget_exhausted,
        "Ein werfender CFA-Beobachter brach Produktanalyse ab oder blieb "
        "in der Verlusttelemetrie unsichtbar.");
    const auto reconciled_call = std::find_if(
        reconciled_candidate_call.indirect_control_flow.begin(),
        reconciled_candidate_call.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x14Cu;
        });
    const auto detailed_reconciled_call = std::find_if(
        detailed_reconciled_candidate_call.indirect_control_flow.begin(),
        detailed_reconciled_candidate_call.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x14Cu;
        });
    const auto* reconciled_candidate_function =
        find_function(reconciled_candidate_call, 0x180u);
    const auto find_seed_fact =
        [&reconciled_candidate_call](const std::uint32_t target) {
            return std::find_if(
                reconciled_candidate_call.seed_facts.begin(),
                reconciled_candidate_call.seed_facts.end(),
                [target](const auto& fact) {
                    return fact.target_address == target;
                });
        };
    const auto candidate_seed = find_seed_fact(0x180u);
    const auto stored_seed = find_seed_fact(0x1E0u);
    const auto has_seed_cause = [](const auto& fact,
                                   const auto kind,
                                   const std::uint32_t source) {
        return std::any_of(
            fact.causes.begin(),
            fact.causes.end(),
            [kind, source](const auto& cause) {
                return cause.kind == kind &&
                       cause.source_address.has_value() &&
                       *cause.source_address == source;
            });
    };
    using SeedCause =
        katana::analysis::ControlFlowAnalysisResult::SeedCause;
    using SeedCauseKind =
        katana::analysis::ControlFlowAnalysisResult::SeedCauseKind;
    const SeedCause absent_source{
        SeedCauseKind::StoredCodeAddress,
        std::nullopt,
        std::nullopt,
        std::nullopt};
    const SeedCause real_zero_source{
        SeedCauseKind::StoredCodeAddress,
        std::uint32_t{0u},
        std::nullopt,
        std::nullopt};
    require(
        absent_source != real_zero_source &&
            !absent_source.source_address.has_value() &&
            real_zero_source.source_address == 0u,
        "Die Seed-Provenienz vermischte eine fehlende Quelle mit der "
        "gueltigen Gastadresse null.");
    require(
        reconciled_call !=
                reconciled_candidate_call.indirect_control_flow.end() &&
            detailed_reconciled_call !=
                detailed_reconciled_candidate_call
                    .indirect_control_flow.end() &&
            detailed_reconciled_call->evidence ==
                reconciled_call->evidence &&
            detailed_reconciled_call->targets ==
                reconciled_call->targets &&
            detailed_reconciled_call->analysis_candidates ==
                reconciled_call->analysis_candidates &&
            !reconciled_candidate_call.function_budget_exhausted &&
            katana::analysis::guarded_aot_inventory_complete(
                reconciled_candidate_call) &&
            reconciled_call->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            reconciled_call->targets.empty() &&
            reconciled_call->analysis_candidates ==
                std::vector<std::uint32_t>{0x180u} &&
            reconciled_candidate_function != nullptr &&
            reconciled_candidate_function->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            std::binary_search(
                reconciled_candidate_call.recursive
                    .guarded_candidate_instruction_addresses.begin(),
                reconciled_candidate_call.recursive
                    .guarded_candidate_instruction_addresses.end(),
                0x180u) &&
            !std::binary_search(
                reconciled_candidate_call.recursive
                    .proven_instruction_addresses.begin(),
                reconciled_candidate_call.recursive
                    .proven_instruction_addresses.end(),
                0x180u) &&
            std::none_of(
                reconciled_candidate_call.resolved_edges.begin(),
                reconciled_candidate_call.resolved_edges.end(),
                [](const auto& edge) {
                    return edge.instruction_address == 0x14Cu &&
                           edge.target_address == 0x180u;
                }) &&
            reconciled_candidate_call.guarded_code_inventory_walk
                    .abi_stack_argument_projection_truncated_functions == 0u &&
            find_guarded_aot_entry(reconciled_candidate_call, 0x1E0u) !=
                nullptr,
        "Spaet entdeckter Function-Summary-Callcarrier wurde nicht bis zum "
        "ABI-/Inventarvertrag rueckgekoppelt oder als feste CFG-Kante "
        "eingefroren.");
    require(
            candidate_seed !=
                reconciled_candidate_call.seed_facts.end() &&
            stored_seed !=
                reconciled_candidate_call.seed_facts.end() &&
            has_seed_cause(
                *candidate_seed,
                katana::analysis::ControlFlowAnalysisResult::
                    SeedCauseKind::IndirectAnalysisCandidate,
                0x14Cu) &&
            has_seed_cause(
                *stored_seed,
                katana::analysis::ControlFlowAnalysisResult::
                    SeedCauseKind::StoredCodeAddress,
                0x184u) &&
            reconciled_candidate_call.seed_targets_added ==
                reconciled_candidate_call.seed_facts.size() &&
            reconciled_candidate_call.seed_causes_added >=
                reconciled_candidate_call.seed_facts.size() &&
            reconciled_candidate_call.recursive_incremental_passes != 0u &&
            reconciled_candidate_call
                    .recursive_full_recompute_fallbacks == 0u &&
            reconciled_candidate_call
                    .runtime_contract_normalization_full_scans == 1u &&
            reconciled_candidate_call
                    .decode_boundary_normalization_full_scans == 2u &&
            reconciled_candidate_call
                    .runtime_contract_normalization_entries_visited != 0u &&
            reconciled_candidate_call
                    .decode_boundary_normalization_entries_visited != 0u,
        "Der kanonische Seed-Ledger verlor Ursache, Quelladresse oder den "
        "inkrementellen Recursive-/Normalisierungsvertrag.");
    require(
            explicit_candidate_iterations &&
            maximum_candidate_contract_passes >= 1u &&
            maximum_candidate_contract_passes <= 2u,
        "Der Candidate-Contract meldete keine exakten Reconcile-Paesse.");
    require(
        exact_round_seed_accounting,
        "Die CFA-Progressbruecke verlor ihre exakte Seed-Rundenbilanz.");
    require(
        outer_terminal_phases ==
                std::vector<std::string>{
                    "analysis-terminal-materialization-start",
                    "analysis-terminal-materialized", "complete"} &&
            observed_function_value_terminal &&
            reconciled_candidate_call.recursive_final_materializations ==
                1u &&
            reconciled_candidate_call.recursive.physical_work
                    .public_materializations == 1u &&
            terminal_result_index_copy_items ==
                reconciled_candidate_call.result_index_copy_items &&
            terminal_result_index_sort_items ==
                reconciled_candidate_call.result_index_sort_items &&
            terminal_result_index_materialized_items ==
                reconciled_candidate_call
                    .result_index_materialized_items &&
            terminal_result_index_sort_items != 0u,
        "Terminalmaterialisierung oder ihre physischen "
        "Progresszaehler wurden mehrfach, unvollstaendig oder in falscher "
        "Reihenfolge publiziert.");
    require(
        exact_cache_accounting && observed_multi_root_cfa_bridge,
        "Die CFA-Progressbruecke verlor Cache- oder Multi-Root-Bilanzen.");
    require(
        observed_incremental_seed_round,
        "Die CFA-Progressbruecke belegte kein inkrementelles Seed-Delta.");
    require(
        observed_root_artifact_progress,
        "Die CFA-Progressbruecke belegte keine Root-Artefaktarbeit.");

    const auto tail_registered_callback_image = [] {
        std::vector<std::uint8_t> bytes(0xE0u, 0x09u);
        const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        bytes[0x00u] = 0x03u;
        bytes[0x01u] = 0xD4u; // mov.l @(0x10,pc),r4 -> callback 0x70
        bytes[0x02u] = 0x04u;
        bytes[0x03u] = 0xD3u; // mov.l @(0x14,pc),r3 -> wrapper 0x20
        bytes[0x04u] = 0x0Bu;
        bytes[0x05u] = 0x43u; // jsr @r3
        bytes[0x06u] = 0x09u;
        bytes[0x07u] = 0x00u; // nop (delay)
        bytes[0x08u] = 0x0Bu;
        bytes[0x09u] = 0x00u; // rts
        bytes[0x0Au] = 0x09u;
        bytes[0x0Bu] = 0x00u; // nop (delay)
        put_u32(0x10u, 0x70u);
        put_u32(0x14u, 0x20u);

        bytes[0x20u] = 0x46u;
        bytes[0x21u] = 0x2Fu; // mov.l r4,@-r15
        bytes[0x22u] = 0x03u;
        bytes[0x23u] = 0xD2u; // mov.l @(0x30,pc),r2 -> store target 0x40
        bytes[0x24u] = 0x2Bu;
        bytes[0x25u] = 0x42u; // jmp @r2
        bytes[0x26u] = 0xF6u;
        bytes[0x27u] = 0x65u; // mov.l @r15+,r5 (delay)
        put_u32(0x30u, 0x40u);

        bytes[0x40u] = 0xF0u;
        bytes[0x41u] = 0xE0u; // mov #-16,r0 (runtime frame size)
        bytes[0x42u] = 0x0Cu;
        bytes[0x43u] = 0x3Fu; // add r0,r15
        bytes[0x44u] = 0x51u;
        bytes[0x45u] = 0x1Fu; // mov.l r5,@(4,r15)
        bytes[0x46u] = 0x00u;
        bytes[0x47u] = 0xE5u; // mov #0,r5
        bytes[0x48u] = 0xF1u;
        bytes[0x49u] = 0x53u; // mov.l @(4,r15),r3
        bytes[0x4Au] = 0x62u;
        bytes[0x4Bu] = 0x62u; // mov.l @r6,r2 (runtime object)
        bytes[0x4Cu] = 0x32u;
        bytes[0x4Du] = 0x22u; // mov.l r3,@r2
        bytes[0x4Eu] = 0x10u;
        bytes[0x4Fu] = 0x7Fu; // add #16,r15
        bytes[0x50u] = 0x0Bu;
        bytes[0x51u] = 0x00u; // rts
        bytes[0x52u] = 0x09u;
        bytes[0x53u] = 0x00u; // nop (delay)

        bytes[0x70u] = 0x06u;
        bytes[0x71u] = 0xA0u; // bra 0x80
        bytes[0x72u] = 0x00u;
        bytes[0x73u] = 0xE4u; // mov #0,r4 (delay)
        bytes[0x80u] = 0x0Bu;
        bytes[0x81u] = 0x00u; // shared tail: rts
        bytes[0x82u] = 0x09u;
        bytes[0x83u] = 0x00u; // nop (delay)

        bytes[0x90u] = 0x0Eu;
        bytes[0x91u] = 0xB0u; // bsr 0xB0 -> callback-return helper
        bytes[0x92u] = 0x09u;
        bytes[0x93u] = 0x00u; // nop (delay)
        bytes[0x94u] = 0x03u;
        bytes[0x95u] = 0x64u; // mov r0,r4
        bytes[0x96u] = 0x03u;
        bytes[0x97u] = 0xD2u; // mov.l @(0xA4,pc),r2 -> registrar 0xC0
        bytes[0x98u] = 0x2Bu;
        bytes[0x99u] = 0x42u; // jmp @r2
        bytes[0x9Au] = 0x09u;
        bytes[0x9Bu] = 0x00u; // nop (delay)
        put_u32(0xA4u, 0xC0u);

        bytes[0xB0u] = 0x02u;
        bytes[0xB1u] = 0xD0u; // mov.l @(0xBC,pc),r0 -> callback 0xD0
        bytes[0xB2u] = 0x0Bu;
        bytes[0xB3u] = 0x00u; // rts
        bytes[0xB4u] = 0x09u;
        bytes[0xB5u] = 0x00u; // nop (delay)
        put_u32(0xBCu, 0xD0u);

        bytes[0xC0u] = 0x62u;
        bytes[0xC1u] = 0x62u; // mov.l @r6,r2 (runtime object)
        bytes[0xC2u] = 0x42u;
        bytes[0xC3u] = 0x22u; // mov.l r4,@r2
        bytes[0xC4u] = 0x0Bu;
        bytes[0xC5u] = 0x00u; // rts
        bytes[0xC6u] = 0x09u;
        bytes[0xC7u] = 0x00u; // nop (delay)

        bytes[0xD0u] = 0x0Bu;
        bytes[0xD1u] = 0x00u; // helper-returned callback: rts
        bytes[0xD2u] = 0x09u;
        bytes[0xD3u] = 0x00u; // nop (delay)

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".tail-registered-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-tail-registered-callback"});
        image.add_entry_point(0u);
        image.add_entry_point(0x90u);
        return image;
    };
    const auto tail_registered_callback =
        katana::analysis::analyze_control_flow(tail_registered_callback_image());
    const auto* tail_registered_handler = find_function(tail_registered_callback, 0x70u);
    const auto tail_transfer = std::find_if(
        tail_registered_callback.indirect_control_flow.begin(),
        tail_registered_callback.indirect_control_flow.end(),
        [](const auto& resolution) { return resolution.instruction_address == 0x24u; });
    require(find_function(tail_registered_callback, 0x40u) == nullptr &&
                has_instruction(tail_registered_callback, 0x40u) &&
                tail_registered_handler != nullptr &&
                tail_registered_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                tail_registered_handler->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                has_instruction(tail_registered_callback, 0x70u) &&
                has_instruction(tail_registered_callback, 0x80u),
            "Callbackprovenienz ging ueber einen terminalen Tail-Jump verloren.");
    const auto* direct_tail_argument =
        find_function(tail_registered_callback, 0xD0u);
    const auto direct_tail_transfer = std::find_if(
        tail_registered_callback.indirect_control_flow.begin(),
        tail_registered_callback.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x98u;
        });
    require(
        direct_tail_argument != nullptr &&
            direct_tail_argument->origins ==
                std::vector{
                    katana::analysis::FunctionOrigin::StoredCodeAddress} &&
            direct_tail_argument->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedPartial &&
            direct_tail_transfer !=
                tail_registered_callback.indirect_control_flow.end() &&
            direct_tail_transfer->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            direct_tail_transfer->targets.empty() &&
            direct_tail_transfer->analysis_candidates ==
                std::vector<std::uint32_t>{0xC0u},
        "Ueber einen normalen Helper zurueckgegebener PC-Literal-"
        "Codepointer wurde nicht bis zum Candidate-Tail-Registrarstore "
        "inventarisiert oder der Live-Tail wurde als feste CFG-Kante "
        "eingefroren.");
    require(tail_transfer != tail_registered_callback.indirect_control_flow.end() &&
                tail_transfer->kind == katana::analysis::IndirectControlFlowKind::Jump &&
                tail_transfer->status == katana::analysis::ResolutionStatus::Unresolved &&
                tail_transfer->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                !tail_transfer->target.has_value() &&
                tail_transfer->targets.empty() &&
                tail_transfer->analysis_candidates == std::vector<std::uint32_t>{0x40u} &&
                std::none_of(tail_registered_callback.resolved_edges.begin(),
                             tail_registered_callback.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.instruction_address == 0x24u ||
                                        edge.target_address == 0x70u;
                             }),
            "Tail-Jump-Inventar fror den Live-Transfer als statische Kante ein.");
    const auto* tail_ingress =
        find_guarded_aot_entry(tail_registered_callback, 0x40u);
    const auto* stored_tail_entry =
        find_guarded_aot_entry(tail_registered_callback, 0x70u);
    require(
        tail_ingress != nullptr &&
            tail_ingress->origins ==
                std::vector{
                    katana::analysis::GuardedAotEntryOrigin::TailIngress} &&
            tail_ingress->source_sites ==
                std::vector<std::uint32_t>{0x24u} &&
            tail_ingress->shared_body_address == 0x40u &&
            tail_ingress->entry_byte_extent == 2u,
        "Bewachter Tail-Ingress verlor Herkunft oder eigenstaendigen "
        "AOT-Entryvertrag.");
    require(
        stored_tail_entry != nullptr &&
            stored_tail_entry->origins ==
                std::vector{
                    katana::analysis::GuardedAotEntryOrigin::
                        StoredCodeAddress} &&
            std::binary_search(stored_tail_entry->source_sites.begin(),
                               stored_tail_entry->source_sites.end(),
                               0x4Cu) &&
            stored_tail_entry->shared_body_address == 0x80u &&
            stored_tail_entry->entry_byte_extent == 4u &&
            stored_tail_entry->source_identity.starts_with("sha256:") &&
            stored_tail_entry->entry_byte_identity.starts_with("sha256:"),
        "Bewachter BRA-Entry verlor Storeprovenienz, Delay-Slot-Bytes oder "
        "Shared-Body-Hinweis.");
    const auto tail_registered_ir =
        katana::ir::lower_program(tail_registered_callback);
    require(has_ir_block(tail_registered_ir, 0x40u) &&
                has_ir_block(tail_registered_ir, 0x70u) &&
                has_ir_block(tail_registered_ir, 0x80u) &&
                katana::ir::verify_program(tail_registered_ir).empty(),
            "Bewachte Entries und Shared Body wurden nicht als getrennte "
            "native IR-Blockstarts erhalten.");

    katana::analysis::AnalysisOverrides stored_callback_override;
    stored_callback_override.source_path = "stored-callback-override.txt";
    stored_callback_override.functions.push_back({0x30u, 1u});
    const auto forced_stored_callback = katana::analysis::analyze_control_flow(
        stored_callback_image, &stored_callback_override);
    const auto* forced_stored_handler = find_function(forced_stored_callback, 0x30u);
    require(forced_stored_handler != nullptr &&
                forced_stored_handler->evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride &&
                forced_stored_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::UserOverride,
                                katana::analysis::FunctionOrigin::StoredCodeAddress},
            "Expliziter Function-Override verlor im echten Seedmerge gegen GuardedPartial.");

    const auto partial_ingress_store_image = [](const bool non_stack_store,
                                                const bool stack_argument_destination = false) {
        std::vector<std::uint8_t> bytes(0x80u, 0x09u);
        bytes[0x00u] = 0x60u;
        bytes[0x01u] = 0xE5u; // mov #0x60,r5
        bytes[0x02u] = 0x1Du;
        bytes[0x03u] = 0xB0u; // bsr 0x40
        bytes[0x04u] = stack_argument_destination ? 0xF3u : 0x09u;
        bytes[0x05u] = stack_argument_destination ? 0x64u : 0x00u;
        // mov r15,r4 / nop (delay)
        bytes[0x06u] = 0x0Bu;
        bytes[0x07u] = 0x00u; // rts
        bytes[0x08u] = 0x09u;
        bytes[0x09u] = 0x00u; // nop (delay)
        bytes[0x40u] = 0x56u;
        bytes[0x41u] = 0x2Fu; // mov.l r5,@-r15
        bytes[0x42u] = 0xF6u;
        bytes[0x43u] = 0x65u; // mov.l @r15+,r5
        if (non_stack_store && !stack_argument_destination) {
            bytes[0x44u] = 0x20u;
            bytes[0x45u] = 0xE2u; // mov #0x20,r2 (proven non-stack)
            bytes[0x46u] = 0x52u;
            bytes[0x47u] = 0x22u; // mov.l r5,@r2
            bytes[0x48u] = 0x0Bu;
            bytes[0x49u] = 0x00u; // rts
            bytes[0x4Au] = 0x09u;
            bytes[0x4Bu] = 0x00u; // nop (delay)
        } else {
            bytes[0x44u] = 0x52u;
            bytes[0x45u] =
                stack_argument_destination ? 0x24u : 0x2Fu;
            // mov.l r5,@r4 / mov.l r5,@r15
            bytes[0x46u] = 0x0Bu;
            bytes[0x47u] = 0x00u; // rts
            bytes[0x48u] = 0x09u;
            bytes[0x49u] = 0x00u; // nop (delay)
        }
        bytes[0x60u] = 0x0Bu;
        bytes[0x61u] = 0x00u; // handler: rts
        bytes[0x62u] = 0x09u;
        bytes[0x63u] = 0x00u; // nop (delay)
        bytes[0x70u] = 0x22u;
        bytes[0x71u] = 0x61u; // mov.l @r2,r1
        bytes[0x72u] = 0x0Bu;
        bytes[0x73u] = 0x41u; // jsr @r1
        bytes[0x74u] = 0x09u;
        bytes[0x75u] = 0x00u; // nop (delay)
        bytes[0x76u] = 0x0Bu;
        bytes[0x77u] = 0x00u; // rts
        bytes[0x78u] = 0x09u;
        bytes[0x79u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({non_stack_store ? ".partial-non-stack-store"
                                           : ".partial-stack-store",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           non_stack_store ? "synthetic-partial-non-stack-store"
                                           : "synthetic-partial-stack-store"});
        image.add_entry_point(0u);
        image.add_entry_point(0x40u); // unknown additional registrar ingress
        image.add_entry_point(0x70u);
        return image;
    };

    const auto partial_non_stack_store = katana::analysis::analyze_control_flow(
        partial_ingress_store_image(true));
    const auto* partial_non_stack_handler = find_function(partial_non_stack_store, 0x60u);
    const auto partial_runtime_dispatch =
        std::find_if(partial_non_stack_store.indirect_control_flow.begin(),
                     partial_non_stack_store.indirect_control_flow.end(),
                     [](const auto& resolution) {
                         return resolution.instruction_address == 0x72u;
                     });
    require(partial_non_stack_handler != nullptr &&
                partial_non_stack_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                partial_non_stack_handler->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                has_instruction(partial_non_stack_store, 0x60u) &&
                std::binary_search(
                    partial_non_stack_store.recursive.guarded_candidate_instruction_addresses
                        .begin(),
                    partial_non_stack_store.recursive.guarded_candidate_instruction_addresses
                        .end(),
                    0x60u) &&
                partial_runtime_dispatch !=
                    partial_non_stack_store.indirect_control_flow.end() &&
                partial_runtime_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                partial_runtime_dispatch->targets.empty() &&
                partial_runtime_dispatch->analysis_candidates.empty() &&
                std::none_of(partial_non_stack_store.resolved_edges.begin(),
                             partial_non_stack_store.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.instruction_address == 0x72u ||
                                        edge.target_address == 0x60u;
                             }),
            "Partieller Callerwert erreichte nach Stackspill keinen bewachten "
            "Non-Stack-Store oder fror den Runtime-Dispatcher statisch ein.");

    auto no_abi_partial_store_image = partial_ingress_store_image(true);
    no_abi_partial_store_image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    const auto no_abi_partial_store =
        katana::analysis::analyze_control_flow(no_abi_partial_store_image);
    require(find_function(no_abi_partial_store, 0x60u) == nullptr &&
                !has_instruction(no_abi_partial_store, 0x60u),
            "Partielle Callsite-Ernte lief ohne belegten Gast-ABI-Vertrag.");

    const auto partial_stack_store =
        katana::analysis::analyze_control_flow(partial_ingress_store_image(false));
    require(find_function(partial_stack_store, 0x60u) == nullptr &&
                !has_instruction(partial_stack_store, 0x60u),
            "Partieller Callerwert an einem Stackstore wurde als Callback katalogisiert.");

    const auto partial_argument_stack_store =
        katana::analysis::analyze_control_flow(partial_ingress_store_image(true, true));
    require(find_function(partial_argument_stack_store, 0x60u) == nullptr &&
                !has_instruction(partial_argument_stack_store, 0x60u),
            "Partielle Callsite-Ernte verlor Stackprovenienz eines Argumentzeigers.");

    const auto branch_merged_stack_callback_image = [] {
        std::vector<std::uint8_t> bytes(0x80u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        put_u16(0x00u, 0xE560u); // mov #0x60,r5
        put_u16(0x02u, 0xB01Du); // bsr 0x40
        put_u16(0x04u, 0x66F3u); // mov r15,r6 (caller-stack alias; delay)
        put_u16(0x06u, 0x000Bu); // rts
        put_u16(0x08u, 0x0009u); // nop (delay)
        put_u16(0x40u, 0xEC20u); // mov #0x20,r12 (proven non-stack)
        put_u16(0x42u, 0x2F56u); // mov.l r5,@-r15
        put_u16(0x44u, 0x8902u); // bt 0x4c
        put_u16(0x46u, 0x0236u); // mov.l r3,@(r0,r2)
        put_u16(0x48u, 0xA002u); // bra 0x50
        put_u16(0x4Au, 0x0009u); // nop (delay)
        put_u16(0x4Cu, 0xB010u); // bsr 0x70 with escaped caller-stack alias
        put_u16(0x4Eu, 0x0009u); // nop (delay)
        put_u16(0x50u, 0x62F6u); // mov.l @r15+,r2
        put_u16(0x52u, 0x2C22u); // mov.l r2,@r12
        put_u16(0x54u, 0x000Bu); // rts
        put_u16(0x56u, 0x0009u); // nop (delay)
        put_u16(0x60u, 0x000Bu); // callback: rts
        put_u16(0x62u, 0x0009u); // nop (delay)
        put_u16(0x70u, 0x000Bu); // nested callee: rts
        put_u16(0x72u, 0x0009u); // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".branch-merged-stack-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-branch-merged-stack-callback"});
        image.add_entry_point(0u);
        image.add_entry_point(0x40u); // unknown additional registrar ingress
        return image;
    }();
    const auto branch_merged_stack_callback =
        katana::analysis::analyze_control_flow(branch_merged_stack_callback_image);
    const auto* branch_merged_handler =
        find_function(branch_merged_stack_callback, 0x60u);
    require(branch_merged_handler != nullptr &&
                branch_merged_handler->origins ==
                    std::vector{
                        katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                branch_merged_handler->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedPartial &&
                has_instruction(branch_merged_stack_callback, 0x60u) &&
                std::none_of(branch_merged_stack_callback.resolved_edges.begin(),
                             branch_merged_stack_callback.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.target_address == 0x60u;
                             }),
            "Ein pfadweise erhaltener Callback-Stackspill ging am Registrar-Join "
            "verloren oder wurde als feste CFG-Kante missbraucht.");

    const auto independent_partial_store_image = [](const std::size_t callback_count) {
        require(callback_count != 0u && callback_count <= 64u,
                "Guarded-Code-Inventarfixture erhielt ungueltige Groesse.");
        std::vector<std::uint8_t> bytes(0xA20u, 0x09u);
        std::vector<std::uint32_t> callback_addresses;
        callback_addresses.reserve(callback_count);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        for (std::size_t index = 0u; index < callback_count; ++index) {
            const auto offset = index * 6u;
            const auto literal_address = 0x400u + index * 4u;
            const auto pc_base = (offset + 4u) & ~std::size_t{3u};
            const auto literal_displacement = (literal_address - pc_base) / 4u;
            require(literal_displacement <= 0xFFu,
                    "Guarded-Code-Inventarfixture ueberschritt MOV.L-PC-Bereich.");
            put_u16(offset,
                    static_cast<std::uint16_t>(0xD400u | literal_displacement));
            const auto call_address = static_cast<std::uint32_t>(offset + 2u);
            const auto displacement =
                static_cast<std::uint16_t>((0x800u - (call_address + 4u)) / 2u);
            put_u16(offset + 2u, static_cast<std::uint16_t>(0xB000u | displacement));
            put_u16(offset + 4u, 0x0009u); // nop (delay)
            const auto callback = static_cast<std::uint32_t>(0x900u + index * 4u);
            callback_addresses.push_back(callback);
            put_u32(literal_address, callback);
            put_u16(callback, 0x000Bu); // callback: rts
            put_u16(callback + 2u, 0x0009u); // nop (delay)
        }
        put_u16(callback_count * 6u, 0x000Bu); // rts
        put_u16(callback_count * 6u + 2u, 0x0009u); // nop (delay)
        put_u16(0x800u, 0xE220u); // mov #0x20,r2 (proven non-stack)
        put_u16(0x802u, 0x2242u); // mov.l r4,@r2
        put_u16(0x804u, 0x000Bu); // rts
        put_u16(0x806u, 0x0009u); // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".independent-partial-store",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-independent-partial-store"});
        image.add_entry_point(0u);
        image.add_entry_point(0x800u); // unknown additional registrar ingress
        return std::pair{std::move(image), callback_addresses};
    };
    for (const auto callback_count : {9u, 16u, 64u}) {
        const auto independent_image =
            independent_partial_store_image(callback_count);
        const auto independent_partial_store =
            katana::analysis::analyze_control_flow(independent_image.first);
        require(
            std::count_if(independent_partial_store.recursive.instructions.begin(),
                          independent_partial_store.recursive.instructions.end(),
                          [](const auto& line) {
                              return line.instruction.control_flow ==
                                         katana::sh4::ControlFlowKind::Call &&
                                     line.target_address == 0x800u;
                          }) == static_cast<std::ptrdiff_t>(callback_count) &&
                std::all_of(independent_image.second.begin(),
                            independent_image.second.end(),
                            [&](const auto address) {
                                const auto* callback =
                                    find_function(independent_partial_store, address);
                                return callback != nullptr &&
                                       callback->origins ==
                                           std::vector{
                                               katana::analysis::FunctionOrigin::
                                                   StoredCodeAddress} &&
                                       callback->evidence ==
                                           katana::analysis::ControlFlowEvidence::
                                               GuardedPartial &&
                                       has_instruction(independent_partial_store, address);
                            }) &&
                std::none_of(independent_partial_store.resolved_edges.begin(),
                             independent_partial_store.resolved_edges.end(),
                             [&](const auto& edge) {
                                 return std::binary_search(independent_image.second.begin(),
                                                           independent_image.second.end(),
                                                           edge.target_address);
                             }) &&
                independent_partial_store.guarded_code_inventory_candidates >=
                    callback_count &&
                independent_partial_store.guarded_code_inventory_budget >= 64u &&
                !independent_partial_store.candidate_inventory_truncated,
            "Guarded-Code-Inventar verlor 9/16/64 unabhaengige Callerwerte, "
            "fror sie als CFG-Kanten ein oder meldete falsche Truncation.");
    }

    const auto destination_forwarded_callback_image = [](const bool proven_non_stack) {
        std::vector<std::uint8_t> bytes(0x40u, 0x09u);
        if (proven_non_stack) {
            bytes[0x00u] = 0x38u;
            bytes[0x01u] = 0xE4u; // mov #0x38,r4 (proven non-stack object)
            bytes[0x02u] = 0x0Du;
            bytes[0x03u] = 0xB0u; // bsr 0x20
            bytes[0x04u] = 0x09u;
            bytes[0x05u] = 0x00u; // nop (delay)
            bytes[0x06u] = 0x0Bu;
            bytes[0x07u] = 0x00u; // rts
            bytes[0x08u] = 0x09u;
            bytes[0x09u] = 0x00u; // nop (delay)
        } else {
            bytes[0x00u] = 0x0Eu;
            bytes[0x01u] = 0xB0u; // bsr 0x20 with unknown r4 provenance
            bytes[0x02u] = 0x09u;
            bytes[0x03u] = 0x00u; // nop (delay)
            bytes[0x04u] = 0x0Bu;
            bytes[0x05u] = 0x00u; // rts
            bytes[0x06u] = 0x09u;
            bytes[0x07u] = 0x00u; // nop (delay)
        }
        bytes[0x20u] = 0x00u;
        bytes[0x21u] = 0xE0u; // mov #0,r0 (field offset)
        bytes[0x22u] = 0x02u;
        bytes[0x23u] = 0xD2u; // mov.l @(0x2c,pc),r2 -> local handler 0x30
        bytes[0x24u] = 0x26u;
        bytes[0x25u] = 0x04u; // mov.l r2,@(r0,r4)
        bytes[0x26u] = 0x0Bu;
        bytes[0x27u] = 0x00u; // rts
        bytes[0x28u] = 0x09u;
        bytes[0x29u] = 0x00u; // nop (delay)
        bytes[0x2Cu] = 0x30u;
        bytes[0x2Du] = 0x00u;
        bytes[0x2Eu] = 0x00u;
        bytes[0x2Fu] = 0x00u;
        bytes[0x30u] = 0x0Bu;
        bytes[0x31u] = 0x00u; // handler: rts
        bytes[0x32u] = 0x09u;
        bytes[0x33u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".destination-forwarded-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-destination-forwarded-callback"});
        image.add_entry_point(0u);
        return image;
    };
    const auto destination_forwarded_callback =
        katana::analysis::analyze_control_flow(
            destination_forwarded_callback_image(true));
    const auto* destination_forwarded_handler =
        find_function(destination_forwarded_callback, 0x30u);
    require(destination_forwarded_handler != nullptr &&
                destination_forwarded_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                has_instruction(destination_forwarded_callback, 0x30u),
            "Lokal geladener Codepointer mit Aufrufprovenienz am Storeziel "
            "erreichte das bewachte AOT-Inventar nicht.");
    const auto unknown_destination_callback =
        katana::analysis::analyze_control_flow(
            destination_forwarded_callback_image(false));
    require(find_function(unknown_destination_callback, 0x30u) == nullptr &&
                !has_instruction(unknown_destination_callback, 0x30u),
            "Unbekannte Callsite-Stackprovenienz wurde als bewiesenes "
            "Non-Stack-Callbackobjekt missverstanden.");

    const auto widened_destination_callback_image = [] {
        std::vector<std::uint8_t> bytes(0x70u, 0x09u);
        constexpr std::array<std::uint8_t, 5u> destination_values{
            0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u};
        constexpr std::array<std::uint8_t, 5u> call_displacements{
            0x1Du, 0x1Au, 0x17u, 0x14u, 0x11u};
        for (std::size_t index = 0u; index < destination_values.size(); ++index) {
            const auto offset = index * 6u;
            bytes[offset] = destination_values[index];
            bytes[offset + 1u] = 0xE4u; // mov #destination,r4
            bytes[offset + 2u] = call_displacements[index];
            bytes[offset + 3u] = 0xB0u; // bsr 0x40
            bytes[offset + 4u] = 0x09u;
            bytes[offset + 5u] = 0x00u; // nop (delay)
        }
        bytes[0x1Eu] = 0x0Bu;
        bytes[0x1Fu] = 0x00u; // rts
        bytes[0x20u] = 0x09u;
        bytes[0x21u] = 0x00u; // nop (delay)
        bytes[0x40u] = 0x29u;
        bytes[0x41u] = 0x00u; // movt r0 -> {0, 1}
        bytes[0x42u] = 0x0Cu;
        bytes[0x43u] = 0x34u; // add r0,r4 -> ten values, widened to unknown
        bytes[0x44u] = 0x02u;
        bytes[0x45u] = 0xD2u; // mov.l @(0x50,pc),r2 -> local handler 0x60
        bytes[0x46u] = 0x22u;
        bytes[0x47u] = 0x24u; // mov.l r2,@r4
        bytes[0x48u] = 0x0Bu;
        bytes[0x49u] = 0x00u; // rts
        bytes[0x4Au] = 0x09u;
        bytes[0x4Bu] = 0x00u; // nop (delay)
        bytes[0x50u] = 0x60u;
        bytes[0x51u] = 0x00u;
        bytes[0x52u] = 0x00u;
        bytes[0x53u] = 0x00u;
        bytes[0x60u] = 0x0Bu;
        bytes[0x61u] = 0x00u; // handler: rts
        bytes[0x62u] = 0x09u;
        bytes[0x63u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".widened-destination-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-widened-destination-callback"});
        image.add_entry_point(0u);
        return image;
    }();
    const auto widened_destination_callback =
        katana::analysis::analyze_control_flow(widened_destination_callback_image);
    const auto* widened_destination_handler =
        find_function(widened_destination_callback, 0x60u);
    require(widened_destination_handler != nullptr &&
                widened_destination_handler->origins ==
                    std::vector{katana::analysis::FunctionOrigin::StoredCodeAddress} &&
                has_instruction(widened_destination_callback, 0x60u),
            "Wertmengen-Widening verlor die Aufrufprovenienz des Callback-Storeziels.");

    const auto forwarded_store_image = [](std::vector<std::uint8_t> registrar,
                                          std::string name) {
        std::vector<std::uint8_t> bytes(0x50u, 0x09u);
        bytes[0x00u] = 0x03u;
        bytes[0x01u] = 0xD4u; // mov.l @(0x10,pc),r4 -> handler 0x30
        bytes[0x02u] = 0x04u;
        bytes[0x03u] = 0xD3u; // mov.l @(0x14,pc),r3 -> registrar 0x20
        bytes[0x04u] = 0x0Bu;
        bytes[0x05u] = 0x43u; // jsr @r3
        bytes[0x06u] = 0x09u;
        bytes[0x07u] = 0x00u; // nop (delay)
        bytes[0x08u] = 0x0Bu;
        bytes[0x09u] = 0x00u; // rts
        bytes[0x0Au] = 0x09u;
        bytes[0x0Bu] = 0x00u; // nop (delay)
        bytes[0x10u] = 0x30u;
        bytes[0x14u] = 0x20u;
        std::copy(registrar.begin(), registrar.end(), bytes.begin() + 0x20u);
        bytes[0x30u] = 0x0Bu;
        bytes[0x31u] = 0x00u; // handler: rts
        bytes[0x32u] = 0x09u;
        bytes[0x33u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({"." + name,
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-" + name});
        image.add_entry_point(0u);
        return image;
    };

    const auto stack_pointer = katana::analysis::analyze_control_flow(
        forwarded_store_image(
            {0x46u, 0x2Fu, 0x0Bu, 0x00u, 0x09u, 0x00u}, "stack-pointer"));
    require(find_function(stack_pointer, 0x30u) == nullptr &&
                !has_instruction(stack_pointer, 0x30u),
            "Ein normaler Stackspill wurde faelschlich als gespeicherter Callback katalogisiert.");

    const auto indexed_stack_pointer = katana::analysis::analyze_control_flow(
        forwarded_store_image({0xF3u,
                               0x60u,
                               0x46u,
                               0x02u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u},
                              "indexed-stack-pointer"));
    require(find_function(indexed_stack_pointer, 0x30u) == nullptr &&
                !has_instruction(indexed_stack_pointer, 0x30u),
            "Ein R0-indizierter Stackspill wurde faelschlich als Callback katalogisiert.");

    std::vector<std::uint8_t> ordinary_data_pointer_bytes(0x30u, 0x09u);
    ordinary_data_pointer_bytes[0x00u] = 0x03u;
    ordinary_data_pointer_bytes[0x01u] = 0xD4u; // mov.l @(0x10,pc),r4 -> 0x20
    ordinary_data_pointer_bytes[0x02u] = 0x42u;
    ordinary_data_pointer_bytes[0x03u] = 0x22u; // mov.l r4,@r2
    ordinary_data_pointer_bytes[0x04u] = 0x0Bu;
    ordinary_data_pointer_bytes[0x05u] = 0x00u; // rts
    ordinary_data_pointer_bytes[0x06u] = 0x09u;
    ordinary_data_pointer_bytes[0x07u] = 0x00u; // nop (delay)
    ordinary_data_pointer_bytes[0x10u] = 0x20u;
    ordinary_data_pointer_bytes[0x20u] = 0x0Bu;
    ordinary_data_pointer_bytes[0x22u] = 0x09u;
    katana::io::ExecutableImage ordinary_data_pointer_image;
    ordinary_data_pointer_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    ordinary_data_pointer_image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    ordinary_data_pointer_image.add_segment({".ordinary-data-pointer",
                                             0u,
                                             0u,
                                             ordinary_data_pointer_bytes.size(),
                                             katana::io::SegmentKind::Mixed,
                                             {true, true, true},
                                             std::move(ordinary_data_pointer_bytes),
                                             katana::io::ImageSourceKind::DiscBootFile,
                                             katana::io::ImageLoadPhase::Initial,
                                             "synthetic-ordinary-data-pointer"});
    ordinary_data_pointer_image.add_entry_point(0u);
    const auto ordinary_data_pointer =
        katana::analysis::analyze_control_flow(ordinary_data_pointer_image);
    require(find_function(ordinary_data_pointer, 0x20u) == nullptr &&
                !has_instruction(ordinary_data_pointer, 0x20u),
            "Ein gewoehnlicher codeaehnlicher Datenpointer wurde als Callback katalogisiert.");

    const auto dereferenced_callback_argument_image = [] {
        std::vector<std::uint8_t> bytes(0x60u, 0x09u);
        bytes[0x00u] = 0x50u;
        bytes[0x01u] = 0xE4u; // mov #0x50,r4 (descriptor, not callback)
        bytes[0x02u] = 0x0Du;
        bytes[0x03u] = 0xB0u; // bsr 0x20
        bytes[0x04u] = 0x09u;
        bytes[0x05u] = 0x00u; // nop (delay)
        bytes[0x06u] = 0x0Bu;
        bytes[0x07u] = 0x00u; // rts
        bytes[0x08u] = 0x09u;
        bytes[0x09u] = 0x00u; // nop (delay)
        bytes[0x20u] = 0x42u;
        bytes[0x21u] = 0x66u; // mov.l @r4,r6
        bytes[0x22u] = 0x0Du;
        bytes[0x23u] = 0xB0u; // bsr 0x40
        bytes[0x24u] = 0x09u;
        bytes[0x25u] = 0x00u; // nop (delay)
        bytes[0x26u] = 0x0Bu;
        bytes[0x27u] = 0x00u; // rts
        bytes[0x28u] = 0x09u;
        bytes[0x29u] = 0x00u; // nop (delay)
        bytes[0x40u] = 0x0Bu;
        bytes[0x41u] = 0x46u; // jsr @r6
        bytes[0x42u] = 0x09u;
        bytes[0x43u] = 0x00u; // nop (delay)
        bytes[0x44u] = 0x0Bu;
        bytes[0x45u] = 0x00u; // rts
        bytes[0x46u] = 0x09u;
        bytes[0x47u] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".dereferenced-callback-argument",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-dereferenced-callback-argument"});
        image.add_entry_point(0u);
        return image;
    }();
    katana::analysis::AnalysisOverrides dereferenced_callback_hints;
    dereferenced_callback_hints.mode =
        katana::analysis::AnalysisDirectiveMode::Hint;
    dereferenced_callback_hints.function_entry_hints.push_back({0x40u, 1u});
    const auto dereferenced_callback_argument =
        katana::analysis::analyze_control_flow(
            dereferenced_callback_argument_image,
            &dereferenced_callback_hints);
    const auto callback_consumer = std::find_if(
        dereferenced_callback_argument.static_callback_sinks.begin(),
        dereferenced_callback_argument.static_callback_sinks.end(),
        [](const auto& sink) { return sink.function_address == 0x40u; });
    const auto descriptor_consumer = std::find_if(
        dereferenced_callback_argument.static_callback_sinks.begin(),
        dereferenced_callback_argument.static_callback_sinks.end(),
        [](const auto& sink) { return sink.function_address == 0x20u; });
    require(callback_consumer !=
                dereferenced_callback_argument.static_callback_sinks.end() &&
                callback_consumer->argument_mask == 0x04u &&
                descriptor_consumer ==
                    dereferenced_callback_argument.static_callback_sinks.end() &&
                dereferenced_callback_argument.static_callback_contracts_materialized,
            "Ein durch r4 adressierter Deskriptor wurde selbst als Callbackargument katalogisiert.");

    const auto record_table_callback_image = [](const bool preserve_receiver) {
        std::vector<std::uint8_t> bytes(0x140u, 0x09u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };

        // Root both the constructor and consumer in one authenticated image.
        put_u16(0x00u, 0xB00Eu); // bsr 0x20
        put_u16(0x02u, 0x0009u); // nop (delay)
        put_u16(0x04u, 0xB03Cu); // bsr 0x80
        put_u16(0x06u, 0x0009u); // nop (delay)
        put_u16(0x08u, 0x000Bu); // rts
        put_u16(0x0Au, 0x0009u); // nop (delay)

        // Constructor: incoming r5 is stored at record +16, while the same
        // record (incoming r6) is published into table[r4].
        put_u16(0x20u, 0x6E63u); // mov r6,r14
        put_u16(0x22u, 0x1E54u); // mov.l r5,@(16,r14)
        put_u16(0x24u, 0x6043u); // mov r4,r0
        put_u16(0x26u, 0x4008u); // shll2 r0
        put_u16(0x28u, 0xD306u); // mov.l @(24,pc),r3 -> 0x44
        put_u16(0x2Au, 0x303Cu); // add r3,r0
        put_u16(0x2Cu, 0x20E2u); // mov.l r14,@r0
        put_u16(0x2Eu, 0x000Bu); // rts
        put_u16(0x30u, 0x0009u); // nop (delay)
        put_u32(0x44u, 0x00000100u);

        // Consumer: load the record from the same exact table and invoke
        // record->callback with the record preserved in outgoing r4.
        put_u16(0x80u, 0xD007u); // mov.l @(28,pc),r0 -> 0xA0
        put_u16(0x82u, 0x4408u); // shll2 r4
        put_u16(0x84u, 0x044Eu); // mov.l @(r0,r4),r4
        put_u16(0x86u, 0x5344u); // mov.l @(16,r4),r3
        put_u16(0x88u, 0x430Bu); // jsr @r3
        put_u16(0x8Au,
                preserve_receiver ? 0x0009u : 0xE400u); // delay
        put_u16(0x8Cu, 0x000Bu); // rts
        put_u16(0x8Eu, 0x0009u); // nop (delay)
        put_u32(0xA0u, 0x00000100u);

        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".record-table-callback",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-record-table-callback"});
        image.add_entry_point(0u);
        return image;
    };
    const auto record_table_callback =
        katana::analysis::analyze_control_flow(
            record_table_callback_image(true));
    const auto record_table_constructor = std::find_if(
        record_table_callback.static_callback_sinks.begin(),
        record_table_callback.static_callback_sinks.end(),
        [](const auto& sink) { return sink.function_address == 0x20u; });
    require(record_table_constructor !=
                record_table_callback.static_callback_sinks.end() &&
                record_table_constructor->argument_mask == 0x02u &&
                record_table_constructor->record_argument_mask == 0x02u,
            "Eine exakt publizierte Record-Tabelle hat den Callback(record)-ABI-Vertrag verloren.");

    const auto clobbered_record_table_callback =
        katana::analysis::analyze_control_flow(
            record_table_callback_image(false));
    const auto clobbered_record_table_constructor = std::find_if(
        clobbered_record_table_callback.static_callback_sinks.begin(),
        clobbered_record_table_callback.static_callback_sinks.end(),
        [](const auto& sink) { return sink.function_address == 0x20u; });
    require(clobbered_record_table_constructor ==
                clobbered_record_table_callback.static_callback_sinks.end() ||
                clobbered_record_table_constructor->record_argument_mask == 0u,
            "Ein im Delay-Slot ueberschriebener Record-Empfaenger wurde als Callback(record) akzeptiert.");

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

    const auto displaced_snapshot_image = [] {
        std::vector<std::uint8_t> bytes(0xA0u, 0u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_u32 = [&bytes](const std::size_t offset,
                                      const std::uint32_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        const std::array<std::uint16_t, 9> opcodes{
            0xD30Bu, 0x2452u, 0x6233u, 0x0009u, 0x2452u,
            0x2452u, 0x5323u, 0x430Bu, 0x0009u};
        for (std::size_t index = 0u; index < opcodes.size(); ++index)
            put_u16(index * 2u, opcodes[index]);
        put_u16(0x12u, 0x000Bu);
        put_u16(0x14u, 0x0009u);
        put_u32(0x30u, 0x3040u);
        for (std::size_t index = 0u; index < 4u; ++index) {
            put_u32(0x4Cu + index * 4u,
                    0x3080u + static_cast<std::uint32_t>(index * 4u));
            put_u16(0x80u + index * 4u, 0x000Bu);
            put_u16(0x82u + index * 4u, 0x0009u);
        }
        put_u32(0x5Cu, 1u);
        katana::io::ExecutableImage image;
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.add_segment({".displaced-snapshot",
                           0x3000u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-displaced-snapshot"});
        image.add_entry_point(0x3000u);
        return image;
    };
    const auto displaced_snapshot =
        katana::analysis::analyze_control_flow(displaced_snapshot_image());
    const auto displaced_table = std::find_if(
        displaced_snapshot.jump_tables.begin(),
        displaced_snapshot.jump_tables.end(),
        [](const auto& table) {
            return table.dispatch_address == 0x300Eu;
        });
    const std::vector<std::uint32_t> displaced_targets{
        0x3080u, 0x3084u, 0x3088u, 0x308Cu};
    require(displaced_table != displaced_snapshot.jump_tables.end() &&
                displaced_table->resolved &&
                displaced_table->aot_candidates_only &&
                displaced_table->table_address == 0x304Cu &&
                displaced_table->reason ==
                    "snapshot-displaced-absolute-pointer-candidates" &&
                displaced_table->entries.size() == displaced_targets.size(),
            "Displaced Callbacktabelle erreichte den Kontrollflussfixpunkt nicht.");
    const auto displaced_resolution = std::find_if(
        displaced_snapshot.indirect_control_flow.begin(),
        displaced_snapshot.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x300Eu;
        });
    require(displaced_resolution != displaced_snapshot.indirect_control_flow.end() &&
                displaced_resolution->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                displaced_resolution->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                displaced_resolution->targets.empty() &&
                displaced_resolution->analysis_candidates == displaced_targets &&
                std::none_of(displaced_snapshot.resolved_edges.begin(),
                             displaced_snapshot.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.instruction_address == 0x300Eu;
                             }),
            "Displaced Snapshotkandidaten wurden eingefroren oder zu CFG-Kanten erhoben.");
    for (const auto target : displaced_targets) {
        require(has_instruction(displaced_snapshot, target) &&
                    std::binary_search(
                        displaced_snapshot.recursive.guarded_candidate_instruction_addresses
                            .begin(),
                        displaced_snapshot.recursive.guarded_candidate_instruction_addresses.end(),
                        target),
                "Displaced Snapshotziel wurde nicht als bewachter AOT-Kandidat dekodiert.");
    }

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
                relative_table_analysis->authority ==
                    katana::analysis::JumpTableAuthority::SnapshotCandidate &&
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

        for (const auto& [source_kind, load_phase, policy] : std::array{
                 std::tuple{katana::io::ImageSourceKind::DiscBootFile,
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
            const auto resolution =
                std::find_if(rejected.indirect_control_flow.begin(),
                             rejected.indirect_control_flow.end(),
                             [](const auto& candidate) {
                                 return candidate.instruction_address == relative_table_dispatch;
                             });
            require(resolution != rejected.indirect_control_flow.end() &&
                        resolution->status == katana::analysis::ResolutionStatus::Unresolved &&
                        resolution->evidence ==
                            katana::analysis::ControlFlowEvidence::RuntimeOnly &&
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

    // Product post-bootstrap images are writable as a coarse segment. This
    // compiler-shaped selector assembles the byte offset with forward
    // conditional branches instead of a canonical compare/shift pair. The
    // finite value proof must derive {0,2,4,6}, authenticate the complete
    // producer slice and still reject any declaration whose entry count does
    // not exactly match the native table.
    katana::io::ExecutableImage identity_bound_braf_image;
    identity_bound_braf_image.add_segment(
        {".post-bootstrap",
         0u,
         0u,
         0x34u,
         katana::io::SegmentKind::Mixed,
         {true, true, true},
         {0x00u, 0xE1u, // mov #0,r1
          0x01u, 0x89u, // bt 0x08
          0x02u, 0x71u, // add #2,r1
          0x09u, 0x00u, // nop
          0x01u, 0x89u, // bt 0x0E
          0x04u, 0x71u, // add #4,r1
          0x09u, 0x00u, // nop
          0x03u, 0xC7u, // mova 0x1C,r0
          0x1Du, 0x01u, // mov.w @(r0,r1),r1
          0x07u, 0xE2u, // mov #7,r2 (independent scheduling)
          0x01u, 0x72u, // add #1,r2 (preserves loaded r1)
          0x23u, 0x01u, // braf r1; producer is intentionally non-adjacent
          0x09u, 0x00u, // nop (delay)
          0x09u, 0x00u, // padding
          0x0Au, 0x00u, // table[0] -> 0x24 (base 0x1A)
          0x0Eu, 0x00u, // table[1] -> 0x28
          0x12u, 0x00u, // table[2] -> 0x2C
          0x16u, 0x00u, // table[3] -> 0x30
          0x0Bu, 0x00u, 0x09u, 0x00u, // 0x24: rts / nop
          0x0Bu, 0x00u, 0x09u, 0x00u, // 0x28: rts / nop
          0x0Bu, 0x00u, 0x09u, 0x00u, // 0x2C: rts / nop
          0x0Bu, 0x00u, 0x09u, 0x00u}}); // 0x30: rts / nop
    identity_bound_braf_image.add_entry_point(0u);
    identity_bound_braf_image.add_immutable_range(
        {0u, 0x1Au, "synthetic-braf-producer-v2", 0u});
    identity_bound_braf_image.add_immutable_range(
        {0x1Cu, 8u, "synthetic-braf-table-v2", 0u});
    katana::analysis::AnalysisOverrides identity_bound_braf_override;
    identity_bound_braf_override.source_path =
        "identity-bound-braf-table.txt";
    katana::analysis::JumpTableOverride identity_bound_braf_table{
        0x16u,
        0x1Cu,
        4u,
        1u,
        sizeof(std::uint16_t),
        0x1Au,
        katana::analysis::JumpTableOverrideEncoding::SignedRelative16,
        katana::analysis::JumpTableOverrideTransfer::Jump};
    identity_bound_braf_table.identity_bound_complete = true;
    identity_bound_braf_override.jump_tables.push_back(
        identity_bound_braf_table);
    const auto identity_bound_braf =
        katana::analysis::analyze_control_flow(
            identity_bound_braf_image, &identity_bound_braf_override);
    const auto identity_bound_braf_resolution = std::find_if(
        identity_bound_braf.indirect_control_flow.begin(),
        identity_bound_braf.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == 0x16u;
        });
    require(
        identity_bound_braf_resolution !=
                identity_bound_braf.indirect_control_flow.end() &&
            identity_bound_braf_resolution->status ==
                katana::analysis::ResolutionStatus::Resolved &&
            identity_bound_braf_resolution->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedComplete &&
            identity_bound_braf_resolution->targets ==
                std::vector<std::uint32_t>{0x24u, 0x28u, 0x2Cu, 0x30u},
        "Eine exakt gebundene BRAF-Tabelle verlor ihren begrenzten "
        "nicht-clobbernden Producer-Nachweis.");

    auto interior_root_braf_image = identity_bound_braf_image;
    interior_root_braf_image.add_entry_point(0x0Au);
    const auto interior_root_braf =
        katana::analysis::analyze_control_flow(
            interior_root_braf_image, &identity_bound_braf_override);
    const auto interior_root_table = std::find_if(
        interior_root_braf.jump_tables.begin(),
        interior_root_braf.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == 0x16u; });
    require(interior_root_table != interior_root_braf.jump_tables.end() &&
                interior_root_table->evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride,
            "Ein externer Function-Root hinter dem endlichen Index-Seed "
            "umging den globalen Ingressvertrag.");

    auto resume_entry_braf_override = identity_bound_braf_override;
    resume_entry_braf_override.external_entry_hints.push_back({0x0Au, 0u});
    const auto resume_entry_braf =
        katana::analysis::analyze_control_flow(
            identity_bound_braf_image, &resume_entry_braf_override);
    const auto resume_entry_table = std::find_if(
        resume_entry_braf.jump_tables.begin(),
        resume_entry_braf.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == 0x16u; });
    require(resume_entry_table != resume_entry_braf.jump_tables.end() &&
                resume_entry_table->evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride &&
                std::none_of(
                    resume_entry_braf.recursive.functions.begin(),
                    resume_entry_braf.recursive.functions.end(),
                    [](const auto& function) {
                        return function.address == 0x0Au;
                    }),
            "Ein nicht-rootender AOT-Resume-Entry hinter dem endlichen "
            "Index-Seed umging den globalen Ingressvertrag oder erzeugte "
            "selbst Reichweite.");

    auto architectural_resume_braf_image = identity_bound_braf_image;
    architectural_resume_braf_image.write_u32_le(
        0x0Cu, 0xC703FBFDu); // frchg; mova 0x1C,r0
    const auto architectural_resume_braf =
        katana::analysis::analyze_control_flow(
            architectural_resume_braf_image,
            &identity_bound_braf_override);
    const auto architectural_resume_table = std::find_if(
        architectural_resume_braf.jump_tables.begin(),
        architectural_resume_braf.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == 0x16u; });
    require(
        architectural_resume_table !=
                architectural_resume_braf.jump_tables.end() &&
            architectural_resume_table->evidence ==
                katana::analysis::ControlFlowEvidence::ForcedOverride,
        "Eine architektonische Safepoint-Fortsetzung hinter dem endlichen "
        "Index-Seed wurde als geschlossener Producer zugelassen.");

    auto distant_ingress_braf_image = identity_bound_braf_image;
    distant_ingress_braf_image.add_segment(
        {".distant-ingress",
         0x80u,
         0x80u,
         4u,
         katana::io::SegmentKind::Code,
         {true, false, true},
         {0xC3u, 0xAFu, // bra 0x0A from outside the 48-instruction window
          0x09u, 0x00u}});
    distant_ingress_braf_image.add_entry_point(0x80u);
    const auto distant_ingress_braf =
        katana::analysis::analyze_control_flow(
            distant_ingress_braf_image, &identity_bound_braf_override);
    const auto distant_ingress_table = std::find_if(
        distant_ingress_braf.jump_tables.begin(),
        distant_ingress_braf.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == 0x16u; });
    require(distant_ingress_table != distant_ingress_braf.jump_tables.end() &&
                distant_ingress_table->evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride,
            "Ein entfernter direkter CFG-Vorgaenger hinter dem endlichen "
            "Index-Seed umging den globalen Ingressvertrag.");

    const auto identity_bound_braf_lines = katana::sh4::disassemble(
        identity_bound_braf_image.segments().front().bytes, 0u);
    const auto identity_bound_braf_native =
        katana::analysis::recognize_bounded_relative_jump_table(
            identity_bound_braf_image, identity_bound_braf_lines, 11u);
    require(identity_bound_braf_native.has_value() &&
                identity_bound_braf_native->resolved &&
                identity_bound_braf_native->entries.size() == 4u,
            "Der native BRAF-Recognizer leitete die endliche "
            "branch-assemblierte Indexmenge nicht vollstaendig her.");

    auto short_braf_override = identity_bound_braf_override;
    short_braf_override.jump_tables.front().entry_count = 3u;
    const auto short_braf = katana::analysis::analyze_control_flow(
        identity_bound_braf_image, &short_braf_override);
    const auto short_braf_table = std::find_if(
        short_braf.jump_tables.begin(),
        short_braf.jump_tables.end(),
        [](const auto& table) { return table.dispatch_address == 0x16u; });
    require(short_braf_table != short_braf.jump_tables.end() &&
                short_braf_table->evidence ==
                    katana::analysis::ControlFlowEvidence::ForcedOverride,
            "Eine zu kurze BRAF-Deklaration umging die native "
            "Entry-Count-Bindung.");

    auto clobbered_braf_image = identity_bound_braf_image;
    clobbered_braf_image.write_u32_le(
        0x12u, 0x7201E107u); // mov #7,r1; add #1,r2
    const auto clobbered_braf_lines = katana::sh4::disassemble(
        clobbered_braf_image.segments().front().bytes, 0u);
    const auto clobbered_braf_native =
        katana::analysis::recognize_bounded_relative_jump_table(
            clobbered_braf_image, clobbered_braf_lines, 11u);
    require(
        !clobbered_braf_native.has_value() ||
            !clobbered_braf_native->resolved,
        "Eine BRAF-Tabelle schloss trotz Clobber des geladenen "
        "Branchregisters statisch.");

    auto delay_slot_braf_lines = katana::sh4::disassemble(
        identity_bound_braf_image.segments().front().bytes, 0u);
    delay_slot_braf_lines[8u].is_delay_slot = true;
    const auto delay_slot_braf_native =
        katana::analysis::recognize_bounded_relative_jump_table(
            identity_bound_braf_image, delay_slot_braf_lines, 11u);
    require(
        !delay_slot_braf_native.has_value() ||
            !delay_slot_braf_native->resolved,
        "Eine BRAF-Tabelle verwendete einen Delay-Slot als "
        "geradlinigen Producer.");

    katana::analysis::OwnerSemanticSummary ordered_callee;
    ordered_callee.status =
        katana::analysis::OwnerSemanticSummaryStatus::Complete;
    ordered_callee.authority =
        katana::analysis::OwnerSemanticAuthority::IdentityBound;
    ordered_callee.boundary.entry_address = 0x2000u;
    ordered_callee.boundary.size = 2u;
    ordered_callee.result.complete = true;
    ordered_callee.digest = "sha256:synthetic-callee";
    katana::analysis::OwnerSemanticEffect callee_effect;
    callee_effect.instruction_address = 0x2000u;
    callee_effect.kind =
        katana::analysis::OwnerSemanticEffectKind::MemoryWrite;
    callee_effect.path_identity = "callee:block0";
    ordered_callee.effects.push_back(std::move(callee_effect));

    katana::ir::Instruction ordered_call;
    ordered_call.source_address = 0x1000u;
    ordered_call.operation = katana::ir::Operation::Call;
    ordered_call.target_address = 0x2000u;
    ordered_call.delay_slot = {
        katana::ir::DelaySlotRole::Owner, 0x1002u};
    katana::ir::Instruction ordered_slot;
    ordered_slot.source_address = 0x1002u;
    ordered_slot.operation = katana::ir::Operation::StoreLong;
    ordered_slot.memory_effects.access =
        katana::ir::MemoryAccessKind::Write;
    ordered_slot.memory_effects.width = katana::ir::OperandWidth::Bits32;
    ordered_slot.memory_effects.access_count = 1u;
    ordered_slot.source_register = 4u;
    ordered_slot.effective_address = 0x3000u;
    ordered_slot.delay_slot = {
        katana::ir::DelaySlotRole::Slot, 0x1000u};
    katana::ir::Instruction ordered_return;
    ordered_return.source_address = 0x1004u;
    ordered_return.operation = katana::ir::Operation::Return;
    katana::ir::BasicBlock ordered_block;
    ordered_block.start_address = 0x1000u;
    ordered_block.instructions = {
        ordered_call, ordered_slot, ordered_return};
    katana::ir::Function ordered_caller;
    ordered_caller.entry_address = 0x1000u;
    ordered_caller.blocks.push_back(std::move(ordered_block));
    const std::array ordered_call_evidence{
        katana::analysis::OwnerSemanticDirectCallEvidence{
            0x1000u, 0x2000u, &ordered_callee}};
    const auto ordered_summary =
        katana::analysis::summarize_owner_semantics(
            ordered_caller,
            {0x1000u,
             6u,
             "sha256:synthetic-caller",
             true,
             true},
            {},
            {},
            ordered_call_evidence);
    require(
        ordered_summary.effects.size() == 2u &&
            ordered_summary.effects[0].instruction_address == 0x1002u &&
            ordered_summary.effects[1].instruction_address == 0x2000u &&
            ordered_summary.direct_calls.size() == 1u,
        "Direktaufruf-Effekte wurden vor dem physischen Delay Slot "
        "komponiert.");

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

    auto exact_function_image = code_image(
        {0x0Bu, 0x00u, // root rts
         0x09u, 0x00u, // root delay slot
         0x09u, 0x00u, // unreachable padding
         0x09u, 0x00u, // unreachable padding
         0x09u, 0x00u, // exact external function
         0x09u, 0x00u, // exact external function
         0x0Bu, 0x00u, // adjacent bytes must not be absorbed
         0x09u, 0x00u});
    katana::analysis::AnalysisOverrides exact_function_override;
    exact_function_override.source_path = "exact-function.txt";
    exact_function_override.functions.push_back({8u, 21u, 4u});
    const auto exact_function = katana::analysis::analyze_control_flow(
        exact_function_image, &exact_function_override);
    const auto* exact_candidate = find_function(exact_function, 8u);
    const auto exact_program = katana::ir::lower_program(exact_function);
    const auto exact_ir =
        std::find_if(exact_program.begin(),
                     exact_program.end(),
                     [](const auto& function) {
                         return function.entry_address == 8u;
                     });
    require(exact_candidate != nullptr &&
                exact_candidate->size == 4u &&
                has_instruction(exact_function, 8u) &&
                has_instruction(exact_function, 10u) &&
                !has_instruction(exact_function, 12u) &&
                exact_ir != exact_program.end() &&
                exact_ir->blocks.size() == 1u &&
                exact_ir->blocks.front().instructions.size() == 2u &&
                exact_ir->blocks.front().instructions.front().source_address ==
                    8u &&
                exact_ir->blocks.front().instructions.back().source_address ==
                    10u &&
                katana::ir::verify_program(exact_program).empty() &&
                katana::analysis::format_control_flow_analysis_json(
                    exact_function)
                        .find("\"address\":\"0x00000008\","
                              "\"confidence\":\"certain\","
                              "\"evidence\":\"forced-override\","
                              "\"size\":4") != std::string::npos,
            "Explizite Funktionsgroesse erreicht Analyzer, CFG und natives "
            "AOT-Seeding nicht exakt.");

    katana::analysis::AnalysisOverrides exact_delay_slot_override;
    exact_delay_slot_override.source_path = "exact-delay-slot.txt";
    exact_delay_slot_override.functions.push_back({2u, 22u, 2u});
    const auto exact_delay_slot_error = failure([&] {
        static_cast<void>(katana::analysis::analyze_control_flow(
            exact_function_image, &exact_delay_slot_override));
    });
    require(exact_delay_slot_error.find(
                "Explizite Funktionsgrenze beginnt in einem Delay Slot.") !=
                std::string::npos,
            "Explizite Funktionsgrenze auf einem Delay Slot wurde nicht "
            "fail-closed abgewiesen.");

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
    zero_override.functions.push_back({0u, 4u, 8u});
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
