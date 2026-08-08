#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"
#include "../../src/analysis/guarded_native_entry_shape.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void set_stack_diagnostics_for_serial_fixpoint(const bool enabled) {
#ifdef _WIN32
    require(
        _putenv_s(
            "CODEX_ANALYZER_STACK_DIAGNOSTICS",
            enabled ? "1" : "") == 0,
        "Der Test konnte den seriellen Analyzer-Diagnosemodus nicht setzen.");
#else
    const auto result =
        enabled
            ? ::setenv(
                  "CODEX_ANALYZER_STACK_DIAGNOSTICS",
                  "1",
                  1)
            : ::unsetenv("CODEX_ANALYZER_STACK_DIAGNOSTICS");
    require(
        result == 0,
        "Der Test konnte den seriellen Analyzer-Diagnosemodus nicht setzen.");
#endif
}

void require_same_function_value_semantics(
    const katana::analysis::FunctionValueAnalysisResult& serial,
    const katana::analysis::FunctionValueAnalysisResult& parallel,
    const bool compare_work_counters = true) {
    const auto& serial_inventory = serial.guarded_code_inventory;
    const auto& parallel_inventory = parallel.guarded_code_inventory;
    auto serial_walk_diagnostics = serial_inventory.walk_diagnostics;
    auto parallel_walk_diagnostics = parallel_inventory.walk_diagnostics;
    serial_walk_diagnostics.forwarded_store_evaluation_cache_hits = 0u;
    serial_walk_diagnostics.forwarded_store_evaluation_cache_misses = 0u;
    parallel_walk_diagnostics.forwarded_store_evaluation_cache_hits = 0u;
    parallel_walk_diagnostics.forwarded_store_evaluation_cache_misses = 0u;
    const auto report_mismatch = [](const bool mismatch,
                                    const std::string_view field) {
        if (mismatch)
            std::cerr << "FUNCTION-VALUE-DIFFERENZ: " << field << '\n';
    };
    report_mismatch(serial.summaries != parallel.summaries, "summaries");
    report_mismatch(serial.resolutions != parallel.resolutions, "resolutions");
    report_mismatch(
        serial_inventory.stored_code_addresses !=
            parallel_inventory.stored_code_addresses,
        "stored_code_addresses");
    report_mismatch(
        serial_inventory.returned_code_address_tables !=
            parallel_inventory.returned_code_address_tables,
        "returned_code_address_tables");
    report_mismatch(
        serial_walk_diagnostics != parallel_walk_diagnostics,
        "walk_diagnostics");
    report_mismatch(
        compare_work_counters &&
            serial.fixpoint_iterations != parallel.fixpoint_iterations,
        "fixpoint_iterations");
    if (compare_work_counters &&
        serial.fixpoint_iterations != parallel.fixpoint_iterations)
        std::cerr << "  serial=" << serial.fixpoint_iterations
                  << " parallel=" << parallel.fixpoint_iterations << '\n';
    report_mismatch(
        compare_work_counters &&
            serial.unchanged_ingress_skips !=
                parallel.unchanged_ingress_skips,
        "unchanged_ingress_skips");
    if (compare_work_counters &&
        serial.unchanged_ingress_skips != parallel.unchanged_ingress_skips)
        std::cerr << "  serial=" << serial.unchanged_ingress_skips
                  << " parallel=" << parallel.unchanged_ingress_skips
                  << '\n';
    if (compare_work_counters &&
        (serial.fixpoint_iterations != parallel.fixpoint_iterations ||
         serial.unchanged_ingress_skips !=
             parallel.unchanged_ingress_skips)) {
        std::cerr
            << "  scheduler serial={workers="
            << serial.fixpoint_worker_count
            << ", batches=" << serial.fixpoint_parallel_batches
            << ", speculative="
            << serial.fixpoint_speculative_evaluations
            << ", stale=" << serial.fixpoint_stale_repairs
            << ", max_batch="
            << serial.maximum_fixpoint_batch_size
            << "} parallel={workers="
            << parallel.fixpoint_worker_count
            << ", batches=" << parallel.fixpoint_parallel_batches
            << ", speculative="
            << parallel.fixpoint_speculative_evaluations
            << ", stale=" << parallel.fixpoint_stale_repairs
            << ", max_batch="
            << parallel.maximum_fixpoint_batch_size << "}\n";
    }
    report_mismatch(
        serial.strongly_connected_components !=
            parallel.strongly_connected_components,
        "strongly_connected_components");
    report_mismatch(
        serial.budget_exhausted != parallel.budget_exhausted,
        "budget_exhausted");
    require(
        serial.summaries == parallel.summaries &&
            serial.resolutions == parallel.resolutions &&
            serial_inventory.stored_code_addresses ==
                parallel_inventory.stored_code_addresses &&
            serial_inventory.returned_code_address_tables ==
                parallel_inventory.returned_code_address_tables &&
            serial_inventory.raw_stored_candidate_budget ==
                parallel_inventory.raw_stored_candidate_budget &&
            serial_inventory.raw_stored_candidate_count ==
                parallel_inventory.raw_stored_candidate_count &&
            serial_inventory.candidate_budget ==
                parallel_inventory.candidate_budget &&
            serial_inventory.candidate_count ==
                parallel_inventory.candidate_count &&
            serial_inventory.shape_validation_work ==
                parallel_inventory.shape_validation_work &&
            serial_inventory.shape_validation_work_budget ==
                parallel_inventory.shape_validation_work_budget &&
            serial_inventory.shape_budget_exceeded_candidates ==
                parallel_inventory.shape_budget_exceeded_candidates &&
            serial_inventory.raw_stored_candidates_truncated ==
                parallel_inventory.raw_stored_candidates_truncated &&
            serial_inventory.candidate_budget_exhausted ==
                parallel_inventory.candidate_budget_exhausted &&
            serial_inventory.candidate_inventory_truncated ==
                parallel_inventory.candidate_inventory_truncated &&
            serial_inventory.table_scan_truncated ==
                parallel_inventory.table_scan_truncated &&
            serial_walk_diagnostics ==
                parallel_walk_diagnostics &&
            (!compare_work_counters ||
             (serial.fixpoint_iterations ==
                  parallel.fixpoint_iterations &&
              serial.unchanged_ingress_skips ==
                  parallel.unchanged_ingress_skips)) &&
            serial.strongly_connected_components ==
                parallel.strongly_connected_components &&
            serial.iteration_budget ==
                parallel.iteration_budget &&
            serial.budget_exhausted ==
                parallel.budget_exhausted,
        "Der versionsvalidierte Parallel-Fixpunkt wich semantisch vom "
        "exakten seriellen FIFO-Ergebnis ab.");
}

katana::io::ExecutableImage
image_with_callee(const std::vector<std::uint8_t>& callee);

void verify_persistent_function_value_session() {
    const std::vector<std::uint8_t> callee{
        0x10u,
        0xE0u, // mov #0x10,r0
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u  // nop (delay)
    };
    const auto image = image_with_callee(callee);
    const auto lines =
        katana::sh4::disassemble(
            image.segments().front().bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{{0u, 0u}, {0x20u, 0u}}};

    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        first_shapes{image};
    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        cold_progress;
    const auto first =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                [&cold_progress](const auto& progress) {
                    cold_progress.push_back(progress);
                },
                first_shapes,
                session);
    const auto first_stats = session.statistics();

    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        warm_progress;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        second_shapes{image};
    const auto second =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                [&warm_progress](const auto& progress) {
                    warm_progress.push_back(progress);
                },
                second_shapes,
                session);
    const auto second_stats = session.statistics();
    const auto progress_ledgers_balanced = [](const auto& events) {
        return std::all_of(
            events.begin(),
            events.end(),
            [](const auto& progress) {
                const auto settled =
                    progress.analysis_epochs_published +
                    progress.analysis_epochs_discarded;
                return progress.resolution_root_artifacts_total ==
                           progress.resolution_root_artifacts_reused +
                               progress
                                   .resolution_root_artifacts_recomputed &&
                       settled <= progress.incremental_epochs_started &&
                       (progress.phase != "complete" ||
                        settled ==
                            progress.incremental_epochs_started);
            });
    };
    require_same_function_value_semantics(first, second, false);
    require(
        second_stats.hits >= first_stats.hits &&
            second_stats.entries != 0u &&
            second_stats.retained_payload_bytes != 0u &&
            first_stats.program_graph_builds == 1u &&
            first_stats.program_graph_reuses == 0u &&
            first_stats.analysis_epochs_published == 1u &&
            first_stats.analysis_epochs_discarded == 0u &&
            first_stats.incremental_epochs_started == 1u &&
            first_stats.resolution_root_artifacts_reused == 0u &&
            first_stats.resolution_root_artifacts_recomputed != 0u &&
            second_stats.program_graph_builds == 1u &&
            second_stats.program_graph_reuses == 1u &&
            second_stats.program_graph_functions_reused >=
                boundaries.size() &&
            second_stats.abi_contract_epoch_reuses == 1u &&
            second_stats.summary_state_reuses >= boundaries.size() &&
            second_stats.analysis_epochs_published == 2u &&
            second_stats.analysis_epochs_discarded == 0u &&
            second_stats.incremental_epochs_started == 2u &&
            second_stats.resolution_root_artifacts_reused >
                first_stats.resolution_root_artifacts_reused &&
            second_stats.resolution_root_artifacts_recomputed ==
                first_stats.resolution_root_artifacts_recomputed,
        "Die analyseweite Function-Value-Session lieferte im identischen "
        "zweiten Kandidatenvertrag keinen echten Warm-Hit oder keine "
        "atomar wiederverwendete Graph-/ABI-/Summary-Epoch.");
    require(
        !cold_progress.empty() &&
            cold_progress.back().phase == "complete" &&
            cold_progress.back().active_evaluation_requests == 0u &&
            cold_progress.back().active_cache_key_builds == 0u &&
            cold_progress.back().active_cache_waits == 0u &&
            cold_progress.back().active_cache_replays == 0u &&
            cold_progress.back().active_physical_evaluations == 0u &&
            cold_progress.back().active_cache_commits == 0u &&
            cold_progress.back().logical_evaluations != 0u &&
            cold_progress.back().cache_key_builds ==
                cold_progress.back().logical_evaluations &&
            cold_progress.back().physical_evaluations != 0u &&
            cold_progress.back().cache_commits != 0u &&
            cold_progress.back().evaluation_request_nanoseconds != 0u &&
            cold_progress.back().cache_key_build_nanoseconds != 0u &&
            cold_progress.back().physical_evaluation_nanoseconds != 0u &&
            cold_progress.back().cache_commit_nanoseconds != 0u &&
            !warm_progress.empty() &&
            warm_progress.front().phase == "start" &&
            warm_progress.back().phase == "complete" &&
            warm_progress.back().summarized_functions ==
                warm_progress.back().functions &&
            warm_progress.back().resolution_functions_committed <=
                warm_progress.back().resolution_functions_total &&
            warm_progress.back().active_workers == 0u &&
            warm_progress.back().resolution_root_artifacts_total ==
                warm_progress.back().resolution_root_artifacts_reused +
                    warm_progress.back()
                        .resolution_root_artifacts_recomputed &&
            warm_progress.back().resolution_root_artifacts_reused != 0u &&
            warm_progress.back().resolution_root_artifacts_recomputed == 0u &&
            warm_progress.back().physical_evaluations <=
                warm_progress.back().logical_evaluations &&
            warm_progress.back().program_graph_builds == 0u &&
            warm_progress.back().program_graph_reuses == 1u &&
            warm_progress.back().program_graph_functions_reused >=
                boundaries.size() &&
            warm_progress.back().abi_contract_epoch_reuses == 1u &&
            warm_progress.back().summary_state_reuses >=
                boundaries.size() &&
            warm_progress.back().analysis_epochs_published == 1u &&
            warm_progress.back().analysis_epochs_discarded == 0u &&
            warm_progress.back().incremental_epochs_started == 1u &&
            warm_progress.back().incremental_epochs_started ==
                warm_progress.back().analysis_epochs_published +
                    warm_progress.back().analysis_epochs_discarded &&
            progress_ledgers_balanced(cold_progress) &&
            progress_ledgers_balanced(warm_progress) &&
            warm_progress.back().cache_key_builds ==
                warm_progress.back().logical_evaluations &&
            (warm_progress.back().logical_evaluations == 0u ||
             (warm_progress.back().evaluation_request_nanoseconds != 0u &&
              warm_progress.back().cache_key_build_nanoseconds != 0u)),
        "Der Function-Value-Progress meldete keinen sauberen "
        "analyse-lokalen Start/Abschluss oder verlor seine getrennten "
        "Cache-/Aktivitaets-/Zeitzaehler.");

    session.force_full_cpu_recompute_once();
    const auto before_empty_stats = session.statistics();
    katana::analysis::detail::GuardedNativeEntryShapeCache
        empty_shapes{image};
    const auto empty = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            std::span<const katana::sh4::DisassemblyLine>{},
            boundaries,
            {},
            {},
            empty_shapes,
            session);
    const auto after_empty_stats = session.statistics();
    require(
        empty.summaries.empty() && empty.resolutions.empty() &&
            after_empty_stats.incremental_epochs_started ==
                before_empty_stats.incremental_epochs_started &&
            after_empty_stats.analysis_epochs_published ==
                before_empty_stats.analysis_epochs_published &&
            after_empty_stats.analysis_epochs_discarded ==
                before_empty_stats.analysis_epochs_discarded &&
            after_empty_stats.full_cpu_recompute_fallbacks ==
                before_empty_stats.full_cpu_recompute_fallbacks,
        "Eine leere FVA-Anfrage oeffnete eine falsche Epoch oder "
        "verbrauchte den ausstehenden konservativen CPU-Fallback.");
    katana::analysis::detail::GuardedNativeEntryShapeCache
        forced_shapes{image};
    const auto forced =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                {},
                forced_shapes,
                session);
    const auto forced_stats = session.statistics();
    require_same_function_value_semantics(first, forced, false);
    require(
        forced.full_cpu_recompute_fallbacks == 1u &&
            forced.resolution_root_artifacts_reused.empty() &&
            !forced.resolution_root_artifacts_recomputed.empty() &&
            forced_stats.full_cpu_recompute_fallbacks == 1u &&
            forced_stats.analysis_epochs_published == 3u &&
            forced_stats.incremental_epochs_started == 3u &&
            forced_stats.analysis_epochs_discarded == 0u,
        "Der nicht darstellbare inkrementelle Zustand fiel nicht exakt "
        "einmal auf die semantisch identische CPU-Vollauswertung zurueck.");

    katana::analysis::ResolvedControlFlowEdge local_edge;
    local_edge.instruction_address = 4u;
    local_edge.target_address = 0x08u;
    local_edge.kind = katana::analysis::ResolvedControlFlowKind::Jump;
    local_edge.evidence =
        katana::analysis::ControlFlowEvidence::ProvenComplete;
    katana::analysis::detail::FunctionProgramDelta local_delta;
    local_delta.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Exact;
    local_delta.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::TerminalFull;
    local_delta.expected_published_epoch_version =
        session.published_epoch_version();
    local_delta.image_identity = image.analysis_instance_identity();
    local_delta.image_revision = image.analysis_revision();
    local_delta.changed_semantic_edge_sites.push_back(
        {local_edge.instruction_address,
         {local_edge}});
    session.stage_next_function_program_delta(std::move(local_delta));
    katana::analysis::detail::GuardedNativeEntryShapeCache
        extended_shapes{image};
    const auto extended =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                std::span<const katana::sh4::DisassemblyLine>{},
                std::span<const katana::analysis::FunctionBoundary>{},
                std::span<const katana::analysis::ResolvedControlFlowEdge>{},
                {},
                extended_shapes,
                session);
    const auto extended_stats = session.statistics();
    const std::array semantic_edge_snapshot{local_edge};
    const auto extended_cold =
        katana::analysis::analyze_function_values(
            image, lines, boundaries, semantic_edge_snapshot);
    require_same_function_value_semantics(
        extended_cold, extended, false);
    require(
        !extended.budget_exhausted &&
            extended_stats.program_graph_builds == 2u &&
            extended_stats.program_graph_functions_reused >
                forced_stats.program_graph_functions_reused &&
            extended_stats.program_graph_functions_built >
                forced_stats.program_graph_functions_built &&
            extended_stats.summary_state_reuses >
                forced_stats.summary_state_reuses &&
            extended_stats.caller_scc_invalidations != 0u &&
            extended_stats.caller_scc_invalidations <
                extended.summaries.size() &&
            extended_stats.analysis_epochs_published == 4u &&
            extended_stats.incremental_epochs_started == 4u &&
            extended_stats.analysis_epochs_discarded == 0u,
        "Eine lokale Funktionserweiterung baute unveraenderte immutable "
        "Shards neu oder invalidierte ausserhalb ihres beweisbaren "
        "Caller-SCC-Closure.");

    const auto public_progress_before =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        public_progress_events;
    const auto public_progress = katana::analysis::analyze_function_values(
        image,
        lines,
        boundaries,
        {},
        [&public_progress_events](
            const katana::analysis::FunctionValueAnalysisProgress& progress) {
            public_progress_events.push_back(progress);
        });
    const auto public_progress_after =
        katana::analysis::detail::
            function_value_progress_runtime_statistics_for_testing();
    require(
        !public_progress_events.empty() &&
            public_progress_events.back().phase == "complete" &&
            public_progress_events.back().program_graph_builds == 1u &&
            public_progress_events.back().program_graph_reuses == 0u &&
            public_progress_events.back().abi_contract_epoch_reuses == 0u &&
            public_progress_events.back().summary_state_reuses == 0u &&
            public_progress_events.back().analysis_epochs_published == 1u,
        "Ein frischer oeffentlicher Function-Value-Aufruf erbte eine "
        "Session-Epoch oder meldete keinen vollstaendigen kalten "
        "Programmgraphaufbau.");
    require_same_function_value_semantics(first, public_progress);
    require(
        public_progress_after.callback_activations >
                public_progress_before.callback_activations &&
            public_progress_after.pulse_threads_started >
                public_progress_before.pulse_threads_started &&
            public_progress_after.detailed_cache_sessions_started ==
                public_progress_before.detailed_cache_sessions_started,
        "Ein einfacher oeffentlicher FVA-Progresscallback aktivierte "
        "implizit die teure Detailed-Cachehistorie oder blieb unwirksam.");

    katana::analysis::detail::GuardedNativeEntryShapeCache
        throwing_shapes{image};
    const auto throwing_progress =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                [](const auto&) {
                    throw std::runtime_error(
                        "synthetic-progress-observer-failure");
                },
                throwing_shapes,
                session);
    require_same_function_value_semantics(first, throwing_progress, false);
    require(
        throwing_progress.progress_callback_failed,
        "Ein werfender Function-Value-Beobachter veraenderte die Analyse "
        "oder blieb in der Verlusttelemetrie unsichtbar.");

    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        diagnostic_progress;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        diagnostic_shapes{image};
    // This fixture verifies the lower FunctionEvaluation cache itself. Root
    // artifact reuse would correctly bypass that layer altogether, so force
    // the explicit fail-closed CPU path for this one diagnostic epoch.
    session.force_full_cpu_recompute_once();
    set_stack_diagnostics_for_serial_fixpoint(true);
    const auto diagnostic =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                [&diagnostic_progress](const auto& progress) {
                    diagnostic_progress.push_back(progress);
                },
                diagnostic_shapes,
                session);
    set_stack_diagnostics_for_serial_fixpoint(false);
    require_same_function_value_semantics(first, diagnostic, false);
    require(
        !diagnostic_progress.empty() &&
            diagnostic_progress.back().phase == "complete" &&
            diagnostic_progress.back()
                    .cache_diagnostic_bypass_evaluations != 0u &&
            diagnostic_progress.back().physical_evaluations ==
                diagnostic_progress.back()
                    .cache_diagnostic_bypass_evaluations +
                    diagnostic_progress.back()
                        .cache_replay_fallback_recomputes +
                    diagnostic_progress.back().session_cache_misses,
        "Der opt-in Diagnose-Cachebypass verletzte die exakte "
        "Physical=Miss+ReplayFallback+DiagnosticBypass-Bilanz.");

    std::vector<std::uint8_t> inventory_bytes(0x48u, 0x09u);
    const auto put_inventory_u16 =
        [&inventory_bytes](const std::size_t offset,
                           const std::uint16_t value) {
            inventory_bytes[offset] =
                static_cast<std::uint8_t>(value);
            inventory_bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
        };
    const auto put_inventory_u32 =
        [&inventory_bytes](const std::size_t offset,
                           const std::uint32_t value) {
            inventory_bytes[offset] =
                static_cast<std::uint8_t>(value);
            inventory_bytes[offset + 1u] =
                static_cast<std::uint8_t>(value >> 8u);
            inventory_bytes[offset + 2u] =
                static_cast<std::uint8_t>(value >> 16u);
            inventory_bytes[offset + 3u] =
                static_cast<std::uint8_t>(value >> 24u);
        };
    put_inventory_u16(0x00u, 0xD405u); // mov.l @(0x18,pc),r4
    put_inventory_u16(0x02u, 0xB003u); // bsr 0x0c
    put_inventory_u16(0x04u, 0x0009u);
    put_inventory_u16(0x06u, 0x000Bu);
    put_inventory_u16(0x08u, 0x0009u);
    put_inventory_u16(0x0Cu, 0xE220u); // mov #0x20,r2
    put_inventory_u16(0x0Eu, 0x2242u); // mov.l r4,@r2
    put_inventory_u16(0x10u, 0x000Bu);
    put_inventory_u16(0x12u, 0x0009u);
    put_inventory_u32(0x18u, 0x40u);
    put_inventory_u16(0x40u, 0x000Bu);
    put_inventory_u16(0x42u, 0x0009u);
    put_inventory_u16(0x44u, 0x000Bu);
    put_inventory_u16(0x46u, 0x0009u);

    katana::io::ExecutableImage inventory_image;
    inventory_image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    inventory_image.add_segment({
        ".persistent-session-inventory",
        0u,
        0u,
        inventory_bytes.size(),
        katana::io::SegmentKind::Mixed,
        {true, true, true},
        inventory_bytes});
    inventory_image.add_entry_point(0u);
    const auto inventory_lines =
        katana::sh4::disassemble(inventory_bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u>
        inventory_boundaries{{
            {0u, 0u},
            {0x0Cu, 0u},
            {0x40u, 0u},
            {0x44u, 0u}}};
    katana::analysis::detail::FunctionValueAnalysisSession
        inventory_session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        cold_inventory_shapes{inventory_image};
    const auto cold_inventory =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                inventory_image,
                inventory_lines,
                inventory_boundaries,
                {},
                {},
                cold_inventory_shapes,
                inventory_session);
    require(
        !cold_inventory.guarded_code_inventory
             .stored_code_addresses.empty(),
        "Der Session-Replay-Test erzeugte im Kaltlauf kein "
        "Guarded-AOT-Inventar und koennte dessen Verlust daher nicht "
        "erkennen.");

    katana::analysis::detail::GuardedNativeEntryShapeCache
        warm_inventory_shapes{inventory_image};
    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        warm_inventory_progress;
    // Bypass the higher-level immutable root artifact so the full CPU round
    // must exercise its exact replay path.
    inventory_session.force_full_cpu_recompute_once();
    const auto warm_inventory =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                inventory_image,
                inventory_lines,
                inventory_boundaries,
                {},
                [&warm_inventory_progress](const auto& progress) {
                    warm_inventory_progress.push_back(progress);
                },
                warm_inventory_shapes,
                inventory_session);
    require_same_function_value_semantics(
        cold_inventory, warm_inventory, false);
    require(
        !warm_inventory.guarded_code_inventory
                 .stored_code_addresses.empty() &&
            !warm_inventory_progress.empty() &&
            warm_inventory_progress.back().phase == "complete" &&
            warm_inventory_progress.back().cache_replays != 0u &&
            warm_inventory_progress.back().cache_replay_nanoseconds != 0u &&
            warm_inventory_progress.back()
                    .maximum_cache_replay_nanoseconds != 0u &&
            warm_inventory_progress.back().active_cache_replays == 0u,
        "Ein echter Function-Value-Warmtreffer spielte das nichtleere "
        "Guarded-AOT-Inventar nicht exakt wieder ein oder verlor seine "
        "Replay-Zeittelemetrie.");

    inventory_image.write_u32_le(0x18u, 0x44u);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        mutated_inventory_shapes{inventory_image};
    const auto mutated_inventory =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                inventory_image,
                inventory_lines,
                inventory_boundaries,
                {},
                {},
                mutated_inventory_shapes,
                inventory_session);
    const auto contains_inventory_target =
        [](const auto& analysis, const std::uint32_t target) {
            return std::any_of(
                analysis.guarded_code_inventory
                    .stored_code_addresses.begin(),
                analysis.guarded_code_inventory
                    .stored_code_addresses.end(),
                [target](const auto& candidate) {
                    return candidate.target_address == target;
                });
        };
    require(
        contains_inventory_target(mutated_inventory, 0x44u) &&
            !contains_inventory_target(mutated_inventory, 0x40u) &&
            inventory_session.statistics().misses != 0u,
        "Eine In-place-Mutation desselben ExecutableImage wurde nicht "
        "als neue Session-Generation gebunden oder lieferte ein altes "
        "Literal-Inventar.");

    auto shape_mutation_image = inventory_image;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        revision_bound_shapes{shape_mutation_image};
    require(
        revision_bound_shapes.classify(0x44u) ==
            katana::analysis::detail::
                GuardedNativeEntryShapeStatus::Valid,
        "Der Shape-Cache erkannte den gueltigen Ausgangseinstieg nicht.");
    shape_mutation_image.write_u32_le(0x44u, 0xFFFFFFFFu);
    require(
        revision_bound_shapes.classify(0x44u) ==
            katana::analysis::detail::
                GuardedNativeEntryShapeStatus::StructurallyInvalid,
        "Der Shape-Cache verwendete nach einer Image-Mutation eine "
        "veraltete gueltige Klassifikation.");

    katana::analysis::detail::FunctionValueAnalysisSession
        concurrently_reused_session;
    std::barrier concurrent_start{3};
    auto concurrent_plain = std::async(
        std::launch::async,
        [&] {
            concurrent_start.arrive_and_wait();
            katana::analysis::detail::
                GuardedNativeEntryShapeCache shapes{image};
            return katana::analysis::detail::
                analyze_function_values_with_guarded_entry_cache(
                    image,
                    lines,
                    boundaries,
                    {},
                    {},
                    shapes,
                    concurrently_reused_session);
        });
    auto concurrent_inventory = std::async(
        std::launch::async,
        [&] {
            concurrent_start.arrive_and_wait();
            // Deliberately construct against the other image: the analysis
            // entrypoint must rebind this retained cache before classifying.
            katana::analysis::detail::
                GuardedNativeEntryShapeCache shapes{image};
            return katana::analysis::detail::
                analyze_function_values_with_guarded_entry_cache(
                    inventory_image,
                    inventory_lines,
                    inventory_boundaries,
                    {},
                    {},
                    shapes,
                    concurrently_reused_session);
        });
    concurrent_start.arrive_and_wait();
    const auto concurrent_plain_result =
        concurrent_plain.get();
    const auto concurrent_inventory_result =
        concurrent_inventory.get();
    require_same_function_value_semantics(
        first, concurrent_plain_result);
    require_same_function_value_semantics(
        mutated_inventory, concurrent_inventory_result);

    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        expanded_boundaries{{
            {0u, 0u},
            {0x10u, 0u},
            {0x20u, 0u}}};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        expanded_shapes{image};
    const auto expanded_cached =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                expanded_boundaries,
                {},
                {},
                expanded_shapes,
                session);
    const auto expanded_stats = session.statistics();
    require(
        expanded_stats.program_graph_functions_reused >
                extended_stats.program_graph_functions_reused &&
            expanded_stats.program_graph_functions_built >
                extended_stats.program_graph_functions_built,
        "Eine neue unabhaengige Funktion verwarf unveraenderte "
        "Programmgraph-Shards oder erzeugte keinen eigenen Neubau.");
    katana::analysis::detail::FunctionValueAnalysisSession
        expanded_fresh_session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        expanded_fresh_shapes{image};
    const auto expanded_fresh =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                expanded_boundaries,
                {},
                {},
                expanded_fresh_shapes,
                expanded_fresh_session);
    require_same_function_value_semantics(
        expanded_cached, expanded_fresh, false);

    constexpr std::array<katana::analysis::ResolvedControlFlowEdge, 1u>
        changed_edges{{{
            0x04u,
            0x10u,
            katana::analysis::ResolvedControlFlowKind::Jump,
            true,
            katana::analysis::ControlFlowEvidence::GuardedPartial,
            {},
            false}}};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        changed_shapes{image};
    const auto changed_cached =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                changed_edges,
                {},
                changed_shapes,
                session);
    const auto changed_stats = session.statistics();
    require(
        changed_stats.misses > expanded_stats.misses,
        "Eine geaenderte CFG-/Callee-Abhaengigkeit wurde faelschlich als "
        "vollstaendiger Session-Hit behandelt.");

    katana::analysis::detail::FunctionValueAnalysisSession
        fresh_session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        fresh_shapes{image};
    const auto changed_fresh =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                changed_edges,
                {},
                fresh_shapes,
                fresh_session);
    require_same_function_value_semantics(
        changed_cached, changed_fresh, false);

    katana::analysis::detail::FunctionValueAnalysisSession
        tiny_session{1u, 1u};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        tiny_shapes{image};
    const auto tiny =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                {},
                tiny_shapes,
                tiny_session);
    require_same_function_value_semantics(first, tiny);

    katana::analysis::detail::FunctionValueAnalysisSession
        evicting_session{1u, 1'024u * 1'024u * 1'024u};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        evicting_shapes{image};
    const auto evicting =
        katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                lines,
                boundaries,
                {},
                {},
                evicting_shapes,
                evicting_session);
    require_same_function_value_semantics(first, evicting);
    require(
        evicting_session.statistics().evictions != 0u,
        "Ein Eintrag grosser Session-Cache uebte den deterministischen "
        "LRU-Eviction-Pfad nicht aus.");
}

void verify_incremental_resolution_root_reuse() {
    std::vector<std::uint8_t> bytes(0x60u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x412Bu); // jmp @r1
    put_u16(0x02u, 0x0009u); // delay nop
    put_u16(0x20u, 0x000Bu); // independent target
    put_u16(0x22u, 0x0009u);
    put_u16(0x40u, 0x422Bu); // disconnected jmp @r2 root
    put_u16(0x42u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({
        ".incremental-resolution-roots",
        0u,
        0u,
        bytes.size(),
        katana::io::SegmentKind::Code,
        {true, false, true},
        bytes});
    image.add_entry_point(0x00u);
    image.add_entry_point(0x40u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x04u},
            {0x20u, 0x04u},
            {0x40u, 0x04u},
        }};

    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        cold_shapes{image};
    const auto cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            cold_shapes,
            session);
    require(
        cold.resolution_root_artifacts_reused.empty() &&
            cold.resolution_root_artifacts_recomputed ==
                std::vector<std::uint32_t>({0x00u, 0x40u}),
        "Der kalte Root-Artefaktlauf meldete keine exakte kanonische "
        "Ausgangsmenge.");

    constexpr std::array<katana::analysis::ResolvedControlFlowEdge, 1u>
        changed_edges{{{
            0x00u,
            0x20u,
            katana::analysis::ResolvedControlFlowKind::Jump,
            false,
            katana::analysis::ControlFlowEvidence::ProvenComplete,
            {},
            false}}};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        incremental_shapes{image};
    const auto incremental = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            changed_edges,
            {},
            incremental_shapes,
            session);

    katana::analysis::detail::FunctionValueAnalysisSession
        reference_session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        reference_shapes{image};
    const auto reference = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            changed_edges,
            {},
            reference_shapes,
            reference_session);
    require_same_function_value_semantics(
        incremental, reference, false);

    const auto contains = [](const auto& values,
                             const std::uint32_t value) {
        return std::find(values.begin(), values.end(), value) !=
               values.end();
    };
    require(
        contains(incremental.resolution_root_artifacts_reused, 0x40u) &&
            !contains(incremental.resolution_root_artifacts_reused, 0x00u) &&
            contains(
                incremental.resolution_root_artifacts_recomputed, 0x00u) &&
            !contains(
                incremental.resolution_root_artifacts_recomputed, 0x40u) &&
            contains(incremental.incremental_dirty_functions, 0x00u) &&
            !contains(incremental.incremental_dirty_functions, 0x40u) &&
            incremental.full_cpu_recompute_fallbacks == 0u &&
            incremental.resolution_root_artifacts_reused.size() +
                    incremental.resolution_root_artifacts_recomputed.size() ==
                2u,
        "Eine lokale Candidate-Kante invalidierte den getrennten Root "
        "oder der inkrementelle Lauf wich vom frischen CPU-Referenzlauf ab.");

    using Session =
        katana::analysis::detail::FunctionValueAnalysisSession;
    Session exact_root_budget_session{
        16'384u,
        1'024u * 1'024u * 1'024u,
        false,
        {},
        Session::default_maximum_resolution_dependency_nodes,
        2u,
        Session::default_maximum_resolution_epoch_retained_bytes};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        exact_cold_shapes{image};
    const auto exact_cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            exact_cold_shapes,
            exact_root_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        exact_warm_shapes{image};
    std::vector<katana::analysis::FunctionValueAnalysisProgress>
        exact_warm_progress;
    const auto exact_warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            [&](const auto& progress) {
                exact_warm_progress.push_back(progress);
            },
            exact_warm_shapes,
            exact_root_budget_session);
    require_same_function_value_semantics(
        exact_cold, exact_warm, false);
    require(
        exact_warm.resolution_root_artifacts_reused.size() == 2u &&
            exact_warm.resolution_root_artifacts_recomputed.empty() &&
            exact_warm.resolution_root_artifacts_retained == 2u &&
            exact_warm.resolution_epoch_retained_bytes != 0u &&
            exact_warm.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::None &&
            !exact_warm_progress.empty() &&
            exact_warm_progress.back()
                    .resolution_root_artifacts_retained == 2u &&
            exact_warm_progress.back()
                    .resolution_epoch_retained_bytes ==
                exact_warm.resolution_epoch_retained_bytes &&
            exact_warm_progress.back()
                    .resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::None &&
            exact_root_budget_session.statistics()
                    .resolution_root_artifacts_retained == 2u &&
            exact_root_budget_session.statistics()
                    .resolution_epoch_retained_bytes ==
                exact_warm.resolution_epoch_retained_bytes &&
            exact_root_budget_session.statistics()
                    .resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::None,
        "Das exakt ausgeschoepfte Root-Retention-Limit verlor Artefakte.");

    // The canonical admission budget must include both reused roots and roots
    // created in this epoch. Keeping the reused prefix after the new suffix
    // overflows would publish a history-dependent partial cache.
    auto initial_bytes = bytes;
    initial_bytes[0x40u] = 0x0Bu; // rts: present, but not a root yet
    initial_bytes[0x41u] = 0x00u;
    const auto initial_lines =
        katana::sh4::disassemble(initial_bytes, 0u);
    Session mixed_reuse_new_budget_session{
        16'384u,
        1'024u * 1'024u * 1'024u,
        false,
        {},
        Session::default_maximum_resolution_dependency_nodes,
        1u,
        Session::default_maximum_resolution_epoch_retained_bytes};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        mixed_initial_shapes{image};
    const auto mixed_initial = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            initial_lines,
            boundaries,
            {},
            {},
            mixed_initial_shapes,
            mixed_reuse_new_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        mixed_overflow_shapes{image};
    const auto mixed_overflow = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            mixed_overflow_shapes,
            mixed_reuse_new_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        mixed_after_overflow_shapes{image};
    const auto mixed_after_overflow = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            mixed_after_overflow_shapes,
            mixed_reuse_new_budget_session);
    require_same_function_value_semantics(
        mixed_overflow, mixed_after_overflow, false);
    require_same_function_value_semantics(
        mixed_overflow, exact_cold, false);
    require_same_function_value_semantics(
        mixed_after_overflow, exact_cold, false);
    require(
        mixed_initial.resolution_root_artifacts_retained == 1u &&
            mixed_initial.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::None &&
            mixed_overflow.resolution_root_artifacts_reused ==
                std::vector<std::uint32_t>{0x00u} &&
            mixed_overflow.resolution_root_artifacts_recomputed ==
                std::vector<std::uint32_t>{0x40u} &&
            mixed_overflow.resolution_root_artifacts_retained == 0u &&
            mixed_overflow.resolution_epoch_retained_bytes == 0u &&
            mixed_overflow.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    RootEntryLimit &&
            mixed_after_overflow.resolution_root_artifacts_reused.empty() &&
            mixed_after_overflow.resolution_root_artifacts_recomputed ==
                std::vector<std::uint32_t>({0x00u, 0x40u}) &&
            mixed_after_overflow.resolution_root_artifacts_retained == 0u &&
            mixed_after_overflow.resolution_epoch_retained_bytes == 0u &&
            mixed_after_overflow.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    RootEntryLimit,
        "Reused und neue Root-Artefakte umgingen gemeinsam das Limit oder "
        "hinterliessen nach dem Overflow eine partielle Retention. "
        "initial_retained=" +
            std::to_string(
                mixed_initial.resolution_root_artifacts_retained) +
            ", overflow_reused=" +
            std::to_string(
                mixed_overflow.resolution_root_artifacts_reused.size()) +
            ", overflow_recomputed=" +
            std::to_string(
                mixed_overflow.resolution_root_artifacts_recomputed.size()) +
            ", overflow_retained=" +
            std::to_string(
                mixed_overflow.resolution_root_artifacts_retained) +
            ", overflow_bytes=" +
            std::to_string(
                mixed_overflow.resolution_epoch_retained_bytes) +
            ", overflow_reason=" +
            std::string{katana::analysis::
                resolution_retention_limit_reason_name(
                    mixed_overflow
                        .resolution_retention_limit_reason)} +
            ", after_reused=" +
            std::to_string(
                mixed_after_overflow
                    .resolution_root_artifacts_reused.size()) +
            ", after_recomputed=" +
            std::to_string(
                mixed_after_overflow
                    .resolution_root_artifacts_recomputed.size()) +
            ", after_reason=" +
            std::string{katana::analysis::
                resolution_retention_limit_reason_name(
                    mixed_after_overflow
                        .resolution_retention_limit_reason)});

    Session overflowing_root_budget_session{
        16'384u,
        1'024u * 1'024u * 1'024u,
        false,
        {},
        Session::default_maximum_resolution_dependency_nodes,
        1u,
        Session::default_maximum_resolution_epoch_retained_bytes};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        overflow_cold_shapes{image};
    const auto overflow_cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            overflow_cold_shapes,
            overflowing_root_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        overflow_warm_shapes{image};
    const auto overflow_warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            overflow_warm_shapes,
            overflowing_root_budget_session);
    require_same_function_value_semantics(
        overflow_cold, overflow_warm, false);
    require(
        overflow_warm.resolution_root_artifacts_reused.empty() &&
            overflow_warm.resolution_root_artifacts_recomputed.size() ==
                2u &&
            overflow_warm.resolution_root_artifacts_retained == 0u &&
            overflow_warm.resolution_epoch_retained_bytes == 0u &&
            overflow_warm.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    RootEntryLimit &&
            overflow_warm.full_cpu_recompute_fallbacks == 0u,
        "Ein Root ueber dem Retention-Limit wurde teilweise behalten oder "
        "faelschlich als Analysefallback gemeldet.");

    Session overflowing_byte_budget_session{
        16'384u,
        1'024u * 1'024u * 1'024u,
        false,
        {},
        Session::default_maximum_resolution_dependency_nodes,
        Session::default_maximum_resolution_root_artifacts,
        1u};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        byte_cold_shapes{image};
    const auto byte_cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            byte_cold_shapes,
            overflowing_byte_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        byte_warm_shapes{image};
    const auto byte_warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            byte_warm_shapes,
            overflowing_byte_budget_session);
    require_same_function_value_semantics(
        byte_cold, byte_warm, false);
    require(
        byte_warm.resolution_root_artifacts_reused.empty() &&
            byte_warm.resolution_root_artifacts_recomputed.size() == 2u &&
            byte_warm.resolution_root_artifacts_retained == 0u &&
            byte_warm.resolution_epoch_retained_bytes == 0u &&
            byte_warm.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    ByteLimit &&
            byte_warm.full_cpu_recompute_fallbacks == 0u,
        "Ein Byte-Retention-Ueberlauf beeinflusste Analyse oder behielt "
        "unbudgetierte Root-Artefakte.");

    const auto presentationless_epoch =
        overflowing_byte_budget_session.published_epoch_version();
    katana::analysis::detail::FunctionProgramDelta terminal_delta;
    terminal_delta.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Unchanged;
    terminal_delta.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::TerminalFull;
    terminal_delta.expected_published_epoch_version =
        presentationless_epoch;
    terminal_delta.image_identity = image.analysis_instance_identity();
    terminal_delta.image_revision = image.analysis_revision();
    overflowing_byte_budget_session.stage_next_function_program_delta(
        std::move(terminal_delta));
    std::string recomputed_terminal_phase;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        recomputed_terminal_shapes{image};
    const auto recomputed_terminal = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            std::span<const katana::sh4::DisassemblyLine>{},
            std::span<const katana::analysis::FunctionBoundary>{},
            std::span<const katana::analysis::ResolvedControlFlowEdge>{},
            [&](const auto& progress) {
                recomputed_terminal_phase = progress.phase;
            },
            recomputed_terminal_shapes,
            overflowing_byte_budget_session);
    require_same_function_value_semantics(
        recomputed_terminal, exact_cold, false);
    require(
        recomputed_terminal_phase != "terminal-materialized" &&
            recomputed_terminal.result_materialization ==
                katana::analysis::FunctionValueResultMaterialization::
                    TerminalFull &&
            recomputed_terminal.resolution_root_artifacts_reused.empty() &&
            recomputed_terminal.resolution_root_artifacts_recomputed.size() ==
                2u &&
            recomputed_terminal.resolution_root_artifacts_retained == 0u &&
            recomputed_terminal.resolution_epoch_retained_bytes == 0u &&
            recomputed_terminal.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::ByteLimit &&
            overflowing_byte_budget_session.published_epoch_version() ==
                presentationless_epoch + 1u,
        "Ein Unchanged/TerminalFull benutzte nach verworfener "
        "Praesentationsretention den leeren Fast-Materializer statt den "
        "unveraenderten ProgramGraph exakt neu auszuwerten.");

    Session overflowing_dependency_budget_session{
        16'384u,
        1'024u * 1'024u * 1'024u,
        false,
        {},
        0u,
        Session::default_maximum_resolution_root_artifacts,
        Session::default_maximum_resolution_epoch_retained_bytes};
    katana::analysis::detail::GuardedNativeEntryShapeCache
        dependency_cold_shapes{image};
    const auto dependency_cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            dependency_cold_shapes,
            overflowing_dependency_budget_session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        dependency_warm_shapes{image};
    const auto dependency_warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            {},
            dependency_warm_shapes,
            overflowing_dependency_budget_session);
    require_same_function_value_semantics(
        dependency_cold, dependency_warm, false);
    require(
        dependency_warm.resolution_root_artifacts_reused.empty() &&
            dependency_warm.resolution_root_artifacts_recomputed.size() ==
                2u &&
            dependency_warm.resolution_root_artifacts_retained == 0u &&
            dependency_warm.resolution_epoch_retained_bytes == 0u &&
            dependency_warm.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    DependencyNodeLimit &&
            overflowing_dependency_budget_session.statistics()
                    .resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    DependencyNodeLimit,
        "Das Dependency-Node-Limit blieb untypisiert oder behielt einen "
        "partiellen Resolution-Cache.");

    if (katana::analysis::global_analysis_executor().maximum_jobs() >= 2u) {
        std::atomic_size_t resolution_jobs_started = 0u;
        std::atomic_size_t resolution_jobs_completed = 0u;
        std::atomic_bool release_remaining_jobs = false;
        Session unwind_session;
        katana::analysis::detail::ResolutionExecutionObserverForTesting
            unwind_observer;
        unwind_observer.job_started =
            [&](const std::size_t index) {
                resolution_jobs_started.fetch_add(
                    1u, std::memory_order_release);
                if (index == 0u) {
                    while (resolution_jobs_started.load(
                               std::memory_order_acquire) < 2u)
                        std::this_thread::yield();
                    return;
                }
                while (!release_remaining_jobs.load(
                    std::memory_order_acquire))
                    std::this_thread::yield();
            };
        unwind_observer.job_completed =
            [&](const std::size_t) {
                resolution_jobs_completed.fetch_add(
                    1u, std::memory_order_release);
            };
        unwind_observer.commit =
            [&](const std::size_t index) {
                if (index != 0u) return;
                release_remaining_jobs.store(
                    true, std::memory_order_release);
                throw std::runtime_error(
                    "synthetic-resolution-commit");
            };
        unwind_session.set_resolution_execution_observer_for_testing(
            std::move(unwind_observer));
        katana::analysis::detail::GuardedNativeEntryShapeCache
            unwind_shapes{image};
        auto unwind_error =
            std::make_shared<std::exception_ptr>();
        std::promise<void> unwind_retired_promise;
        auto unwind_retired =
            unwind_retired_promise.get_future();
        katana::analysis::AnalysisWorkDescriptor unwind_work;
        unwind_work.phase =
            katana::analysis::AnalysisWorkPhase::Resolution;
        unwind_work.subject_kind =
            katana::analysis::AnalysisWorkSubjectKind::Root;
        unwind_work.subject = 0u;
        unwind_work.priority =
            katana::analysis::AnalysisWorkPriorityKind::CriticalPrefix;
        unwind_work.critical_prefix = 0u;
        katana::analysis::global_analysis_executor().submit(
            std::move(unwind_work),
            [&, unwind_error]() noexcept {
                try {
                    static_cast<void>(katana::analysis::detail::
                        analyze_function_values_with_guarded_entry_cache(
                            image,
                            lines,
                            boundaries,
                            {},
                            {},
                            unwind_shapes,
                            unwind_session));
                } catch (...) {
                    *unwind_error = std::current_exception();
                }
                return katana::analysis::
                    AnalysisWorkDisposition::Complete;
            },
            [&, unwind_error](
                const std::exception_ptr scheduler_error) noexcept {
                if (scheduler_error && !*unwind_error)
                    *unwind_error = scheduler_error;
                unwind_retired_promise.set_value();
            });
        unwind_retired.get();
        bool commit_error_propagated = false;
        try {
            if (*unwind_error)
                std::rethrow_exception(*unwind_error);
        } catch (const std::runtime_error& error) {
            commit_error_propagated =
                std::string_view{error.what()} ==
                "synthetic-resolution-commit";
        }
        require(
            commit_error_propagated &&
                resolution_jobs_started.load(
                    std::memory_order_acquire) >= 2u &&
                resolution_jobs_completed.load(
                    std::memory_order_acquire) ==
                    resolution_jobs_started.load(
                        std::memory_order_acquire),
            "Eine kanonische Resolution-Commit-Ausnahme wurde vor dem "
            "Drain aller gestarteten Sliding-Window-Completion-Callbacks "
            "weitergereicht.");
    }
}

void verify_inventory_region_dependency_reuse() {
    std::vector<std::uint8_t> bytes(0x90u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x412Bu); // candidate-only tail -> 0x20
    put_u16(0x02u, 0x0009u);
    put_u16(0x20u, 0xB00Eu); // direct bsr -> helper 0x40
    put_u16(0x22u, 0x0009u);
    put_u16(0x24u, 0x422Bu); // regional self-tail -> 0x20
    put_u16(0x26u, 0x0009u);
    put_u16(0x40u, 0x432Bu); // helper resolution root
    put_u16(0x42u, 0x0009u);
    put_u16(0x60u, 0x000Bu);
    put_u16(0x62u, 0x0009u);
    put_u16(0x70u, 0x442Bu); // disconnected resolution root
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({
        ".inventory-region-dependency-reuse",
        0u,
        0u,
        bytes.size(),
        katana::io::SegmentKind::Code,
        {true, false, true},
        bytes});
    image.add_entry_point(0x00u);
    image.add_entry_point(0x70u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 5u>
        boundaries{{
            {0x00u, 0x04u},
            {0x20u, 0x08u},
            {0x40u, 0x04u},
            {0x60u, 0x04u},
            {0x70u, 0x04u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u>
        inventory_edges{{
            {0x00u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
            {0x24u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};

    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        cold_shapes{image};
    const auto cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            inventory_edges,
            {},
            cold_shapes,
            session);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        warm_shapes{image};
    const auto warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            inventory_edges,
            {},
            warm_shapes,
            session);
    require_same_function_value_semantics(cold, warm, false);
    require(
        !cold.resolution_root_artifacts_recomputed.empty() &&
            warm.resolution_root_artifacts_recomputed.empty() &&
            warm.resolution_root_artifacts_reused ==
                cold.resolution_root_artifacts_recomputed,
        "Ein unveraenderter InventoryRegion-SCC wurde nicht exakt als "
        "Root-Artefakt wiederverwendet.");

    std::vector<katana::analysis::ResolvedControlFlowEdge> changed_edges{
        inventory_edges.begin(), inventory_edges.end()};
    changed_edges.push_back({
        0x40u,
        0x60u,
        katana::analysis::ResolvedControlFlowKind::Jump,
        false,
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        {},
        false});
    katana::analysis::detail::GuardedNativeEntryShapeCache
        incremental_shapes{image};
    const auto incremental = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            changed_edges,
            {},
            incremental_shapes,
            session);
    katana::analysis::detail::FunctionValueAnalysisSession
        fresh_session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        fresh_shapes{image};
    const auto fresh = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            changed_edges,
            {},
            fresh_shapes,
            fresh_session);
    require_same_function_value_semantics(incremental, fresh, false);
    const auto contains = [](const auto& values,
                             const std::uint32_t address) {
        return std::find(values.begin(), values.end(), address) !=
               values.end();
    };
    require(
        contains(incremental.resolution_root_artifacts_reused, 0x70u) &&
            contains(
                incremental.resolution_root_artifacts_recomputed,
                0x00u) &&
            contains(
                incremental.resolution_root_artifacts_recomputed,
                0x20u) &&
            contains(
                incremental.resolution_root_artifacts_recomputed,
                0x40u) &&
            !contains(
                incremental.resolution_root_artifacts_recomputed,
                0x70u) &&
            incremental.full_cpu_recompute_fallbacks == 0u,
        "Eine Helper-Aenderung hinter einem zyklischen, gleichadressigen "
        "Function/InventoryRegion-Knoten invalidierte nicht den gesamten "
        "Root-Strang oder den getrennten Root gleich mit.");
}

void verify_typed_interfunction_tail_transport() {
    std::vector<std::uint8_t> bytes(0xD0u, 0x09u);
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

    // Root A covers an ordinary statically known Function tail.
    put_u16(0x00u, 0xD403u); // callback literal 0x10 -> r4
    put_u16(0x02u, 0xA025u); // bra 0x50 (Function tail)
    put_u16(0x04u, 0x0009u);
    put_u32(0x10u, 0xB0u);

    // Root B reaches 0x40 through a guarded candidate-only JMP. r4 carries
    // independent ABI code-pointer evidence; the value whose transport is
    // under test lives only in r8. The ordinary Function 0x40 is a
    // one-instruction fallthrough which does not read r8; the same-address
    // InventoryRegion follows its private region edge into 0x42 and stores r8
    // at a proven fixed destination. Any address-only ingress/output
    // projection therefore loses the B8 callback without weakening the
    // unknown-object-store false-positive contract.
    put_u16(0x20u, 0xD404u); // carrier literal 0x34 -> r4
    put_u16(0x22u, 0xD805u); // tested callback literal 0x38 -> r8
    put_u16(0x24u, 0x412Bu); // jmp @r1 -> 0x40 (InventoryRegion tail)
    put_u16(0x26u, 0x0009u);
    put_u32(0x34u, 0xB0u);
    put_u32(0x38u, 0xB8u);

    put_u16(0x40u, 0x0009u); // ordinary Function ends; Region falls through
    put_u16(0x42u, 0xD502u); // fixed destination literal 0x4C -> r5
    put_u16(0x44u, 0x2542u); // carrier makes the ABI sink exact
    put_u16(0x46u, 0x2582u); // Region-only persistent r8 callback store
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u32(0x4Cu, 0xC8u);
    put_u16(0x50u, 0x2742u); // ordinary Function-tail r4 store
    put_u16(0x52u, 0x000Bu);
    put_u16(0x54u, 0x0009u);

    // Root C uses an ordinary call. It reaches the sink only if the ABI
    // backwards slice follows Region 0x90 -> Region 0xA0 before admitting the
    // forwarded call into Function 0x80.
    put_u16(0x60u, 0xD403u); // callback literal 0x70 -> r4
    put_u16(0x62u, 0xB00Du); // bsr 0x80
    put_u16(0x64u, 0x0009u);
    put_u16(0x66u, 0x000Bu);
    put_u16(0x68u, 0x0009u);
    put_u32(0x70u, 0xC0u);
    put_u16(0x80u, 0xA006u); // bra 0x90 (Region tail)
    put_u16(0x82u, 0x0009u);
    put_u16(0x90u, 0xA006u); // Region 0x90 -> Region 0xA0
    put_u16(0x92u, 0x0009u);
    put_u16(0xA0u, 0x2742u); // regional persistent r4 callback store
    put_u16(0xA2u, 0x000Bu);
    put_u16(0xA4u, 0x0009u);

    put_u16(0xB0u, 0x000Bu);
    put_u16(0xB2u, 0x0009u);
    put_u16(0xB8u, 0x000Bu);
    put_u16(0xBAu, 0x0009u);
    put_u16(0xC0u, 0x000Bu);
    put_u16(0xC2u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".typed-interfunction-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0x00u);
    image.add_entry_point(0x20u);
    image.add_entry_point(0x60u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 11u>
        boundaries{{
            {0x00u, 0x06u},
            {0x20u, 0x08u},
            {0x40u, 0x02u},
            {0x42u, 0x0Au},
            {0x50u, 0x06u},
            {0x60u, 0x0Au},
            {0x80u, 0x04u},
            {0xA0u, 0x06u},
            {0xB0u, 0x04u},
            {0xB8u, 0x04u},
            {0xC0u, 0x04u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u>
        edges{{
            {0x24u,
             0x40u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};

    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache cold_shapes{image};
    const auto cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            edges,
            {},
            cold_shapes,
            session);
    katana::analysis::detail::GuardedNativeEntryShapeCache warm_shapes{image};
    const auto warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            edges,
            {},
            warm_shapes,
            session);
    require_same_function_value_semantics(cold, warm, false);
    const auto contains_stored = [](const auto& result,
                                    const std::uint32_t target) {
        return std::any_of(
            result.guarded_code_inventory.stored_code_addresses.begin(),
            result.guarded_code_inventory.stored_code_addresses.end(),
            [target](const auto& candidate) {
                return candidate.target_address == target;
            });
    };
    const auto contains_root = [](const auto& roots,
                                  const std::uint32_t address) {
        return std::find(roots.begin(), roots.end(), address) != roots.end();
    };
    require(
        contains_stored(cold, 0xB0u) &&
            contains_stored(cold, 0xB8u) &&
            contains_stored(cold, 0xC0u) &&
            contains_stored(warm, 0xB0u) &&
            contains_stored(warm, 0xB8u) &&
            contains_stored(warm, 0xC0u) &&
            !cold.guarded_code_inventory.walk_diagnostics.truncated() &&
            !warm.guarded_code_inventory.walk_diagnostics.truncated() &&
            contains_root(warm.resolution_root_artifacts_reused, 0x00u) &&
            contains_root(warm.resolution_root_artifacts_reused, 0x20u) &&
            contains_root(warm.resolution_root_artifacts_reused, 0x60u) &&
            warm.resolution_root_artifacts_recomputed.empty(),
        "Der typisierte Tail-Transport verlor den statischen Function-Tail, "
        "den gleichadressigen InventoryRegion-Tail, die regionale ABI-"
        "Rueckwaertsprojektion oder deren exakte Cold/Warm-Root-"
        "Wiederverwendung (cold_b0=" +
            std::to_string(contains_stored(cold, 0xB0u)) +
            ", cold_b8=" +
            std::to_string(contains_stored(cold, 0xB8u)) +
            ", cold_c0=" +
            std::to_string(contains_stored(cold, 0xC0u)) +
            ", warm_b0=" +
            std::to_string(contains_stored(warm, 0xB0u)) +
            ", warm_b8=" +
            std::to_string(contains_stored(warm, 0xB8u)) +
            ", warm_c0=" +
            std::to_string(contains_stored(warm, 0xC0u)) +
            ", cold_truncated=" +
            std::to_string(
                cold.guarded_code_inventory.walk_diagnostics.truncated()) +
            ", warm_truncated=" +
            std::to_string(
                warm.guarded_code_inventory.walk_diagnostics.truncated()) +
            ", reused_00=" +
            std::to_string(contains_root(
                warm.resolution_root_artifacts_reused, 0x00u)) +
            ", reused_20=" +
            std::to_string(contains_root(
                warm.resolution_root_artifacts_reused, 0x20u)) +
            ", reused_60=" +
            std::to_string(contains_root(
                warm.resolution_root_artifacts_reused, 0x60u)) +
            ", recomputed=" +
            std::to_string(
                warm.resolution_root_artifacts_recomputed.size()) +
            ").");
}

void verify_unresolved_tail_target_is_fail_closed() {
    constexpr std::size_t region_budget = 1'024u;
    constexpr std::size_t target_count = region_budget + 1u;
    constexpr std::uint32_t target_base = 0x100u;
    std::vector<std::uint8_t> bytes(
        target_base + target_count * 4u,
        0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x412Bu); // one candidate-only tail site
    put_u16(0x02u, 0x0009u);
    std::vector<katana::analysis::ResolvedControlFlowEdge> edges;
    edges.reserve(target_count);
    for (std::size_t index = 0u; index < target_count; ++index) {
        const auto target = target_base +
                            static_cast<std::uint32_t>(index * 4u);
        put_u16(target, 0x000Bu);
        put_u16(target + 2u, 0x0009u);
        edges.push_back(
            {0x00u,
             target,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true});
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".unresolved-tail-target",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u>
        boundaries{{{0x00u, 0x04u}}};
    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{image};
    const auto result = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            edges,
            {},
            shapes,
            session);
    const auto& diagnostics =
        result.guarded_code_inventory.walk_diagnostics;
    require(
        !result.budget_exhausted &&
            diagnostics.inventory_region_count == region_budget &&
            diagnostics.pending_inventory_region_count == 1u &&
            diagnostics.inventory_tail_target_unresolved &&
            diagnostics.truncated(),
        "Ein Tail-Ziel jenseits des typisierten Regionbudgets blieb im "
        "finalen FunctionValueAnalysisResult nicht fail-closed sichtbar.");
}

void verify_function_evaluation_cache_telemetry() {
    const auto probe =
        katana::analysis::detail::
            probe_function_evaluation_cache_telemetry_for_testing();
    const auto& statistics = probe.statistics;
    require(
        statistics.balanced() &&
            statistics.lookups ==
                statistics.ready_hits +
                    statistics.in_flight_coalesces +
                    statistics.misses &&
            statistics.hits ==
                statistics.ready_hits +
                    statistics.in_flight_coalesces &&
            probe.physical_computations == statistics.misses,
        "Das Function-Evaluation-Cache-Ledger verletzte seine exakte "
        "Lookup-/Miss-/Physical-Compute-Bilanz.");
    require(
        statistics.ready_hits != 0u &&
            statistics.in_flight_coalesces != 0u &&
            statistics.evictions != 0u &&
            probe.in_flight_waits ==
                statistics.in_flight_coalesces &&
            probe.in_flight_wait_nanoseconds != 0u &&
            probe.maximum_in_flight_wait_nanoseconds != 0u &&
            probe.maximum_in_flight_wait_nanoseconds <=
                probe.in_flight_wait_nanoseconds,
        "Der retailfreie Cache-Probe uebte ReadyHit, "
        "InFlight-Coalescing samt Wait-Zeit oder Eviction nicht aus.");
    for (std::size_t index = 0u;
         index < statistics.miss_reasons.size();
         ++index) {
        require(
            statistics.miss_reasons[index] != 0u,
            "Der retailfreie Cache-Probe liess einen primaeren "
            "Missgrund ungetestet (Index " +
                std::to_string(index) + ").");
    }
    using katana::analysis::detail::FunctionEvaluationCacheLookupOutcome;
    using katana::analysis::detail::FunctionEvaluationCacheMissReason;
    const std::array expected_context_reasons{
        FunctionEvaluationCacheMissReason::Cold,
        FunctionEvaluationCacheMissReason::ProjectedIngressChanged,
        FunctionEvaluationCacheMissReason::SummaryDependencyChanged,
        FunctionEvaluationCacheMissReason::AbiContractChanged,
    };
    std::size_t contextual_decision = 0u;
    bool final_oversize_observed = false;
    for (const auto& decision : probe.decisions) {
        require(
            decision.outcome ==
                    FunctionEvaluationCacheLookupOutcome::Miss &&
                decision.miss_reason.has_value(),
            "Der Cache-Observer erhielt keinen final klassifizierten Miss.");
        if (decision.function_entry == 700u) {
            require(
                contextual_decision < expected_context_reasons.size() &&
                    decision.miss_reason ==
                        expected_context_reasons[contextual_decision],
                "Die bounded Multi-Context-Historie erklaerte einen "
                "alternierenden Kontext gegen die falsche Baseline.");
            ++contextual_decision;
        }
        if (decision.function_entry == 900u &&
            decision.miss_reason ==
                FunctionEvaluationCacheMissReason::
                    OversizeOrNoExactReplay)
            final_oversize_observed = true;
    }
    require(
        contextual_decision == expected_context_reasons.size() &&
            final_oversize_observed &&
            probe.observer_statistics.balanced() &&
            probe.observer_statistics.lookups ==
                probe.decisions.size() &&
            probe.observer_statistics.misses ==
                probe.decisions.size() &&
            probe.observer_statistics.miss_reasons[
                static_cast<std::size_t>(
                    FunctionEvaluationCacheMissReason::Cold)] == 1u &&
            probe.observer_statistics.miss_reasons[
                static_cast<std::size_t>(
                    FunctionEvaluationCacheMissReason::
                        ProjectedIngressChanged)] == 1u &&
            probe.observer_statistics.miss_reasons[
                static_cast<std::size_t>(
                    FunctionEvaluationCacheMissReason::
                        SummaryDependencyChanged)] == 1u &&
            probe.observer_statistics.miss_reasons[
                static_cast<std::size_t>(
                    FunctionEvaluationCacheMissReason::
                        AbiContractChanged)] == 1u &&
            probe.observer_statistics.miss_reasons[
                static_cast<std::size_t>(
                    FunctionEvaluationCacheMissReason::
                        OversizeOrNoExactReplay)] == 1u,
        "Observer-/Missgrund-Evidence verlor entweder den genauen "
        "Multi-Context-Pfad, die Aggregatbilanz oder die finale "
        "Oversize-Reklassifizierung.");
    require(
        probe.throwing_observer_semantics_preserved,
        "Eine Observer-Exception veraenderte Cache-Semantik oder Reuse.");
    require(
        probe.bounded_context_history_limit == 64u &&
            probe.bounded_context_history_entries ==
                probe.bounded_context_history_limit,
        "Die kausale Multi-Context-Historie ueberschritt ihr "
        "funktionales Retentionslimit.");
    require(
        probe.bounded_absent_history_entries != 0u &&
            probe.bounded_absent_history_entries < 5'000u &&
            probe.bounded_absent_history_accounted_bytes <=
                probe.bounded_absent_history_byte_limit &&
            probe.bounded_absent_history_accounted_bytes ==
                probe.bounded_absent_history_byte_limit,
        "Die exakte Eviction-/Oversize-Historie ist nicht durch ein "
        "hartes Bytebudget begrenzt.");
    require(
        !probe.bounded_exact_replay_available &&
            probe.unbounded_exact_replay_available &&
            probe.unbounded_exact_replay_preserved &&
            probe.coordinator_requests == 2u &&
            probe.coordinator_producers == 1u &&
            probe.coordinator_ready_reuses +
                    probe.coordinator_in_flight_reuses ==
                1u &&
            probe.coordinator_entries == 1u &&
            probe.coordinator_retained_payload_bytes != 0u &&
            probe.coordinator_evictions == 0u &&
            probe.coordinator_session_lookups == 1u &&
            probe.coordinator_session_entries == 0u &&
            probe.coordinator_physical_computations == 1u &&
            probe.coordinator_collision_safe &&
            probe.coordinator_failure_pinned &&
            probe.coordinator_ready_eviction_recomputed &&
            probe.coordinator_in_flight_eviction_safe,
        "Der Multi-Root-Coordinator verlor bei verweigerter Session-"
        "Admission sein exaktes Ergebnis, verletzte das Ready-Bytebudget "
        "oder evictete laufende Single-Flight-Arbeit.");
    require(
        probe.inline_only_artifact_bytes ==
                probe.inline_only_artifact_owner_bytes &&
            probe.controlled_artifact_bytes >
                probe.inline_only_artifact_bytes &&
            probe.controlled_entry_retained_payload_bytes >
                probe.controlled_artifact_bytes &&
            probe.exact_limit_entries == 1u &&
            probe.exact_limit_retained_payload_bytes ==
                probe.controlled_entry_retained_payload_bytes &&
            probe.one_byte_short_entries == 0u &&
            probe.one_byte_short_retained_payload_bytes == 0u,
        "Das Function-Evaluation-Payloadbudget zaehlte Inline-Owner "
        "doppelt oder verletzte seine deklarierte Admission-Grenze.");
}

katana::io::ExecutableImage image_with_callee(const std::vector<std::uint8_t>& callee) {
    std::vector<std::uint8_t> bytes(128u, 0x09u);
    const std::vector<std::uint8_t> main{
        0x0Eu,
        0xB0u, // bsr 0x20
        0x09u,
        0x00u, // nop (delay)
        0x2Bu,
        0x40u, // jmp @r0
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(main.begin(), main.end(), bytes.begin());
    std::copy(callee.begin(), callee.end(), bytes.begin() + 0x20u);
    bytes[0x10u] = 0x0Bu; // jsr @r2 (unchanged candidate site)
    bytes[0x11u] = 0x42u;
    bytes[0x12u] = 0x09u;
    bytes[0x13u] = 0x00u;
    bytes[0x14u] = 0x0Bu;
    bytes[0x15u] = 0x00u;
    bytes[0x16u] = 0x09u;
    bytes[0x17u] = 0x00u;
    bytes[0x14u] = 0x0Bu;
    bytes[0x15u] = 0x00u;
    bytes[0x16u] = 0x09u;
    bytes[0x17u] = 0x00u;
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
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

katana::io::ExecutableImage classification_image(std::vector<std::uint8_t> bytes) {
    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
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

const katana::analysis::IndirectControlFlowResolution*
site(const katana::analysis::ControlFlowAnalysisResult& analysis, const std::uint32_t address) {
    const auto found = std::find_if(
        analysis.indirect_control_flow.begin(),
        analysis.indirect_control_flow.end(),
        [address](const auto& candidate) { return candidate.instruction_address == address; });
    return found == analysis.indirect_control_flow.end() ? nullptr : &*found;
}

const katana::analysis::FunctionRegisterValueSummary*
summary(const katana::analysis::ControlFlowAnalysisResult& analysis,
        const std::uint32_t function,
        const std::uint8_t reg) {
    const auto owner = std::find_if(
        analysis.function_value_summaries.begin(),
        analysis.function_value_summaries.end(),
        [function](const auto& candidate) { return candidate.function_address == function; });
    if (owner == analysis.function_value_summaries.end()) return nullptr;
    const auto value =
        std::find_if(owner->registers.begin(),
                     owner->registers.end(),
                     [reg](const auto& candidate) { return candidate.register_index == reg; });
    return value == owner->registers.end() ? nullptr : &*value;
}

katana::io::ExecutableImage returned_table_load_image(
    const std::vector<std::uint16_t>& setup_opcodes,
    const std::uint16_t load_opcode,
    const std::vector<std::uint32_t>& returned_addresses,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& table_slots) {
    require(!returned_addresses.empty() && returned_addresses.size() <= 8u,
            "Returned-Table-Testfixture erhielt keine begrenzte Rueckgabemenge.");
    std::vector<std::uint8_t> bytes(0x800u, 0x09u);
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
    const auto put_return = [&](const std::size_t address,
                                const std::uint32_t value) {
        require(value <= 0x7Fu,
                "Returned-Table-Testfixture braucht positive MOV-Immediate-Werte.");
        put_u16(address,
                static_cast<std::uint16_t>(0xE000u | value)); // mov #value,r0
        put_u16(address + 2u, 0x000Bu);                       // rts
        put_u16(address + 4u, 0x0009u);                       // nop (delay)
    };

    put_u16(0x00u, 0xB00Eu); // bsr 0x20
    put_u16(0x02u, 0x0009u); // nop (delay)
    auto cursor = 0x04u;
    for (const auto opcode : setup_opcodes) {
        put_u16(cursor, opcode);
        cursor += 2u;
    }
    require(cursor + 6u <= 0x20u,
            "Returned-Table-Testfixture ueberlappt den Accessor.");
    put_u16(cursor, load_opcode);
    put_u16(cursor + 2u, 0x000Bu); // rts
    put_u16(cursor + 4u, 0x0009u); // nop (delay)

    const auto branch_count = returned_addresses.size() - 1u;
    const auto default_return = 0x20u + branch_count * 2u;
    for (std::size_t index = 0u; index < branch_count; ++index) {
        const auto branch_address = 0x20u + index * 2u;
        const auto target_address = default_return + (index + 1u) * 6u;
        const auto displacement =
            (target_address - (branch_address + 4u)) / 2u;
        require(displacement <= 0x7Fu,
                "Returned-Table-Testfixture ueberschritt den BT-Bereich.");
        put_u16(branch_address,
                static_cast<std::uint16_t>(0x8900u | displacement));
        put_return(target_address, returned_addresses[index]);
    }
    put_return(default_return, returned_addresses.back());

    for (const auto [address, value] : table_slots) {
        require(address <= bytes.size() - 4u,
                "Returned-Table-Testslot liegt ausserhalb des Images.");
        put_u32(address, value);
    }
    std::set<std::uint32_t> handlers;
    for (const auto& [address, value] : table_slots) {
        static_cast<void>(address);
        if (value >= 0xC0u && value <= bytes.size() - 4u)
            handlers.insert(value);
    }
    for (const auto handler : handlers) {
        put_u16(handler, 0x000Bu);
        put_u16(handler + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".returned-table-load",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       std::move(bytes),
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-returned-table-load"});
    image.add_entry_point(0u);
    return image;
}

katana::analysis::FunctionValueAnalysisResult
returned_table_values(const katana::io::ExecutableImage& image) {
    const auto lines =
        katana::sh4::disassemble(image.segments().front().bytes, 0u);
    constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x20u};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

const katana::analysis::ReturnedCodeAddressTableCandidate*
returned_table_candidate(
    const katana::analysis::FunctionValueAnalysisResult& analysis,
    const std::uint32_t table_address) {
    const auto found = std::find_if(
        analysis.guarded_code_inventory.returned_code_address_tables.begin(),
        analysis.guarded_code_inventory.returned_code_address_tables.end(),
        [table_address](const auto& candidate) {
            return candidate.table_address == table_address;
        });
    return found == analysis.guarded_code_inventory.returned_code_address_tables.end()
               ? nullptr
               : &*found;
}

katana::analysis::FunctionValueAnalysisResult
incomplete_return_family_values() {
    std::vector<std::uint8_t> bytes(0x60u, 0x09u);
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
    put_u16(0x00u, 0xD105u); // mov.l @(0x18,pc),r1 -> known accessor 0x20
    put_u16(0x02u, 0x410Bu); // jsr @r1 (incomplete family)
    put_u16(0x04u, 0x0009u); // nop (delay)
    put_u16(0x06u, 0x6803u); // mov r0,r8 (preserve semantic provenance)
    put_u16(0x08u, 0x6C03u); // mov r0,r12
    put_u16(0x0Au, 0x63C2u); // mov.l @r12,r3
    put_u16(0x0Cu, 0x430Bu); // jsr @r3
    put_u16(0x0Eu, 0x0009u); // nop (delay)
    put_u16(0x10u, 0x000Bu); // rts
    put_u16(0x12u, 0x0009u); // nop (delay)
    put_u32(0x18u, 0x20u);
    put_u16(0x20u, 0xE040u); // known candidate returns non-stack table 0x40
    put_u16(0x22u, 0x000Bu);
    put_u16(0x24u, 0x0009u);
    put_u32(0x40u, 0x50u);
    put_u16(0x50u, 0x000Bu); // callback
    put_u16(0x52u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.add_segment({".incomplete-return-family",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-incomplete-return-family"});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x20u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x02u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true}, // private analysis-candidate carrier, never an executable CFG edge
    }};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries, edges);
}

katana::analysis::FunctionValueAnalysisResult
shifted_stack_alias_values(const bool isolated_harvest) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x7FD4u); // add #-44,r15
    put_u16(0x02u, 0x64F3u); // mov r15,r4
    put_u16(0x04u, 0xE560u); // mov #0x60,r5 (callback)
    put_u16(0x06u, 0xB01Bu); // bsr 0x40
    put_u16(0x08u, 0x0009u); // nop (delay)
    put_u16(0x0Au, 0x7F2Cu); // add #44,r15
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);
    put_u16(0x40u, 0x2452u); // mov.l r5,@r4
    put_u16(0x42u, 0x2F62u); // mov.l r6,@r15 (same rebased slot)
    put_u16(0x44u, 0x6542u); // mov.l @r4,r5
    put_u16(0x46u, 0xE220u); // mov #0x20,r2
    put_u16(0x48u, 0x2252u); // mov.l r5,@r2
    put_u16(0x4Au, 0x000Bu);
    put_u16(0x4Cu, 0x0009u);
    put_u16(0x60u, 0x000Bu);
    put_u16(0x62u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".shifted-stack-alias",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-shifted-stack-alias"});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    if (isolated_harvest) {
        image.add_entry_point(0x40u);
        constexpr std::array<std::uint32_t, 2u> function_entries{0u, 0x40u};
        return katana::analysis::analyze_function_values(
            image, lines, function_entries);
    }
    constexpr std::array<std::uint32_t, 1u> function_entries{0u};
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

katana::analysis::FunctionValueAnalysisResult
guarded_inventory_budget_values(
    const std::size_t candidate_count,
    const bool include_returned_method_table = false) {
    constexpr std::size_t record_size = 0x20u;
    constexpr std::uint32_t handler_base = 0x1'0000u;
    constexpr std::uint32_t method_caller = 0x8000u;
    constexpr std::uint32_t method_accessor = 0x8040u;
    constexpr std::uint32_t method_table = 0x9000u;
    const auto returned_handler =
        handler_base + static_cast<std::uint32_t>(candidate_count * 4u);
    std::vector<std::uint8_t> bytes(
        handler_base +
            (candidate_count + (include_returned_method_table ? 1u : 0u)) *
                4u,
        0x09u);
    std::vector<std::uint32_t> function_entries;
    function_entries.reserve(
        candidate_count + (include_returned_method_table ? 2u : 0u));
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
    for (std::size_t index = 0u; index < candidate_count; ++index) {
        const auto caller = index * record_size;
        const auto callback =
            handler_base + static_cast<std::uint32_t>(index * 4u);
        function_entries.push_back(static_cast<std::uint32_t>(caller));
        put_u16(caller + 0x00u, 0xD405u); // mov.l @(0x18,pc),r4
        put_u16(caller + 0x02u, 0xB003u); // bsr local registrar +0x0c
        put_u16(caller + 0x04u, 0x0009u);
        put_u16(caller + 0x06u, 0x000Bu);
        put_u16(caller + 0x08u, 0x0009u);
        put_u16(caller + 0x0Cu, 0xE220u); // mov #0x20,r2
        put_u16(caller + 0x0Eu, 0x2242u); // mov.l r4,@r2
        put_u16(caller + 0x10u, 0x000Bu);
        put_u16(caller + 0x12u, 0x0009u);
        put_u32(caller + 0x18u, callback);
        put_u16(callback, 0x000Bu);
        put_u16(callback + 2u, 0x0009u);
    }
    if (include_returned_method_table) {
        function_entries.push_back(method_caller);
        function_entries.push_back(method_accessor);
        put_u16(method_caller + 0x00u, 0xB01Eu); // bsr method_accessor
        put_u16(method_caller + 0x02u, 0x0009u);
        put_u16(method_caller + 0x04u, 0x6C03u); // mov r0,r12
        put_u16(method_caller + 0x06u, 0x63C2u); // mov.l @r12,r3
        put_u16(method_caller + 0x08u, 0x430Bu); // jsr @r3
        put_u16(method_caller + 0x0Au, 0x0009u);
        put_u16(method_caller + 0x0Cu, 0x000Bu);
        put_u16(method_caller + 0x0Eu, 0x0009u);
        put_u16(method_accessor + 0x00u, 0xD001u); // mov.l @(0x8048,pc),r0
        put_u16(method_accessor + 0x02u, 0x000Bu);
        put_u16(method_accessor + 0x04u, 0x0009u);
        put_u32(method_accessor + 0x08u, method_table);
        put_u32(method_table + 0x00u, returned_handler);
        put_u32(method_table + 0x04u, 1u); // bounded-table sentinel
        put_u16(returned_handler, 0x000Bu);
        put_u16(returned_handler + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    if (include_returned_method_table) {
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::
                EntryPointStraightLineQuiescent);
        image.set_initial_snapshot_entry(method_caller);
    }
    image.add_segment({".guarded-inventory-budget",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes,
                       katana::io::ImageSourceKind::DiscBootFile,
                       katana::io::ImageLoadPhase::Initial,
                       "synthetic-guarded-inventory-budget"});
    image.add_entry_point(0u);
    if (include_returned_method_table)
        image.add_entry_point(method_caller);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    return katana::analysis::analyze_function_values(
        image, lines, function_entries);
}

bool has_stored_code_address(
    const katana::analysis::FunctionValueAnalysisResult& analysis,
    const std::uint32_t target) {
    return std::any_of(
        analysis.guarded_code_inventory.stored_code_addresses.begin(),
        analysis.guarded_code_inventory.stored_code_addresses.end(),
        [target](const auto& candidate) {
            return candidate.target_address == target;
        });
}

katana::analysis::FunctionValueAnalysisResult
conditional_shared_tail_values() {
    std::vector<std::uint8_t> bytes(0x90u, 0x09u);
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
    put_u16(0x00u, 0xE570u); // mov #0x70,r5 (initial carrier evidence)
    put_u16(0x02u, 0xB00Du); // bsr 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);
    put_u16(0x20u, 0x412Bu); // jmp @r1 (candidate-only tail)
    put_u16(0x22u, 0x0009u);
    put_u16(0x40u, 0xE500u); // clear carrier evidence before owner split
    put_u16(0x42u, 0x890Du); // bt 0x60 (unknown condition)
    put_u16(0x44u, 0x000Bu); // internal path without a store
    put_u16(0x46u, 0x0009u);
    put_u16(0x60u, 0x0722u); // external shared tail: stc vbr,r7
    put_u16(0x62u, 0xD402u); // mov.l @(0x6C,pc),r4 -> callback 0x70
    put_u16(0x64u, 0x2742u); // mov.l r4,@r7
    put_u16(0x66u, 0x000Bu);
    put_u16(0x68u, 0x0009u);
    put_u32(0x6Cu, 0x70u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".conditional-shared-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x04u},
        {0x60u, 0x0Au},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x20u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
nonisolated_tail_cycle_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
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
    put_u16(0x00u, 0xD404u); // callback literal 0x14 -> r4
    put_u16(0x02u, 0xE900u); // live cycle counter r9 = 0
    put_u16(0x04u, 0x412Bu); // candidate-only tail -> 0x20
    put_u16(0x06u, 0x0009u);
    put_u32(0x14u, 0x70u);

    put_u16(0x20u, 0x7901u); // add #1,r9
    put_u16(0x22u, 0x2742u); // callback r4 -> persistent unknown
    put_u16(0x24u, 0x422Bu); // candidate-only self tail
    put_u16(0x26u, 0x0009u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".nonisolated-tail-cycle",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x08u},
        {0x20u, 0x08u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x04u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x24u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
dead_r3_live_r9_tail_contract_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
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
    put_u16(0x00u, 0xD404u); // callback literal 0x14 -> r4
    put_u16(0x02u, 0xE37Fu); // dead incoming junk -> r3
    put_u16(0x04u, 0x412Bu); // candidate-only tail -> 0x20
    put_u16(0x06u, 0x0009u);
    put_u32(0x14u, 0x70u);

    put_u16(0x20u, 0x6943u); // callback r4 -> callee-saved r9
    put_u16(0x22u, 0x422Bu); // candidate-only tail -> 0x40
    put_u16(0x24u, 0x0009u);

    put_u16(0x40u, 0xE300u); // overwrite r3 before its first read
    put_u16(0x42u, 0xE400u); // destroy the original ABI carrier
    put_u16(0x44u, 0x6493u); // live incoming r9 -> r4
    put_u16(0x46u, 0x2742u); // callback r4 -> persistent unknown
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".dead-r3-live-r9-tail-contract",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u> boundaries{{
        {0x00u, 0x08u},
        {0x20u, 0x06u},
        {0x40u, 0x0Cu},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x04u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x22u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
multi_owner_inventory_start_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0xE470u); // mov #0x70,r4
    put_u16(0x02u, 0xB00Du); // bsr 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);
    put_u16(0x20u, 0x412Bu); // jmp @r1, candidate-only 0x40
    put_u16(0x22u, 0x0009u);
    put_u16(0x30u, 0xA006u); // owner A -> shared 0x40
    put_u16(0x32u, 0x0009u);
    put_u16(0x38u, 0xA002u); // owner B -> shared 0x40
    put_u16(0x3Au, 0x0009u);
    put_u16(0x40u, 0x2742u); // mov.l r4,@r7
    put_u16(0x42u, 0x000Bu);
    put_u16(0x44u, 0x0009u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".multi-owner-inventory-start",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x04u},
        {0x30u, 0x16u},
        {0x38u, 0x0Eu},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x20u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

struct DuplicateForwardedContextRun final {
    katana::analysis::FunctionValueAnalysisResult values;
    katana::analysis::FunctionValueAnalysisProgress progress;
    katana::analysis::detail::FunctionValueAnalysisSessionStatistics
        session_statistics;
};

DuplicateForwardedContextRun
duplicate_forwarded_context_values(
    const std::size_t maximum_session_entries = 0u,
    const std::size_t maximum_session_bytes = 0u) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD40Bu); // owner A: callback literal 0x30 -> r4
    put_u16(0x02u, 0x412Bu); // candidate-only tail A -> 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x10u, 0xD407u); // owner B: same literal 0x30 -> r4
    put_u16(0x12u, 0x412Bu); // candidate-only tail B -> 0x20
    put_u16(0x14u, 0x0009u);
    put_u16(0x16u, 0x000Bu);
    put_u16(0x20u, 0x2742u); // persistent callback store
    put_u16(0x22u, 0x000Bu);
    put_u16(0x24u, 0x0009u);
    put_u32(0x30u, 0x70u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".duplicate-forwarded-context",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x08u},
            {0x10u, 0x08u},
            {0x20u, 0x06u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u>
        edges{{
            {0x02u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
            {0x12u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};
    katana::analysis::detail::FunctionValueAnalysisSession session(
        maximum_session_entries,
        maximum_session_bytes,
        false);
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{image};
    DuplicateForwardedContextRun run;
    run.values = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            edges,
            [&run](const auto& progress) {
                run.progress = progress;
            },
            shapes,
            session);
    run.session_statistics = session.statistics();
    return run;
}

struct ContextualStaleErrorRun final {
    katana::analysis::FunctionValueAnalysisResult values;
};

ContextualStaleErrorRun contextual_stale_error_values(
    katana::analysis::detail::FunctionValueAnalysisSession*
        session_for_testing = nullptr) {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // R has four distinct guarded candidate callsites. Their descendant F
    // lanes stay separate by the caller/callsite family key.
    put_u16(0x00u, 0xE460u); // object 0x60 -> r4
    put_u16(0x02u, 0x410Bu); // guarded candidate call -> F
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0xE464u); // object 0x64 -> r4
    put_u16(0x08u, 0x410Bu); // guarded candidate call -> F
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xE468u); // object 0x68 -> r4
    put_u16(0x0Eu, 0x410Bu); // guarded candidate call -> F
    put_u16(0x10u, 0x0009u);
    put_u16(0x12u, 0xE46Cu); // object 0x6c -> r4
    put_u16(0x14u, 0x410Bu); // guarded candidate call -> F
    put_u16(0x16u, 0x0009u);
    put_u16(0x18u, 0x000Bu);
    put_u16(0x1Au, 0x0009u);

    // F preserves its distinct object identity in r6 while r4 follows the
    // shared callback field. H reads r6, so its one joined lane widens.
    put_u16(0x20u, 0x6643u); // mov r4,r6
    put_u16(0x22u, 0x6442u); // mov.l @r4,r4
    put_u16(0x24u, 0xB004u); // bsr H (0x30)
    put_u16(0x26u, 0x0009u);
    put_u16(0x28u, 0x000Bu);
    put_u16(0x2Au, 0x0009u);

    put_u16(0x30u, 0x6063u); // H: mov r6,r0
    put_u16(0x32u, 0x2742u); // H: persistent callback store
    put_u16(0x34u, 0x6463u); // H: mov r6,r4 (retained ABI read)
    put_u16(0x36u, 0x2742u); // H: persistent object-identity store
    put_u16(0x38u, 0x000Bu);
    put_u16(0x3Au, 0x0009u);

    // Different object identities, one identical decode-valid callback.
    put_u32(0x60u, 0x90u);
    put_u32(0x64u, 0x90u);
    put_u32(0x68u, 0x90u);
    put_u32(0x6Cu, 0x90u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".contextual-stale-error",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x1Cu},
            {0x20u, 0x0Cu},
            {0x30u, 0x0Cu},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 4u> edges{{
        {0x02u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x08u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x0Eu,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x14u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    std::optional<katana::analysis::detail::FunctionValueAnalysisSession>
        owned_session;
    if (session_for_testing == nullptr)
        owned_session.emplace();
    auto& session = session_for_testing == nullptr
                        ? *owned_session
                        : *session_for_testing;
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{image};
    ContextualStaleErrorRun run;
    run.values = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image, lines, boundaries, edges, {}, shapes, session);
    return run;
}

void verify_contextual_stale_error_regression(
    const ContextualStaleErrorRun& reference) {
    using ContextualSession =
        katana::analysis::detail::FunctionValueAnalysisSession;
    using JacobiFaultEvent =
        katana::analysis::detail::ContextualReturnJacobiFaultEventForTesting;
    using JacobiFaultHook = katana::analysis::detail::
        ContextualReturnJacobiFaultHookForTesting;
    using JacobiFaultPoint = katana::analysis::detail::
        ContextualReturnJacobiFaultHookPointForTesting;
    constexpr std::string_view sentinel_error =
        "Testfehler vor Contextual-Return-Jacobi-Auswertung.";

    ContextualSession stale_error_session;
    std::atomic_size_t stale_error_injections = 0u;
    std::atomic_size_t stale_error_stale_discards = 0u;
    std::atomic_size_t stale_error_pre_freezes = 0u;
    std::atomic_size_t stale_error_snapshots = 0u;
    std::atomic_size_t stale_error_logical_admissions = 0u;
    std::atomic_size_t stale_error_semantic_lane_creations = 0u;
    std::atomic_size_t stale_error_semantic_lane_reuses = 0u;
    std::atomic_size_t stale_error_exact_subscriber_replays = 0u;
    std::atomic_bool stale_error_budget_exhausted = false;
    std::atomic_bool stale_error_invocation_aborted = false;
    stale_error_session
        .set_contextual_return_scheduler_diagnostics_observer_for_testing(
            [&stale_error_stale_discards,
             &stale_error_snapshots,
             &stale_error_logical_admissions,
             &stale_error_semantic_lane_creations,
             &stale_error_semantic_lane_reuses,
             &stale_error_exact_subscriber_replays,
             &stale_error_budget_exhausted,
             &stale_error_invocation_aborted](const auto& diagnostics) {
                stale_error_snapshots.fetch_add(
                    1u, std::memory_order_relaxed);
                stale_error_stale_discards.fetch_add(
                    diagnostics.stale_snapshot_discards,
                    std::memory_order_relaxed);
                stale_error_logical_admissions.fetch_add(
                    diagnostics.fixpoint_scheduler_logical_admissions,
                    std::memory_order_relaxed);
                stale_error_semantic_lane_creations.fetch_add(
                    diagnostics.semantic_lane_creations,
                    std::memory_order_relaxed);
                stale_error_semantic_lane_reuses.fetch_add(
                    diagnostics.semantic_lane_reuses,
                    std::memory_order_relaxed);
                stale_error_exact_subscriber_replays.fetch_add(
                    diagnostics.exact_subscriber_replays,
                    std::memory_order_relaxed);
                if (diagnostics.contextual_context_budget_exhausted ||
                    diagnostics.contextual_evaluation_budget_exhausted ||
                    diagnostics.composite_logical_budget_exhausted)
                    stale_error_budget_exhausted.store(
                        true, std::memory_order_relaxed);
                if (diagnostics.invocation_aborted_by_exception)
                    stale_error_invocation_aborted.store(
                        true, std::memory_order_relaxed);
            });
    JacobiFaultHook stale_error_hook;
    stale_error_hook.maximum_batch_size = 3u;
    stale_error_hook.callback = [&stale_error_injections,
                                 &stale_error_pre_freezes](
                                    const JacobiFaultEvent& event) {
        if (event.point == JacobiFaultPoint::BeforeStaleFreeze) {
            stale_error_pre_freezes.fetch_add(
                1u, std::memory_order_relaxed);
            return false;
        }
        if (event.point != JacobiFaultPoint::BeforeEvaluation ||
            event.function_address != 0x30u || event.batch_size != 3u ||
            event.batch_index != 2u)
            return false;
        std::size_t expected = 0u;
        return stale_error_injections.compare_exchange_strong(
            expected, 1u, std::memory_order_relaxed);
    };
    stale_error_session.set_contextual_return_jacobi_fault_hook_for_testing(
        std::move(stale_error_hook));
    ContextualStaleErrorRun stale_error_repaired;
    std::string stale_error_message;
    try {
        stale_error_repaired =
            contextual_stale_error_values(&stale_error_session);
    } catch (const std::exception& error) {
        stale_error_message = error.what();
    }
    const auto stale_error_candidate = std::find_if(
        stale_error_repaired.values.guarded_code_inventory
            .stored_code_addresses.begin(),
        stale_error_repaired.values.guarded_code_inventory
            .stored_code_addresses.end(),
        [](const auto& candidate) {
            return candidate.target_address == 0x90u;
        });
    require(
        stale_error_message.empty() &&
            stale_error_injections.load(std::memory_order_relaxed) == 1u &&
            stale_error_stale_discards.load(std::memory_order_relaxed) !=
                0u &&
            stale_error_pre_freezes.load(std::memory_order_relaxed) != 0u &&
            stale_error_snapshots.load(std::memory_order_relaxed) != 0u &&
            stale_error_semantic_lane_creations.load(
                std::memory_order_relaxed) ==
                stale_error_logical_admissions.load(
                    std::memory_order_relaxed) &&
            stale_error_semantic_lane_creations.load(
                std::memory_order_relaxed) != 0u &&
            stale_error_semantic_lane_reuses.load(
                std::memory_order_relaxed) != 0u &&
            stale_error_exact_subscriber_replays.load(
                std::memory_order_relaxed) >
                stale_error_logical_admissions.load(
                    std::memory_order_relaxed) &&
            !stale_error_budget_exhausted.load(
                std::memory_order_relaxed) &&
            !stale_error_invocation_aborted.load(
                std::memory_order_relaxed),
        "Der KR-4985-Test erzeugte keinen gesunden stale Contextual-"
        "Return-Jacobi-Workerfehler mit anschliessender Reparatur "
        "(error='" + stale_error_message + "', injections=" +
            std::to_string(
                stale_error_injections.load(std::memory_order_relaxed)) +
            ", stale_discards=" +
            std::to_string(
                stale_error_stale_discards.load(std::memory_order_relaxed)) +
            ", prefreezes=" +
            std::to_string(
                stale_error_pre_freezes.load(std::memory_order_relaxed)) +
            ", snapshots=" +
            std::to_string(
                stale_error_snapshots.load(std::memory_order_relaxed)) +
            ", logical_admissions=" +
            std::to_string(stale_error_logical_admissions.load(
                std::memory_order_relaxed)) +
            ", semantic_lane_creations=" +
            std::to_string(stale_error_semantic_lane_creations.load(
                std::memory_order_relaxed)) +
            ", semantic_lane_reuses=" +
            std::to_string(stale_error_semantic_lane_reuses.load(
                std::memory_order_relaxed)) +
            ", exact_subscriber_replays=" +
            std::to_string(stale_error_exact_subscriber_replays.load(
                std::memory_order_relaxed)) +
            ", budget=" +
            std::to_string(
                stale_error_budget_exhausted.load(
                    std::memory_order_relaxed)) +
            ", aborted=" +
            std::to_string(
                stale_error_invocation_aborted.load(
                    std::memory_order_relaxed)) + ").");
    require_same_function_value_semantics(
        reference.values, stale_error_repaired.values, false);
    const auto stale_error_evidence = [&] {
        if (stale_error_candidate ==
            stale_error_repaired.values.guarded_code_inventory
                .stored_code_addresses.end())
            return std::string{"<kein Kandidat>"};
        std::string text;
        for (const auto site : stale_error_candidate->evidence_call_sites) {
            if (!text.empty()) text += ',';
            text += std::to_string(site);
        }
        return text;
    }();
    require(
        stale_error_candidate !=
                stale_error_repaired.values.guarded_code_inventory
                    .stored_code_addresses.end() &&
            stale_error_candidate->store_instruction_addresses ==
                std::vector<std::uint32_t>{0x32u} &&
            stale_error_candidate->evidence_call_sites ==
                std::vector<std::uint32_t>{
                    0x02u, 0x08u, 0x0Eu, 0x14u, 0x24u},
        "Ein stale Contextual-Return-Fehler publizierte oder verlor "
        "Summary-/Evidence-Zustand vor seiner Reparatur "
        "(actual_call_sites=[" + stale_error_evidence + "]).");

    ContextualSession current_error_session;
    std::atomic_size_t current_error_injections = 0u;
    JacobiFaultHook current_error_hook;
    current_error_hook.maximum_batch_size = 1u;
    current_error_hook.callback = [&current_error_injections](
                                      const JacobiFaultEvent& event) {
        if (event.point != JacobiFaultPoint::BeforeEvaluation ||
            event.function_address != 0x30u || event.batch_size != 1u)
            return false;
        std::size_t expected = 0u;
        return current_error_injections.compare_exchange_strong(
            expected, 1u, std::memory_order_relaxed);
    };
    current_error_session
        .set_contextual_return_jacobi_fault_hook_for_testing(
            std::move(current_error_hook));
    std::string current_error_message;
    try {
        static_cast<void>(
            contextual_stale_error_values(&current_error_session));
    } catch (const std::exception& error) {
        current_error_message = error.what();
    }
    require(
        current_error_injections.load(std::memory_order_relaxed) == 1u &&
            current_error_message == sentinel_error,
        "Ein current Contextual-Return-Jacobi-Workerfehler wurde nicht "
        "mit der injizierten Sentinel-Meldung propagiert.");
}

struct DuplicateIsolatedContextRun final {
    katana::analysis::FunctionValueAnalysisResult values;
    katana::analysis::FunctionValueAnalysisProgress progress;
    katana::analysis::detail::FunctionValueAnalysisSessionStatistics
        session_statistics;
};

DuplicateIsolatedContextRun
duplicate_isolated_context_values(
    const std::size_t maximum_session_entries = 0u,
    const std::size_t maximum_session_bytes = 0u) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] =
            static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD40Bu); // owner A: callback literal 0x30 -> r4
    put_u16(0x02u, 0xB00Du); // bsr 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x10u, 0xD407u); // owner B: same literal 0x30 -> r4
    put_u16(0x12u, 0xB005u); // bsr 0x20
    put_u16(0x14u, 0x0009u);
    put_u16(0x16u, 0x000Bu);
    put_u16(0x20u, 0x2742u); // persistent callback store
    put_u16(0x22u, 0x000Bu);
    put_u16(0x24u, 0x0009u);
    put_u32(0x30u, 0x70u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".duplicate-isolated-context",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    image.add_entry_point(0x10u);
    // Independent callee entry => unknown ingress => both concrete callers
    // require initial Isolated Store Harvest.
    image.add_entry_point(0x20u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x08u},
            {0x10u, 0x08u},
            {0x20u, 0x06u},
        }};
    katana::analysis::detail::FunctionValueAnalysisSession session(
        maximum_session_entries,
        maximum_session_bytes,
        false);
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{image};
    DuplicateIsolatedContextRun run;
    run.values = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            [&run](const auto& progress) {
                run.progress = progress;
            },
            shapes,
            session);
    run.session_statistics = session.statistics();
    return run;
}

katana::analysis::FunctionValueAnalysisResult
parameterized_candidate_return_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
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
    put_u16(0x00u, 0xE460u); // mov #0x60,r4 (table)
    put_u16(0x02u, 0xB00Du); // bsr 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);
    put_u16(0x20u, 0x410Bu); // jsr @r1, candidate-only 0x40
    put_u16(0x22u, 0x0009u);
    put_u16(0x24u, 0x6C03u); // mov r0,r12
    put_u16(0x26u, 0x63C2u); // mov.l @r12,r3
    put_u16(0x28u, 0x000Bu);
    put_u16(0x2Au, 0x0009u);
    put_u16(0x40u, 0xB006u); // bsr 0x50 (normal direct helper)
    put_u16(0x42u, 0x0009u);
    put_u16(0x44u, 0x000Bu);
    put_u16(0x46u, 0x0009u);
    put_u16(0x50u, 0x6043u); // helper: mov r4,r0
    put_u16(0x52u, 0x000Bu);
    put_u16(0x54u, 0x0009u);
    put_u32(0x60u, 0x70u);
    put_u32(0x64u, 1u);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".parameterized-candidate-return",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x0Cu},
        {0x40u, 0x08u},
        {0x50u, 0x06u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x20u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
contextual_dereference_return_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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
    put_u16(0x00u, 0xE460u); // object pointer -> r4
    put_u16(0x02u, 0xB00Du); // bsr owner 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);

    put_u16(0x20u, 0x410Bu); // guarded candidate-only call -> 0x40
    put_u16(0x22u, 0x0009u);
    put_u16(0x24u, 0x6C03u); // owner consumes returned table
    put_u16(0x26u, 0x63C2u); // mov.l @r12,r3
    put_u16(0x28u, 0x000Bu);
    put_u16(0x2Au, 0x0009u);

    put_u16(0x40u, 0x6442u); // candidate: mov.l @r4,r4
    put_u16(0x42u, 0xB005u); // bsr helper 0x50
    put_u16(0x44u, 0x0009u);
    put_u16(0x46u, 0x000Bu);
    put_u16(0x48u, 0x0009u);

    put_u16(0x50u, 0x6042u); // helper: mov.l @r4,r0
    put_u16(0x52u, 0x000Bu);
    put_u16(0x54u, 0x0009u);

    put_u32(0x60u, 0x70u); // object field -> helper context
    put_u32(0x70u, 0x80u); // helper return -> callback table
    put_u32(0x80u, 0x90u); // table handler
    put_u32(0x84u, 1u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".contextual-dereference-return",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x0Cu},
        {0x40u, 0x0Au},
        {0x50u, 0x06u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x20u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries, edges);
}

struct MultiOwnerContextualResult {
    katana::analysis::FunctionValueAnalysisResult values;
    std::size_t resolution_roots = 0u;
    std::size_t function_count = 0u;
    katana::analysis::FunctionValueAnalysisProgress final_progress;
};

MultiOwnerContextualResult
multi_owner_contextual_return_values() {
    std::vector<std::uint8_t> bytes(0x180u, 0x09u);
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

    put_u16(0x00u, 0xE470u); // owner A object -> r4
    put_u16(0x02u, 0xB00Du); // bsr owner A 0x20
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0xE474u); // owner B object -> r4
    put_u16(0x08u, 0xB012u); // bsr owner B 0x30
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);

    const auto put_owner = [&](const std::size_t address) {
        put_u16(address + 0x00u, 0x410Bu); // candidate call -> 0x50
        put_u16(address + 0x02u, 0x0009u);
        put_u16(address + 0x04u, 0x6C03u); // mov r0,r12
        put_u16(address + 0x06u, 0x63C2u); // mov.l @r12,r3
        put_u16(address + 0x08u, 0x000Bu);
        put_u16(address + 0x0Au, 0x0009u);
    };
    put_owner(0x20u);
    put_owner(0x30u);

    put_u16(0x50u, 0x6442u); // candidate: mov.l @r4,r4
    put_u16(0x52u, 0xB005u); // bsr shared helper 0x60
    put_u16(0x54u, 0x0009u);
    put_u16(0x56u, 0x000Bu);
    put_u16(0x58u, 0x0009u);
    put_u16(0x60u, 0x6042u); // shared helper: mov.l @r4,r0
    put_u16(0x62u, 0x000Bu);
    put_u16(0x64u, 0x0009u);

    put_u32(0x70u, 0x80u);
    put_u32(0x74u, 0x88u);
    put_u32(0x80u, 0x90u);
    put_u32(0x88u, 0x98u);
    put_u32(0x90u, 0xC0u);
    put_u32(0x94u, 1u);
    put_u32(0x98u, 0xD0u);
    put_u32(0x9Cu, 1u);
    put_u16(0xC0u, 0x000Bu);
    put_u16(0xC2u, 0x0009u);
    put_u16(0xD0u, 0x000Bu);
    put_u16(0xD2u, 0x0009u);

    std::vector<katana::analysis::FunctionBoundary> boundaries{
        {0x00u, 0x10u},
        {0x20u, 0x0Cu},
        {0x30u, 0x0Cu},
        {0x50u, 0x0Au},
        {0x60u, 0x06u},
    };
    for (std::uint32_t address = 0x100u;
         address < 0x180u;
         address += 8u) {
        if (address == 0x100u) {
            put_u16(address, 0x2542u); // local mov.l r4,@r5 inventory probe
            put_u16(address + 2u, 0x000Bu);
            put_u16(address + 4u, 0x0009u);
            boundaries.push_back({address, 0x06u});
        } else {
            put_u16(address, 0x000Bu);
            put_u16(address + 2u, 0x0009u);
            boundaries.push_back({address, 0x04u});
        }
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".multi-owner-contextual-return",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x20u,
         0x50u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x30u,
         0x50u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    katana::analysis::FunctionValueAnalysisProgress final_progress;
    auto values = katana::analysis::analyze_function_values(
        image,
        lines,
        boundaries,
        edges,
        [&final_progress](const auto& progress) {
            final_progress = progress;
        });
    return {std::move(values),
            final_progress.resolution_functions_total,
            boundaries.size(),
             final_progress};
}

katana::analysis::FunctionValueAnalysisResult
contextual_candidate_input_overflow_values(const bool stack_argument) {
    const auto helper_address = stack_argument ? 0x90u : 0x80u;
    const auto callback_address = stack_argument ? 0xB0u : 0xA0u;
    const auto object_base = stack_argument ? 0x50u : 0x40u;
    std::vector<std::uint8_t> bytes(
        stack_argument ? 0xC0u : 0xB0u, 0x09u);
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

    std::vector<katana::analysis::ResolvedControlFlowEdge> edges;
    edges.reserve(9u);
    std::size_t cursor = 0u;
    for (std::uint32_t index = 0u; index < 9u; ++index) {
        const auto object_address = object_base + index * 4u;
        const auto value_register = stack_argument ? 0u : 4u;
        put_u16(cursor,
                static_cast<std::uint16_t>(
                    0xE000u | (value_register << 8u) | object_address));
        cursor += 2u;
        if (stack_argument) {
            put_u16(cursor, 0x2F02u); // mov.l r0,@r15
            cursor += 2u;
        }
        const auto call_site = static_cast<std::uint32_t>(cursor);
        put_u16(cursor, 0x410Bu); // jsr @r1
        put_u16(cursor + 2u, 0x0009u);
        cursor += 4u;
        edges.push_back(
            {call_site,
             helper_address,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true});
        put_u32(object_address, callback_address);
    }
    put_u16(cursor, 0x000Bu);
    put_u16(cursor + 2u, 0x0009u);
    const auto owner_size = static_cast<std::uint32_t>(cursor + 4u);

    auto helper_cursor = static_cast<std::size_t>(helper_address);
    if (stack_argument) {
        put_u16(helper_cursor, 0x64F2u); // mov.l @r15,r4
        helper_cursor += 2u;
    }
    put_u16(helper_cursor, 0x6142u); // mov.l @r4,r1
    put_u16(helper_cursor + 2u, 0x410Bu); // jsr @r1
    put_u16(helper_cursor + 4u, 0x0009u);
    put_u16(helper_cursor + 6u, 0xE000u); // complete return {0}
    put_u16(helper_cursor + 8u, 0x000Bu);
    put_u16(helper_cursor + 10u, 0x0009u);
    put_u16(callback_address, 0x000Bu);
    put_u16(callback_address + 2u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({stack_argument
                           ? ".contextual-stack-input-overflow"
                           : ".contextual-register-input-overflow",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    const std::array boundaries{
        katana::analysis::FunctionBoundary{0u, owner_size},
        katana::analysis::FunctionBoundary{
            helper_address,
            static_cast<std::uint32_t>(helper_cursor + 12u -
                                       helper_address)},
        katana::analysis::FunctionBoundary{callback_address, 4u},
    };
    katana::analysis::detail::FunctionValueAnalysisSession session;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        cold_shapes{image};
    const auto cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            edges,
            {},
            cold_shapes,
            session);
    const auto cold_epoch = session.published_epoch_version();
    katana::analysis::detail::FunctionProgramDelta terminal_delta;
    terminal_delta.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Unchanged;
    terminal_delta.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::TerminalFull;
    terminal_delta.expected_published_epoch_version = cold_epoch;
    terminal_delta.image_identity = image.analysis_instance_identity();
    terminal_delta.image_revision = image.analysis_revision();
    session.stage_next_function_program_delta(
        std::move(terminal_delta));
    bool used_terminal_materializer = false;
    katana::analysis::detail::GuardedNativeEntryShapeCache
        warm_shapes{image};
    const auto warm = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            std::span<const katana::sh4::DisassemblyLine>{},
            std::span<const katana::analysis::FunctionBoundary>{},
            std::span<const katana::analysis::ResolvedControlFlowEdge>{},
            [&](const auto& progress) {
                used_terminal_materializer =
                    used_terminal_materializer ||
                    progress.phase == "terminal-materialized";
            },
            warm_shapes,
            session);
    require_same_function_value_semantics(cold, warm, false);
    require(
        !used_terminal_materializer &&
            cold.guarded_code_inventory.walk_diagnostics.truncated() &&
            warm.guarded_code_inventory.walk_diagnostics.truncated() &&
            warm.resolution_root_artifacts_retained == 0u &&
            warm.resolution_epoch_retained_bytes == 0u &&
            warm.resolution_retention_limit_reason ==
                katana::analysis::ResolutionRetentionLimitReason::
                    IncompleteRoot &&
            session.published_epoch_version() == cold_epoch + 1u,
        "Ein root-spezifisch abgeschnittener Lauf publizierte eine "
        "partielle Presentation-Epoch oder verlor beim folgenden "
        "Unchanged/TerminalFull seine Truncation-Diagnose.");
    return warm;
}

katana::analysis::FunctionValueAnalysisResult
contextual_read_contract_and_fixpoint_budget_values() {
    std::vector<std::uint8_t> bytes(0x620u, 0x09u);
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
    const auto put_bsr = [&](const std::size_t site,
                             const std::size_t target) {
        const auto displacement =
            (static_cast<std::int64_t>(target) -
             static_cast<std::int64_t>(site + 4u)) /
            2;
        put_u16(site,
                static_cast<std::uint16_t>(
                    0xB000u |
                    (static_cast<std::uint16_t>(displacement) & 0x0FFFu)));
    };

    put_u16(0x000u, 0xE460u); // real contextual object -> r4
    put_u16(0x002u, 0xE564u); // irrelevant contextual value -> r5
    put_bsr(0x004u, 0x020u);
    put_u16(0x006u, 0x0009u);
    put_u16(0x008u, 0x000Bu);
    put_u16(0x00Au, 0x0009u);

    put_u16(0x020u, 0x410Bu); // guarded candidate-only call -> 0x100
    put_u16(0x022u, 0x0009u);
    put_u16(0x024u, 0x6C03u); // mov r0,r12
    put_u16(0x026u, 0x63C2u); // mov.l @r12,r3
    put_u16(0x028u, 0x000Bu);
    put_u16(0x02Au, 0x0009u);

    put_u32(0x060u, 0x080u);
    put_u32(0x080u, 0x090u);
    put_u32(0x084u, 1u);
    put_u16(0x090u, 0x000Bu);
    put_u16(0x092u, 0x0009u);

    put_bsr(0x100u, 0x300u);
    put_u16(0x102u, 0x0009u);
    put_u16(0x104u, 0x000Bu);
    put_u16(0x106u, 0x0009u);

    std::vector<katana::analysis::FunctionBoundary> boundaries;
    constexpr std::size_t chain_base = 0x300u;
    constexpr std::size_t chain_wrapper_count = 65u;
    boundaries.reserve(4u + chain_wrapper_count);
    boundaries.push_back({0x000u, 0x00Cu});
    boundaries.push_back({0x020u, 0x00Cu});
    boundaries.push_back({0x100u, 0x08u});
    for (std::size_t index = 0u; index < chain_wrapper_count;
         ++index) {
        const auto address = chain_base + index * 8u;
        put_bsr(address, address + 8u);
        put_u16(address + 2u, 0x0009u);
        put_u16(address + 4u, 0x000Bu);
        put_u16(address + 6u, 0x0009u);
        boundaries.push_back(
            {static_cast<std::uint32_t>(address), 0x08u});
    }
    constexpr std::size_t chain_leaf =
        chain_base + chain_wrapper_count * 8u;
    put_u16(chain_leaf, 0x6042u); // leaf: mov.l @r4,r0
    put_u16(chain_leaf + 2u, 0x000Bu);
    put_u16(chain_leaf + 4u, 0x0009u);
    boundaries.push_back(
        {static_cast<std::uint32_t>(chain_leaf), 0x06u});

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    image.set_initial_snapshot_entry(0u);
    image.add_segment({".contextual-read-contract-fixpoint-budget",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x020u,
         0x100u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

struct AbiContractProbeResult {
    bool observed = false;
    bool stack_reads_complete = false;
    std::vector<std::int32_t> stack_read_slots;
    std::uint8_t persistent_store_sources = 0u;
    bool budget_exhausted = false;
};

AbiContractProbeResult fixed_shift_abi_contract_values(
    const std::uint16_t shift_opcode,
    const std::uint32_t input) {
    std::vector<std::uint8_t> bytes(0x34u, 0x09u);
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
    put_u16(0x00u, 0x60F3u); // mov r15,r0
    put_u16(0x02u, 0xD307u); // input literal 0x20 -> r3
    put_u16(0x04u, shift_opcode);
    put_u16(0x06u, 0x043Eu); // mov.l @(r0,r3),r4
    put_u16(0x08u, 0xD506u); // persistent global 0x30 -> r5
    put_u16(0x0Au, 0x2542u);
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);
    put_u32(0x20u, input);
    put_u32(0x24u, 0x30u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".fixed-shift-abi-contract",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u>
        boundaries{{{0x00u, 0x10u}}};
    AbiContractProbeResult probe;
    const auto analysis =
        katana::analysis::detail::
            analyze_function_values_with_abi_contract_observer_for_testing(
                image,
                lines,
                boundaries,
                {},
                [&](const auto& observation) {
                    if (observation.function_address != 0u)
                        return;
                    probe.observed = true;
                    probe.stack_reads_complete =
                        observation.stack_reads_complete;
                    probe.stack_read_slots.assign(
                        observation.stack_read_slots.begin(),
                        observation.stack_read_slots.end());
                    probe.persistent_store_sources =
                        observation.persistent_store_sources;
                });
    probe.budget_exhausted = analysis.budget_exhausted;
    return probe;
}

katana::analysis::FunctionValueAnalysisResult
object_field_tail_values(const bool overwrite_callback_from_object) {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
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
    put_u16(0x00u, 0xE460u); // mov #0x60,r4 (object)
    put_u16(0x02u, 0xE570u); // mov #0x70,r5 (real callback argument)
    put_u16(0x04u, 0xB00Cu); // bsr 0x20
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x000Bu);
    put_u16(0x0Au, 0x0009u);
    put_u16(0x20u, 0xE004u); // mov #4,r0
    put_u16(0x22u,
            overwrite_callback_from_object
                ? 0x054Eu // mov.l @(r0,r4),r5
                : 0x0009u);
    put_u16(0x24u, 0x462Bu); // jmp @r6 (candidate-only tail)
    put_u16(0x26u, 0x0009u);
    put_u16(0x40u, 0x2752u); // mov.l r5,@r7 (unknown object)
    put_u16(0x42u, 0x000Bu);
    put_u16(0x44u, 0x0009u);
    put_u16(0x60u, 0x000Bu); // object address is also decode-valid
    put_u16(0x62u, 0x0009u);
    put_u32(0x64u, 0x70u);   // ordinary field, coincidentally code-like
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".object-field-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x0Cu},
        {0x20u, 0x08u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x24u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
candidate_call_stack_tail_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0xE470u); // mov #0x70,r4 (callback argument)
    put_u16(0x02u, 0x410Bu); // jsr @r1 (candidate-only wrapper)
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);

    put_u16(0x20u, 0x7FFCu); // add #-4,r15
    put_u16(0x22u, 0x2F42u); // mov.l r4,@r15
    put_u16(0x24u, 0x65F2u); // mov.l @r15,r5
    put_u16(0x26u, 0x422Bu); // jmp @r2 (candidate-only registrar)
    put_u16(0x28u, 0x7F04u); // add #4,r15 (delay)

    put_u16(0x40u, 0x900Eu); // mov.w @(0x60,pc),r0 (guarded frame size)
    put_u16(0x42u, 0x3F08u); // sub r0,r15
    put_u16(0x44u, 0x1F51u); // mov.l r5,@(4,r15)
    put_u16(0x46u, 0x53F1u); // mov.l @(4,r15),r3
    put_u16(0x48u, 0x2732u); // mov.l r3,@r7 (unknown object)
    put_u16(0x4Au, 0x7F08u); // add #8,r15
    put_u16(0x4Cu, 0x000Bu);
    put_u16(0x4Eu, 0x0009u);
    put_u16(0x60u, 0x0008u); // writable captured stack-frame size 8

    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".candidate-call-stack-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x0Au},
        {0x20u, 0x0Au},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x02u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x26u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
helper_returned_code_pointer_tail_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0xE470u); // mov #0x70,r4 (callback argument)
    put_u16(0x02u, 0xB00Du); // bsr 0x20 (normal direct helper)
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x6403u); // mov r0,r4
    put_u16(0x08u, 0x422Bu); // jmp @r2 (candidate-only registrar)
    put_u16(0x0Au, 0x0009u);

    put_u16(0x20u, 0x6043u); // helper: mov r4,r0
    put_u16(0x22u, 0x000Bu);
    put_u16(0x24u, 0x0009u);

    put_u16(0x40u, 0x2742u); // registrar: mov.l r4,@r7
    put_u16(0x42u, 0x000Bu);
    put_u16(0x44u, 0x0009u);

    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".helper-returned-code-pointer-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x0Cu},
        {0x20u, 0x06u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x08u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult direct_literal_global_store_values() {
    std::vector<std::uint8_t> bytes(0x40u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD403u); // callback literal 0x30 -> r4
    put_u16(0x02u, 0xD504u); // persistent global 0x20 -> r5
    put_u16(0x04u, 0x2542u); // mov.l r4,@r5
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);
    put_u32(0x10u, 0x30u);
    put_u32(0x14u, 0x20u);
    put_u16(0x30u, 0x000Bu);
    put_u16(0x32u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".direct-literal-global-store",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u> boundaries{{
        {0x00u, 0x0Au},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
reloaded_stack_epoch_values(const bool displaced_reload,
                            const bool retain_old_alias = false,
                            const bool register_reload = false) {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xD407u); // old-stack callback literal 0x20 -> r4
    put_u16(0x02u, 0x2F42u); // old epoch: mov.l r4,@r15
    put_u16(0x04u,
            retain_old_alias
                ? 0x62F3u // preserve an alias of the old r15 in r2
                : 0x0009u);
    put_u16(0x06u,
            displaced_reload ? 0xE140u : 0xE170u);
    put_u16(0x08u,
            register_reload
                ? 0x6F13u // context switch: mov r1,r15
                : displaced_reload
                ? 0x5F15u // context restore: mov.l @(20,r1),r15
                : 0x6F12u); // context restore: mov.l @r1,r15
    put_u16(0x0Au, 0xD406u); // old-alias-only callback literal -> r4
    put_u16(0x0Cu, 0x2242u); // must not become a new-epoch stack slot
    put_u16(0x0Eu, 0xD406u); // new-stack callback literal 0x28 -> r4
    put_u16(0x10u, 0x2F42u); // new epoch: mov.l r4,@r15
    put_u16(0x12u, 0x432Bu); // candidate-only tail -> sink 0x40
    put_u16(0x14u, 0x0009u);

    put_u32(0x20u, 0x80u);
    put_u32(0x24u, 0x88u);
    put_u32(0x28u, 0x90u);

    put_u16(0x40u, 0x64F2u); // sink reloads new slot 0 -> r4
    put_u16(0x42u, 0xD505u); // persistent destination literal 0x58 -> r5
    put_u16(0x44u, 0x2542u);
    put_u16(0x46u, 0x000Bu);
    put_u16(0x48u, 0x0009u);
    put_u32(0x54u, 0x2000u); // displaced restored SP
    put_u32(0x58u, 0x60u);
    put_u32(0x70u, 0x2000u); // direct restored SP

    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);
    put_u16(0x88u, 0x000Bu);
    put_u16(0x8Au, 0x0009u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({register_reload
                           ? ".register-reloaded-stack-epoch"
                           : displaced_reload
                               ? ".displaced-reloaded-stack-epoch"
                               : ".direct-reloaded-stack-epoch",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 5u> boundaries{{
        {0x00u, 0x16u},
        {0x40u, 0x0Au},
        {0x80u, 0x04u},
        {0x88u, 0x04u},
        {0x90u, 0x04u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> edges{{
        {0x12u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
saved_stack_epoch_self_reload_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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
    const auto put_bsr = [&](const std::size_t site,
                             const std::size_t target) {
        const auto displacement =
            (static_cast<std::int64_t>(target) -
             static_cast<std::int64_t>(site + 4u)) /
            2;
        put_u16(site,
                static_cast<std::uint16_t>(
                    0xB000u |
                    (static_cast<std::uint16_t>(displacement) &
                     0x0FFFu)));
    };

    put_u16(0x00u, 0xD40Bu); // old callback 0x80 -> r4
    put_u16(0x02u, 0x2F42u); // old [sp+0] = callback
    put_u16(0x04u, 0xD10Bu); // saved-SP cell 0x60 -> r1
    put_u16(0x06u, 0x21F2u); // unknown r15 -> initialized global
    put_bsr(0x08u, 0x70u);   // epoch must survive a normal helper call
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xDF0Au); // handler SP 0x2000 -> r15
    put_u16(0x0Eu, 0xD40Bu); // handler-only callback 0x88 -> r4
    put_u16(0x10u, 0x2F42u); // handler [sp+0] = callback
    put_u16(0x12u, 0xDF0Bu); // address 0x60 -> r15, new coord 0
    put_u16(0x14u, 0x6FF2u); // Sonic form: mov.l @r15,r15
    put_u16(0x16u, 0x64F2u); // restored old [sp+0] -> r4
    put_u16(0x18u, 0xD50Au); // persistent destination 0x64 -> r5
    put_u16(0x1Au, 0x2542u);
    put_u16(0x1Cu, 0x000Bu);
    put_u16(0x1Eu, 0x0009u);

    put_u32(0x30u, 0x80u);
    put_u32(0x34u, 0x60u);
    put_u32(0x38u, 0x2000u);
    put_u32(0x3Cu, 0x88u);
    put_u32(0x40u, 0x60u);
    put_u32(0x44u, 0x64u);
    put_u32(0x60u, 0x90u); // static decoy must lose to unknown forward

    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);
    for (const auto target : {0x80u, 0x88u, 0x90u}) {
        put_u16(target, 0x000Bu);
        put_u16(target + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".saved-stack-epoch-self-reload",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 5u>
        boundaries{{
            {0x00u, 0x20u},
            {0x70u, 0x04u},
            {0x80u, 0x04u},
            {0x88u, 0x04u},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
stale_saved_stack_epoch_values() {
    std::vector<std::uint8_t> bytes(0x94u, 0x09u);
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
    put_u16(0x00u, 0xD10Bu); // saved-SP cell -> r1
    put_u16(0x02u, 0x21F2u); // save empty current epoch
    put_u16(0x04u, 0xD40Bu); // callback added only after snapshot
    put_u16(0x06u, 0x2F42u);
    put_u16(0x08u, 0xDF0Bu); // handler SP
    put_u16(0x0Au, 0xDF0Cu); // saved-SP cell address -> r15
    put_u16(0x0Cu, 0x6FF2u); // restore stale snapshot
    put_u16(0x0Eu, 0x430Bu); // candidate call -> stack sink
    put_u16(0x10u, 0x0009u);
    put_u16(0x12u, 0x000Bu);
    put_u16(0x14u, 0x0009u);

    put_u16(0x20u, 0x64F2u);
    put_u16(0x22u, 0xD507u);
    put_u16(0x24u, 0x2542u);
    put_u16(0x26u, 0x000Bu);
    put_u16(0x28u, 0x0009u);
    put_u32(0x30u, 0x60u);
    put_u32(0x34u, 0x80u);
    put_u32(0x38u, 0x2000u);
    put_u32(0x3Cu, 0x60u);
    put_u32(0x40u, 0x64u);
    put_u32(0x60u, 0x90u);
    for (const auto target : {0x80u, 0x90u}) {
        put_u16(target, 0x000Bu);
        put_u16(target + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".stale-saved-stack-epoch",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u>
        boundaries{{
            {0x00u, 0x16u},
            {0x20u, 0x0Au},
            {0x80u, 0x04u},
            {0x90u, 0x04u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u>
        edges{{
            {0x0Eu,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
saved_stack_epoch_missing_memory_loop_values() {
    std::vector<std::uint8_t> bytes(0x24u, 0x09u);
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
    put_u16(0x00u, 0xD002u); // fixed cell 0x20 -> r0
    put_u16(0x02u, 0x20F2u); // save current SP epoch
    put_u16(0x04u, 0x2010u); // partial store removes 32-bit cell
    put_u16(0x06u, 0xAFFDu); // bra 0x04
    put_u16(0x08u, 0x0009u);
    put_u32(0x0Cu, 0x20u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".saved-stack-epoch-missing-memory-loop",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u>
        boundaries{{
            {0x00u, 0x0Au},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
saved_stack_epoch_missing_stack_loop_values() {
    std::vector<std::uint8_t> bytes(0x10u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x2FF2u); // [r15] = current SP epoch
    put_u16(0x02u, 0x8902u); // bt 0x0A retains the slot
    put_u16(0x04u, 0x2F00u); // mov.b r0,@r15 removes the long slot
    put_u16(0x06u, 0xAFFCu); // bra 0x02
    put_u16(0x08u, 0x0009u);
    put_u16(0x0Au, 0xAFFAu); // retained-slot path rejoins at 0x02
    put_u16(0x0Cu, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".saved-stack-epoch-missing-stack-loop",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u>
        boundaries{{
            {0x00u, 0x0Eu},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
saved_stack_epoch_regenerated_source_loop_values() {
    std::vector<std::uint8_t> bytes(0x10u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    put_u16(0x00u, 0x8902u); // bt 0x08 chooses the regenerating arm
    put_u16(0x02u, 0x2F00u); // mov.b r0,@r15 removes the long slot
    put_u16(0x04u, 0xAFFCu); // bra 0x00
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x2FF2u); // mov.l r15,@r15 recreates an epoch-only slot
    put_u16(0x0Au, 0xAFF9u); // bra 0x00
    put_u16(0x0Cu, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".saved-stack-epoch-regenerated-source-loop",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u>
        boundaries{{
            {0x00u, 0x0Eu},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
translated_saved_stack_epoch_loop_values(
    const bool direct_local_sink) {
    std::vector<std::uint8_t> bytes(0x94u, 0x09u);
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

    put_u16(0x00u, 0xD40Bu); // callback 0x80 -> r4
    put_u16(0x02u, 0x2F42u); // [r15] = callback
    put_u16(0x04u, 0xD10Cu); // saved-SP cell 0x60 -> r1
    put_u16(0x06u, 0x21F2u); // [r1] = r15 plus captured epoch
    put_u16(0x08u, 0x6A12u); // captured epoch -> r10
    put_u16(0x0Au, 0x8902u); // bt 0x12, unknown T exits loop
    put_u16(0x0Cu, 0x7A01u); // add #1,r10 translates epoch slots
    put_u16(0x0Eu, 0xAFFCu); // bra 0x0A
    put_u16(0x10u, 0x0009u);
    put_u16(0x12u, 0x6FA3u); // consume widened epoch as new r15
    put_u16(0x14u, 0x64F2u); // possible lost slot -> r4
    put_u16(
        0x16u,
        direct_local_sink
            ? 0xD510u // persistent destination 0x64 -> r5
            : 0xB013u); // register-only persistent sink 0x40
    put_u16(
        0x18u,
        direct_local_sink
            ? 0x2542u // owner-local [r5] = r4
            : 0x0009u);
    put_u16(0x1Au, 0x000Bu);
    put_u16(0x1Cu, 0x0009u);

    put_u32(0x30u, 0x80u);
    put_u32(0x38u, 0x60u);
    put_u16(0x40u, 0xD505u); // persistent destination 0x64 -> r5
    put_u16(0x42u, 0x2542u); // [r5] = r4
    put_u16(0x44u, 0x000Bu);
    put_u16(0x46u, 0x0009u);
    put_u32(0x58u, 0x64u);
    put_u32(0x60u, 0x90u); // static decoy loses to memory forwarding
    for (const auto target : {0x80u, 0x90u}) {
        put_u16(target, 0x000Bu);
        put_u16(target + 2u, 0x0009u);
    }

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({direct_local_sink
                           ? ".translated-saved-stack-epoch-local-loop"
                           : ".translated-saved-stack-epoch-forwarded-loop",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    std::vector<katana::analysis::FunctionBoundary> boundaries{
        {0x00u, 0x1Eu},
        {0x80u, 0x04u},
        {0x90u, 0x04u},
    };
    if (!direct_local_sink)
        boundaries.insert(boundaries.begin() + 1u,
                          {0x40u, 0x08u});
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

struct RecursiveStackProjectionWideningResult {
    katana::analysis::FunctionValueAnalysisResult analysis;
    bool recursive_contract_observed = false;
    bool recursive_stack_reads_complete = true;
    std::uint8_t recursive_persistent_store_sources = 0u;
};

RecursiveStackProjectionWideningResult
recursive_stack_projection_widening_values() {
    std::vector<std::uint8_t> bytes(0x110u, 0x09u);
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
    const auto put_bsr = [&](const std::size_t site,
                             const std::size_t target) {
        const auto displacement =
            (static_cast<std::int64_t>(target) -
             static_cast<std::int64_t>(site + 4u)) /
            2;
        put_u16(site,
                static_cast<std::uint16_t>(
                    0xB000u |
                    (static_cast<std::uint16_t>(displacement) &
                     0x0FFFu)));
    };

    // Root ingress contributes one finite stack callback. The recursive
    // function rewrites this exact incoming slot before taking a snapshot, so
    // its seven epoch carriers begin genuinely empty.
    put_u16(0x00u, 0xD003u); // callback literal 0x10 -> r0
    put_u16(0x02u, 0x1F08u); // callback -> incoming [sp+32]
    put_bsr(0x04u, 0x40u);
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x000Bu);
    put_u16(0x0Au, 0x0009u);
    put_u32(0x10u, 0xC0u);

    put_u16(0x40u, 0x6AF3u); // entry SP -> r10
    put_u16(0x42u, 0x7A20u); // incoming callback slot -> r10
    put_u16(0x44u, 0xE000u);
    put_u16(0x46u, 0x2A00u); // byte overwrite removes the old long slot
    put_u16(0x48u, 0x7FD8u); // add #-40,r15
    put_u16(0x4Au, 0x69F3u); // current SP -> r9
    put_u16(0x4Cu, 0x29F2u); // save the still-empty current epoch
    put_u16(0x4Eu, 0x6892u); // saved empty epoch -> r8
    put_u16(0x50u, 0x4800u); // lose its exact coordinate, retain provenance
    for (std::size_t slot = 0u; slot < 7u; ++slot) {
        put_u16(
            0x52u + slot * 2u,
            static_cast<std::uint16_t>(
                0x1F80u | slot)); // seven empty unresolved epoch spills
    }
    put_u16(0x60u, 0xD40Fu); // finite callback literal 0xA0 -> r4
    put_u16(0x62u, 0x1F48u); // callback mutates [sp+32]
    put_u16(0x64u, 0xD50Fu); // callback destination 0x100 -> r5
    put_u16(0x66u, 0x2542u); // keep the finite callback in the inventory
    put_u16(0x68u, 0x56FAu); // consume prior-ring loss from [sp+40]
    put_u16(0x6Au, 0xD70Fu); // loss destination 0x104 -> r7
    put_u16(0x6Cu, 0x2762u); // persistent sink observes the lost slot
    put_bsr(0x6Eu, 0x40u);   // same callsite grows the frame by 40 per ring
    put_u16(0x70u, 0x0009u);
    put_u16(0x72u, 0x410Bu); // unresolved jsr @r1: callee set incomplete
    put_u16(0x74u, 0x0009u);
    put_u16(0x76u, 0x000Bu);
    put_u16(0x78u, 0x0009u);

    put_u32(0xA0u, 0xC0u);
    put_u32(0xA4u, 0x100u);
    put_u32(0xA8u, 0x104u);
    put_u16(0xC0u, 0x000Bu);
    put_u16(0xC2u, 0x0009u);
    put_u32(0x100u, 0u);
    put_u32(0x104u, 0u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".recursive-stack-projection-widening",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x0Cu},
            {0x40u, 0x3Au},
            {0xC0u, 0x04u},
        }};
    RecursiveStackProjectionWideningResult result;
    result.analysis =
        katana::analysis::detail::
            analyze_function_values_with_abi_contract_observer_for_testing(
                image,
                lines,
                boundaries,
                {},
                [&](const auto& observation) {
                    if (observation.function_address != 0x40u)
                        return;
                    result.recursive_contract_observed = true;
                    result.recursive_stack_reads_complete =
                        observation.stack_reads_complete;
                    result.recursive_persistent_store_sources =
                        observation.persistent_store_sources;
                });
    return result;
}

katana::analysis::FunctionValueAnalysisResult mixed_literal_scalar_store_values() {
    std::vector<std::uint8_t> bytes(0x40u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0x8902u); // bt 0x08, unknown T
    put_u16(0x02u, 0xD404u); // literal 0x30 -> r4
    put_u16(0x04u, 0xA002u); // bra 0x0C
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0xE434u); // decode-valid scalar, no literal provenance
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xD503u); // persistent global 0x20 -> r5
    put_u16(0x0Eu, 0x2542u); // mov.l r4,@r5
    put_u16(0x10u, 0x000Bu);
    put_u16(0x12u, 0x0009u);
    put_u32(0x14u, 0x30u);
    put_u32(0x1Cu, 0x20u);
    put_u16(0x30u, 0x000Bu);
    put_u16(0x32u, 0x0009u);
    put_u16(0x34u, 0x000Bu);
    put_u16(0x36u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".mixed-literal-scalar-store",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u> boundaries{{
        {0x00u, 0x14u},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult wide_pc_literal_join_store_values() {
    std::vector<std::uint8_t> bytes(0x100u, 0x09u);
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
    constexpr std::array<std::uint16_t, 8u> literal_loads{
        0xD41Fu, 0xD41Eu, 0xD41Du, 0xD41Cu,
        0xD41Bu, 0xD41Au, 0xD419u, 0xD418u,
    };
    constexpr std::array<std::uint16_t, 8u> join_branches{
        0xA020u, 0xA01Cu, 0xA018u, 0xA014u,
        0xA010u, 0xA00Cu, 0xA008u, 0xA004u,
    };
    constexpr std::array<std::uint32_t, 9u> callbacks{
        0xC0u, 0xC4u, 0xC8u, 0xCCu, 0xD0u,
        0xD4u, 0xD8u, 0xDCu, 0xE0u,
    };
    for (std::size_t index = 0u; index < literal_loads.size(); ++index) {
        const auto address = index * 8u;
        put_u16(address, 0x8902u); // bt to the next candidate node
        put_u16(address + 2u, literal_loads[index]);
        put_u16(address + 4u, join_branches[index]);
        put_u16(address + 6u, 0x0009u);
    }
    put_u16(0x40u, 0xD417u); // ninth callback literal 0xA0 -> r4
    put_u16(0x42u, 0xA001u); // bra 0x48
    put_u16(0x44u, 0x0009u);
    put_u16(0x48u, 0xD516u); // persistent global 0xB0 -> r5
    put_u16(0x4Au, 0x2542u); // joined callback r4 -> global
    put_u16(0x4Cu, 0x000Bu);
    put_u16(0x4Eu, 0x0009u);
    for (std::size_t index = 0u; index < callbacks.size(); ++index) {
        put_u32(0x80u + index * 4u, callbacks[index]);
        put_u16(callbacks[index], 0x000Bu);
        put_u16(callbacks[index] + 2u, 0x0009u);
    }
    put_u32(0xA4u, 0xB0u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".wide-pc-literal-join-store",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 1u> boundaries{{
        {0x00u, 0x50u},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::io::ExecutableImage wide_abi_stack_indirect_call_image() {
    std::vector<std::uint8_t> bytes(0x180u, 0x09u);
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
    constexpr std::array<std::uint16_t, 8u> literal_loads{
        0xD71Fu, 0xD71Eu, 0xD71Du, 0xD71Cu,
        0xD71Bu, 0xD71Au, 0xD719u, 0xD718u,
    };
    constexpr std::array<std::uint16_t, 8u> join_branches{
        0xA020u, 0xA01Cu, 0xA018u, 0xA014u,
        0xA010u, 0xA00Cu, 0xA008u, 0xA004u,
    };
    constexpr std::array<std::uint32_t, 9u> callbacks{
        0x140u, 0x144u, 0x148u, 0x14Cu, 0x150u,
        0x154u, 0x158u, 0x15Cu, 0x160u,
    };
    for (std::size_t index = 0u; index < literal_loads.size(); ++index) {
        const auto address = index * 8u;
        put_u16(address, 0x8902u);
        put_u16(address + 2u, literal_loads[index]);
        put_u16(address + 4u, join_branches[index]);
        put_u16(address + 6u, 0x0009u);
    }
    put_u16(0x40u, 0xD717u); // ninth callback literal -> r7
    put_u16(0x42u, 0xA001u); // bra 0x48
    put_u16(0x44u, 0x0009u);
    put_u16(0x48u, 0xB032u); // bsr 0xB0
    put_u16(0x4Au, 0x0009u);
    put_u16(0x4Cu, 0x000Bu);
    put_u16(0x4Eu, 0x0009u);
    for (std::size_t index = 0u; index < callbacks.size(); ++index) {
        put_u32(0x80u + index * 4u, callbacks[index]);
        put_u16(callbacks[index], 0x000Bu);
        put_u16(callbacks[index] + 2u, 0x0009u);
    }

    put_u16(0xB0u, 0x2FE6u); // mov.l r14,@-r15
    put_u16(0xB2u, 0x4F22u); // sts.l pr,@-r15
    put_u16(0xB4u, 0xFF0Bu); // fmov.s fr0,@-r15 (FPSCR.SZ: 4/8)
    put_u16(0xB6u, 0x6EF3u); // mov r15,r14
    put_u16(0xB8u, 0xE000u); // mov #0,r0
    put_u16(0xBAu, 0x0E76u); // mov.l r7,@(r0,r14)
    put_u16(0xBCu, 0xB020u); // bsr 0x100 (ordinary helper)
    put_u16(0xBEu, 0x0009u);
    put_u16(0xC0u, 0xE000u); // mov #0,r0 (helper clobbers r0)
    put_u16(0xC2u, 0x03EEu); // mov.l @(r0,r14),r3
    put_u16(0xC4u, 0x430Bu); // jsr @r3
    put_u16(0xC6u, 0x0009u);
    put_u16(0xC8u, 0xF0F9u); // fmov.s @r15+,fr0 (FPSCR.SZ: 4/8)
    put_u16(0xCAu, 0x4F26u); // lds.l @r15+,pr
    put_u16(0xCCu, 0x6EF6u); // mov.l @r15+,r14
    put_u16(0xCEu, 0x000Bu);
    put_u16(0xD0u, 0x0009u);

    put_u16(0x100u, 0x000Bu);
    put_u16(0x102u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".wide-abi-stack-indirect-call",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(0u);
    return image;
}

katana::analysis::FunctionValueAnalysisResult
wide_abi_stack_indirect_call_values() {
    const auto image = wide_abi_stack_indirect_call_image();
    const auto lines =
        katana::sh4::disassemble(image.segments().front().bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u> boundaries{{
        {0x00u, 0x50u},
        {0xB0u, 0x22u},
        {0x100u, 0x04u},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
mixed_null_callback_register_argument_values() {
    constexpr auto base = std::uint32_t{0x1000u};
    std::vector<std::uint8_t> bytes(0x60u, 0x09u);
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
    put_u16(0x00u, 0x8902u); // bt 0x08, unknown T
    put_u16(0x02u, 0xD404u); // callback literal 0x14 -> r4
    put_u16(0x04u, 0xA002u); // bra 0x0C
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0xE400u); // nullptr alternative
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xB010u); // bsr base + 0x30
    put_u16(0x0Eu, 0x0009u);
    put_u16(0x10u, 0x000Bu);
    put_u16(0x12u, 0x0009u);
    put_u32(0x14u, base + 0x50u);

    put_u16(0x30u, 0x2742u); // registrar stores r4 to unknown object r7
    put_u16(0x32u, 0x000Bu);
    put_u16(0x34u, 0x0009u);
    put_u16(0x50u, 0x000Bu);
    put_u16(0x52u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".mixed-null-callback-register-argument",
                       base,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(base);
    const auto lines = katana::sh4::disassemble(bytes, base);
    const std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {base, 0x14u},
        {base + 0x30u, 0x06u},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
sub_register_fifth_stack_callback_values(const bool resolved_frame_size,
                                         const bool store_callback = true) {
    std::vector<std::uint8_t> bytes(0x70u, 0x09u);
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
    put_u16(0x00u, resolved_frame_size ? 0x980Au : 0x0009u);
    put_u16(0x02u, 0x3F88u); // sub r8,r15
    put_u16(0x04u, store_callback ? 0xD006u : 0xE001u);
    // callback literal 0x20 / harmless scalar -> r0
    put_u16(0x06u, 0x2F02u); // fifth ABI argument r0 -> [sp]
    put_u16(0x08u, 0xB012u); // bsr 0x30
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0x3F8Cu); // add r8,r15
    put_u16(0x0Eu, 0x000Bu);
    put_u16(0x10u, 0x0009u);
    // Writable mov.w PC literal: a guarded singleton matching Sonic's real
    // 14,352-byte GCC-style frame allocation.
    put_u32(0x18u, 14'352u);
    put_u32(0x20u, 0x50u);

    put_u16(0x30u, 0x64F2u); // mov.l @r15,r4
    put_u16(0x32u, 0xD505u); // persistent global literal 0x48 -> r5
    put_u16(0x34u, 0x2542u); // callback r4 -> global
    put_u16(0x36u, 0x000Bu);
    put_u16(0x38u, 0x0009u);
    put_u32(0x48u, 0x60u);
    put_u16(0x50u, 0x000Bu);
    put_u16(0x52u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({resolved_frame_size ? ".sub-register-fifth-stack-callback"
                       : store_callback ? ".unresolved-sub-register-stack-base"
                                        : ".harmless-unresolved-stack-access",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x12u},
        {0x30u, 0x0Au},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult fifth_stack_callback_values() {
    std::vector<std::uint8_t> bytes(0x70u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD004u); // callback literal 0x50 -> r0
    put_u16(0x02u, 0x61F3u); // mov r15,r1
    put_u16(0x04u, 0x71FCu); // add #-4,r1
    put_u16(0x06u, 0xD204u); // local literal 0x54 -> r2
    put_u16(0x08u, 0x2122u); // caller local r2 -> [sp-4]
    put_u16(0x0Au, 0x2F02u); // fifth ABI argument r0 -> [sp]
    put_u16(0x0Cu, 0xB010u); // bsr 0x30
    put_u16(0x0Eu, 0x0009u);
    put_u16(0x10u, 0x000Bu);
    put_u16(0x12u, 0x0009u);
    put_u32(0x14u, 0x50u);
    put_u32(0x18u, 0x54u);

    put_u16(0x30u, 0x64F2u); // mov.l @r15,r4
    put_u16(0x32u, 0x61F3u); // mov r15,r1
    put_u16(0x34u, 0x71FCu); // add #-4,r1
    put_u16(0x36u, 0x6612u); // mov.l @r1,r6
    put_u16(0x38u, 0xD503u); // global A 0x60 -> r5
    put_u16(0x3Au, 0x2542u); // callback r4 -> global A
    put_u16(0x3Cu, 0xD503u); // global B 0x64 -> r5
    put_u16(0x3Eu, 0x2562u); // caller-local r6 -> global B
    put_u16(0x40u, 0x000Bu);
    put_u16(0x42u, 0x0009u);
    put_u32(0x48u, 0x60u);
    put_u32(0x4Cu, 0x64u);
    put_u16(0x50u, 0x000Bu);
    put_u16(0x52u, 0x0009u);
    put_u16(0x54u, 0x000Bu);
    put_u16(0x56u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".fifth-stack-callback",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x14u},
        {0x30u, 0x14u},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
exact_abi_stack_slot_projection_values() {
    std::vector<std::uint8_t> bytes(0x190u, 0x09u);
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
    put_u16(0x00u, 0xD045u); // callback literal 0x118 -> r0
    put_u16(0x02u, 0xD346u); // guarded target literal 0x11C -> r3
    put_u16(0x04u, 0xE201u); // harmless scalar -> r2
    put_u16(0x06u, 0x61F3u); // mov r15,r1
    put_u16(0x08u, 0x2102u); // callback -> outgoing stack slot 0
    auto cursor = std::size_t{0x0Au};
    for (std::size_t slot = 1u; slot <= 64u; ++slot) {
        static_cast<void>(slot);
        put_u16(cursor, 0x7104u); // add #4,r1
        put_u16(cursor + 2u, 0x2122u); // scalar -> outgoing slot
        cursor += 4u;
    }
    put_u16(0x10Au, 0x430Bu); // jsr @r3, guarded-partial known target
    put_u16(0x10Cu, 0x0009u);
    put_u16(0x10Eu, 0x000Bu);
    put_u16(0x110u, 0x0009u);
    put_u32(0x118u, 0x180u);
    put_u32(0x11Cu, 0x140u);

    put_u16(0x140u, 0x8904u); // bt 0x14C (unknown condition)
    put_u16(0x142u, 0x64F2u); // store path: only incoming slot 0 -> r4
    put_u16(0x144u, 0xD506u); // persistent global literal -> r5
    put_u16(0x146u, 0x2542u); // callback r4 -> global
    put_u16(0x148u, 0x000Bu);
    put_u16(0x14Au, 0x0009u);
    put_u16(0x14Cu, 0xD305u); // tail path: epilogue literal -> r3
    put_u16(0x14Eu, 0x432Bu); // jmp @r3
    put_u16(0x150u, 0x0009u);
    put_u32(0x160u, 0x178u);
    put_u32(0x164u, 0x170u);
    put_u16(0x170u, 0x000Bu); // pure epilogue inventory region
    put_u16(0x172u, 0x0009u);
    put_u16(0x180u, 0x000Bu);
    put_u16(0x182u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".exact-abi-stack-slot-projection",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x112u},
        {0x140u, 0x12u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x10Au,
         0x140u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x14Eu,
         0x170u,
         katana::analysis::ResolvedControlFlowKind::Jump,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult fifth_stack_callback_tail_values() {
    std::vector<std::uint8_t> bytes(0x80u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xD005u); // fifth callback literal 0x70 -> r0
    put_u16(0x02u, 0x2F02u); // r0 -> caller [sp]
    put_u16(0x04u, 0x410Bu); // jsr @r1 (candidate-only wrapper)
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x000Bu);
    put_u16(0x0Au, 0x0009u);
    put_u32(0x18u, 0x70u);

    put_u16(0x20u, 0x65F2u); // wrapper: mov.l @r15,r5
    put_u16(0x22u, 0x422Bu); // jmp @r2 (candidate-only tail)
    put_u16(0x24u, 0x0009u);

    put_u16(0x40u, 0xD704u); // persistent global 0x5C -> r7
    put_u16(0x42u, 0x2752u); // tail registrar stores r5
    put_u16(0x44u, 0x000Bu);
    put_u16(0x46u, 0x0009u);
    put_u32(0x54u, 0x5Cu);
    put_u16(0x70u, 0x000Bu);
    put_u16(0x72u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".fifth-stack-callback-tail",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u> boundaries{{
        {0x00u, 0x0Cu},
        {0x20u, 0x06u},
    }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
        {0x04u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
        {0x22u,
         0x40u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
         true},
    }};
    return katana::analysis::analyze_function_values(image, lines, boundaries, edges);
}

katana::analysis::FunctionValueAnalysisResult
mixed_stack_object_destination_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0x8902u); // bt 0x08, unknown T
    put_u16(0x02u, 0x64F3u); // stack path: mov r15,r4
    put_u16(0x04u, 0xA002u); // bra 0x0C
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0xE470u); // object path: mov #0x70,r4
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xD504u); // callback literal 0x20 -> r5
    put_u16(0x0Eu, 0xB017u); // bsr 0x40
    put_u16(0x10u, 0x0009u);
    put_u16(0x12u, 0x000Bu);
    put_u16(0x14u, 0x0009u);
    put_u32(0x20u, 0x90u);

    put_u16(0x40u, 0x2452u); // mixed destination: mov.l r5,@r4
    put_u16(0x42u, 0xB00Du); // bsr 0x60
    put_u16(0x44u, 0x0009u);
    put_u16(0x46u, 0x000Bu);
    put_u16(0x48u, 0x0009u);

    put_u16(0x60u, 0x64F2u); // reload caller slot 0 -> r4
    put_u16(0x62u, 0xD505u); // persistent destination literal -> r5
    put_u16(0x64u, 0x2542u); // mov.l r4,@r5
    put_u16(0x66u, 0x000Bu);
    put_u16(0x68u, 0x0009u);
    put_u32(0x78u, 0x7Cu);

    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".mixed-stack-object-destination",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, true, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u> boundaries{{
        {0x00u, 0x16u},
        {0x40u, 0x0Au},
        {0x60u, 0x0Au},
        {0x90u, 0x04u},
    }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, {});
}

katana::analysis::FunctionValueAnalysisResult helper_mixed_return_store_values() {
    std::vector<std::uint8_t> bytes(0x70u, 0x09u);
    const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    put_u16(0x00u, 0xB00Eu); // bsr 0x20
    put_u16(0x02u, 0x0009u);
    put_u16(0x04u, 0x6403u); // mov r0,r4
    put_u16(0x06u, 0xD504u); // persistent global 0x38 -> r5
    put_u16(0x08u, 0x2542u); // mov.l r4,@r5
    put_u16(0x0Au, 0x000Bu);
    put_u16(0x0Cu, 0x0009u);
    put_u32(0x18u, 0x38u);

    put_u16(0x20u, 0x8902u); // bt 0x28, unknown T
    put_u16(0x22u, 0xD003u); // literal 0x60 -> r0
    put_u16(0x24u, 0xA002u); // bra 0x2C
    put_u16(0x26u, 0x0009u);
    put_u16(0x28u, 0xE064u); // decode-valid scalar, no literal provenance
    put_u16(0x2Au, 0x0009u);
    put_u16(0x2Cu, 0x000Bu);
    put_u16(0x2Eu, 0x0009u);
    put_u32(0x30u, 0x60u);
    put_u16(0x60u, 0x000Bu);
    put_u16(0x62u, 0x0009u);
    put_u16(0x64u, 0x000Bu);
    put_u16(0x66u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".helper-mixed-return-store",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Mixed,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<std::uint32_t, 2u> entries{{0x00u, 0x20u}};
    return katana::analysis::analyze_function_values(image, lines, entries);
}

enum class ReturnedStackCallbackLossCase {
    ConsumeReturnedR0,
    OverwriteReturnedR0,
    OverwriteReturnedR0WithMoveT,
};

katana::analysis::FunctionValueAnalysisResult
returned_stack_callback_loss_values(
    const ReturnedStackCallbackLossCase test_case) {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u);
    if (test_case ==
        ReturnedStackCallbackLossCase::ConsumeReturnedR0) {
        put_u16(0x04u, 0x6403u); // returned r0 -> r4
        put_u16(0x06u, 0xD507u); // destination literal 0x24 -> r5
        put_u16(0x08u, 0x2542u); // persistent [r5] = r4
        put_u16(0x0Au, 0x000Bu);
        put_u16(0x0Cu, 0x0009u);
    } else if (
        test_case ==
        ReturnedStackCallbackLossCase::OverwriteReturnedR0) {
        put_u16(0x04u, 0xE000u); // overwrite returned r0
        put_u16(0x06u, 0xD406u); // fresh callback literal 0x20 -> r4
        put_u16(0x08u, 0xD506u); // destination literal 0x24 -> r5
        put_u16(0x0Au, 0x2542u); // persistent [r5] = r4
        put_u16(0x0Cu, 0x000Bu);
        put_u16(0x0Eu, 0x0009u);
    } else {
        put_u16(0x04u, 0x0029u); // movt r0 is an exact overwrite
        put_u16(0x06u, 0x6403u); // overwritten r0 -> r4
        put_u16(0x08u, 0xD506u); // destination literal 0x24 -> r5
        put_u16(0x0Au, 0x2542u); // must not carry the stale loss marker
        put_u16(0x0Cu, 0x000Bu);
        put_u16(0x0Eu, 0x0009u);
    }
    put_u32(0x20u, 0x90u);
    put_u32(0x24u, 0x84u);

    put_u16(0x40u, 0xD405u); // callback literal 0x58 -> r4
    put_u16(0x42u, 0x2F42u); // [r15] = callback
    put_u16(0x44u, 0x3F0Cu); // unknown add r0,r15 loses its slot
    put_u16(0x46u, 0x60F2u); // possible lost callback -> r0
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u32(0x58u, 0x90u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {test_case ==
                 ReturnedStackCallbackLossCase::ConsumeReturnedR0
             ? ".returned-stack-callback-loss-consumed"
             : test_case ==
                       ReturnedStackCallbackLossCase::OverwriteReturnedR0
                   ? ".returned-stack-callback-loss-overwritten"
                   : ".returned-stack-callback-loss-movt-overwritten",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    const std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u,
             test_case ==
                     ReturnedStackCallbackLossCase::ConsumeReturnedR0
                 ? 0x0Eu
                 : 0x10u},
            {0x40u, 0x0Cu},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

enum class MemoryStackCallbackLossCase {
    ConsumeExactCell,
    ReadUnknownCell,
    ReadUnknownEmptySavedEpoch,
    ReadDifferentCell,
    EmptySavedEpoch,
};

katana::analysis::FunctionValueAnalysisResult
memory_stack_callback_loss_values(
    const MemoryStackCallbackLossCase test_case) {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    const bool consume =
        test_case == MemoryStackCallbackLossCase::ConsumeExactCell ||
        test_case == MemoryStackCallbackLossCase::ReadUnknownCell;
    const bool capture_callback =
        test_case != MemoryStackCallbackLossCase::EmptySavedEpoch &&
        test_case !=
            MemoryStackCallbackLossCase::
                ReadUnknownEmptySavedEpoch;
    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u);
    put_u16(
        0x04u,
        test_case == MemoryStackCallbackLossCase::ReadUnknownCell ||
                test_case ==
                    MemoryStackCallbackLossCase::
                        ReadUnknownEmptySavedEpoch
            ? 0x6103u // unknown returned r0 -> address r1
            : 0xD107u); // selected-cell literal 0x24 -> r1
    put_u16(0x06u, 0x6A12u); // [r1] -> r10
    put_u16(0x08u, 0x6FA3u); // restored candidate epoch -> r15
    put_u16(0x0Au, 0x64F2u); // possible lost callback -> r4
    if (consume) {
        put_u16(0x0Cu, 0xD506u); // destination literal 0x28 -> r5
        put_u16(0x0Eu, 0x2542u); // consume the unresolved value
        put_u16(0x10u, 0x000Bu);
        put_u16(0x12u, 0x0009u);
    } else {
        put_u16(0x0Cu, 0xD404u); // overwrite with fresh callback
        put_u16(0x0Eu, 0xD506u); // destination literal 0x28 -> r5
        put_u16(0x10u, 0x2542u); // independent valid callback store
        put_u16(0x12u, 0x000Bu);
        put_u16(0x14u, 0x0009u);
    }
    put_u32(0x20u, 0x90u);
    put_u32(
        0x24u,
        test_case == MemoryStackCallbackLossCase::ReadDifferentCell
            ? 0x88u
            : 0x80u);
    put_u32(0x28u, 0x84u);

    if (capture_callback) {
        put_u16(0x40u, 0xD405u); // callback literal 0x58 -> r4
        put_u16(0x42u, 0x2F42u); // capture callback on current stack
    } else {
        put_u16(0x40u, 0x0009u);
        put_u16(0x42u, 0x0009u);
    }
    put_u16(0x44u, 0xD105u); // saved-SP cell literal 0x5C -> r1
    put_u16(0x46u, 0x21F2u); // [r1] = r15 plus captured epoch
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u32(0x58u, 0x90u);
    put_u32(0x5Cu, 0x80u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {consume
             ? test_case ==
                       MemoryStackCallbackLossCase::ReadUnknownCell
                   ? ".memory-stack-callback-loss-unknown-cell"
                   : ".memory-stack-callback-loss-consumed"
             : test_case ==
                       MemoryStackCallbackLossCase::
                           ReadUnknownEmptySavedEpoch
                   ? ".memory-stack-callback-loss-unknown-empty-epoch"
             : test_case ==
                       MemoryStackCallbackLossCase::ReadDifferentCell
                   ? ".memory-stack-callback-loss-other-cell"
                   : ".memory-stack-callback-loss-empty-epoch",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    const std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, consume ? 0x14u : 0x16u},
            {0x40u, 0x0Cu},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
unknown_saved_stack_epoch_memory_roundtrip_values(
    const bool callback_after_empty_snapshot = false) {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    if (callback_after_empty_snapshot) {
        put_u16(0x00u, 0x6103u); // unknown r0 -> address r1
        put_u16(0x02u, 0x21F2u); // unknown [r1] = empty saved r15 epoch
        put_u16(0x04u, 0xD40Au); // callback literal 0x30 -> r4
        put_u16(0x06u, 0x2F42u); // callback mutates the captured current epoch
    } else {
        put_u16(0x00u, 0xD40Bu); // callback literal 0x30 -> r4
        put_u16(0x02u, 0x2F42u); // current [r15] = callback
        put_u16(0x04u, 0x6103u); // unknown r0 -> address r1
        put_u16(0x06u, 0x21F2u); // unknown [r1] = saved r15 epoch
    }
    put_u16(0x08u, 0x6F12u); // reload saved epoch through unknown [r1]
    put_u16(0x0Au, 0x64F2u); // possible restored callback -> r4
    put_u16(0x0Cu, 0xD509u); // destination literal 0x34 -> r5
    put_u16(0x0Eu, 0x2542u); // consume the unresolved value
    put_u16(0x10u, 0x000Bu);
    put_u16(0x12u, 0x0009u);
    put_u32(0x30u, 0x80u);
    put_u32(0x34u, 0x84u);
    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {callback_after_empty_snapshot
             ? ".unknown-empty-saved-stack-epoch-late-callback"
             : ".unknown-saved-stack-epoch-memory-roundtrip",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{
            {0x00u, 0x14u},
            {0x80u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
identity_transformed_old_stack_alias_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xD40Bu); // old-stack callback literal 0x30 -> r4
    put_u16(0x02u, 0x2F42u); // old [r15] = callback
    put_u16(0x04u, 0x60F3u); // preserve old r15 in r0
    put_u16(0x06u, 0xCB00u); // or #0,r0 is an exact identity
    put_u16(0x08u, 0xE140u); // replacement stack value -> r1
    put_u16(0x0Au, 0x6F13u); // fresh epoch: mov r1,r15
    put_u16(0x0Cu, 0x6402u); // old [r0] callback -> r4
    put_u16(0x0Eu, 0xD509u); // destination literal 0x34 -> r5
    put_u16(0x10u, 0x2542u); // consume the unresolved old alias
    put_u16(0x12u, 0x000Bu);
    put_u16(0x14u, 0x0009u);
    put_u32(0x30u, 0x80u);
    put_u32(0x34u, 0x84u);
    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".identity-transformed-old-stack-alias",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{
            {0x00u, 0x16u},
            {0x80u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
merged_old_stack_alias_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xD40Bu); // old-stack callback literal 0x30 -> r4
    put_u16(0x02u, 0x2F42u); // old [r15] = callback
    put_u16(0x04u, 0x8902u); // bt 0x0C: path without alias
    put_u16(0x06u, 0x60F3u); // alias path: old r15 -> r0
    put_u16(0x08u, 0xA001u); // bra merge at 0x0E
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0xE000u); // other path: r0 is not an alias
    put_u16(0x0Eu, 0xE140u); // replacement stack value -> r1
    put_u16(0x10u, 0x6F13u); // fresh epoch: mov r1,r15
    put_u16(0x12u, 0x6402u); // possible old [r0] callback -> r4
    put_u16(0x14u, 0xD507u); // destination literal 0x34 -> r5
    put_u16(0x16u, 0x2542u); // consume the merged alias loss
    put_u16(0x18u, 0x000Bu);
    put_u16(0x1Au, 0x0009u);
    put_u32(0x30u, 0x80u);
    put_u32(0x34u, 0x84u);
    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".merged-old-stack-alias",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{
            {0x00u, 0x1Cu},
            {0x80u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
conditional_memory_stack_callback_loss_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u);
    put_u16(0x04u, 0xD106u); // marked-cell literal 0x20 -> r1
    put_u16(0x06u, 0x6412u); // possible lost callback -> r4
    put_u16(0x08u, 0xD506u); // destination literal 0x24 -> r5
    put_u16(0x0Au, 0x2542u); // consume the unresolved value
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);
    put_u32(0x20u, 0x80u);
    put_u32(0x24u, 0x84u);

    put_u16(0x40u, 0x8906u); // bt 0x50, path without cell
    put_u16(0x42u, 0xD407u); // callback literal 0x60 -> r4
    put_u16(0x44u, 0x2F42u); // [r15] = callback
    put_u16(0x46u, 0x3F0Cu); // unknown add r0,r15 loses its slot
    put_u16(0x48u, 0x64F2u); // possible lost callback -> r4
    put_u16(0x4Au, 0xD106u); // marked-cell literal 0x64 -> r1
    put_u16(0x4Cu, 0x2142u); // exact cell gets raw loss marker
    put_u16(0x4Eu, 0xA000u); // bra 0x52
    put_u16(0x50u, 0x0009u);
    put_u16(0x52u, 0x000Bu);
    put_u16(0x54u, 0x0009u);
    put_u32(0x60u, 0x90u);
    put_u32(0x64u, 0x80u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".conditional-memory-stack-callback-loss",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x10u},
            {0x40u, 0x16u},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
conditional_stack_slot_callback_loss_values() {
    std::vector<std::uint8_t> bytes(0xA0u, 0x09u);
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

    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u);
    put_u16(0x04u, 0x8902u); // bt 0x0C, path without spill
    put_u16(0x06u, 0x2F02u); // [r15] = returned loss marker
    put_u16(0x08u, 0xA001u); // bra 0x0E
    put_u16(0x0Au, 0x0009u);
    put_u16(0x0Cu, 0x0009u);
    put_u16(0x0Eu, 0x64F2u); // possible lost callback -> r4
    put_u16(0x10u, 0xD503u); // destination literal 0x20 -> r5
    put_u16(0x12u, 0x2542u); // consume the unresolved value
    put_u16(0x14u, 0x000Bu);
    put_u16(0x16u, 0x0009u);
    put_u32(0x20u, 0x84u);

    put_u16(0x40u, 0xD405u); // callback literal 0x58 -> r4
    put_u16(0x42u, 0x2F42u); // [r15] = callback
    put_u16(0x44u, 0x3F0Cu); // unknown add r0,r15 loses its slot
    put_u16(0x46u, 0x60F2u); // possible lost callback -> r0
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u32(0x58u, 0x90u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".conditional-stack-slot-callback-loss",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x18u},
            {0x40u, 0x0Cu},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
candidate_only_memory_stack_callback_loss_values() {
    std::vector<std::uint8_t> bytes(0xD4u, 0x09u);
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

    put_u16(0x00u, 0xB04Eu); // bsr marker producer 0xA0
    put_u16(0x02u, 0x0009u);
    put_u16(0x04u, 0xB00Cu); // bsr candidate-call owner 0x20
    put_u16(0x06u, 0x0009u);
    put_u16(0x08u, 0x000Bu);
    put_u16(0x0Au, 0x0009u);

    put_u16(0x20u, 0x410Bu); // guarded candidate-only call -> 0x40
    put_u16(0x22u, 0x0009u);
    put_u16(0x24u, 0x000Bu);
    put_u16(0x26u, 0x0009u);

    put_u16(0x40u, 0xD105u); // marked-cell literal 0x58 -> r1
    put_u16(0x42u, 0x6412u); // exact marked cell -> r4
    put_u16(0x44u, 0xD505u); // destination literal 0x5C -> r5
    put_u16(0x46u, 0x2542u); // marker reaches local persistent sink
    put_u16(0x48u, 0x000Bu);
    put_u16(0x4Au, 0x0009u);
    put_u32(0x58u, 0x80u);
    put_u32(0x5Cu, 0x84u);

    put_u16(0xA0u, 0xD405u); // callback literal 0xB8 -> r4
    put_u16(0xA2u, 0x2F42u); // [r15] = callback
    put_u16(0xA4u, 0xD105u); // saved-SP cell literal 0xBC -> r1
    put_u16(0xA6u, 0x21F2u); // cell gets captured callback epoch
    put_u16(0xA8u, 0x000Bu);
    put_u16(0xAAu, 0x0009u);
    put_u32(0xB8u, 0xD0u);
    put_u32(0xBCu, 0x80u);
    put_u16(0xD0u, 0x000Bu);
    put_u16(0xD2u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".candidate-only-memory-stack-callback-loss",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 5u>
        boundaries{{
            {0x00u, 0x0Cu},
            {0x20u, 0x08u},
            {0x40u, 0x0Cu},
            {0xA0u, 0x0Cu},
            {0xD0u, 0x04u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u>
        edges{{
            {0x20u,
             0x40u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

struct OmittedSavedStackAliasCallbackResult {
    katana::analysis::FunctionValueAnalysisResult analysis;
    bool callee_contract_observed = false;
    bool callee_stack_reads_complete = false;
    std::vector<std::int32_t> callee_stack_read_slots;
};

OmittedSavedStackAliasCallbackResult
omitted_saved_stack_alias_callback_values() {
    std::vector<std::uint8_t> bytes(0x84u, 0x09u);
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

    put_u16(0x00u, 0x1FF1u); // save empty r15 epoch in outgoing [sp+4]
    put_u16(0x02u, 0xB01Du); // bsr 0x40
    put_u16(0x04u, 0x0009u);
    put_u16(0x06u, 0x000Bu);
    put_u16(0x08u, 0x0009u);

    put_u16(0x40u, 0xD407u); // callback literal 0x60 -> r4
    put_u16(0x42u, 0x2F42u); // callback mutates the captured epoch
    put_u16(0x44u, 0x000Bu);
    put_u16(0x46u, 0x0009u);
    put_u32(0x60u, 0x80u);
    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".omitted-saved-stack-alias-callback",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{
            {0x00u, 0x0Au},
            {0x40u, 0x08u},
            {0x80u, 0x04u},
        }};
    OmittedSavedStackAliasCallbackResult result;
    result.analysis =
        katana::analysis::detail::
            analyze_function_values_with_abi_contract_observer_for_testing(
                image,
                lines,
                boundaries,
                {},
                [&](const auto& observation) {
                    if (observation.function_address != 0x40u)
                        return;
                    result.callee_contract_observed = true;
                    result.callee_stack_reads_complete =
                        observation.stack_reads_complete;
                    result.callee_stack_read_slots.assign(
                        observation.stack_read_slots.begin(),
                        observation.stack_read_slots.end());
                });
    return result;
}

katana::analysis::FunctionValueAnalysisResult
duplicate_saved_stack_epoch_restore_then_callback_values() {
    std::vector<std::uint8_t> bytes(0x94u, 0x09u);
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

    put_u16(0x00u, 0xD10Bu); // saved-SP cell A 0x80 -> r1
    put_u16(0x02u, 0x21F2u); // capture empty epoch A
    put_u16(0x04u, 0xD20Bu); // saved-SP cell B 0x84 -> r2
    put_u16(0x06u, 0x22F2u); // duplicate empty epoch B
    put_u16(0x08u, 0xDF0Bu); // handler SP 0x2000 -> r15
    put_u16(0x0Au, 0x6F12u); // restore epoch A
    put_u16(0x0Cu, 0xD40Bu); // callback literal 0x3C -> r4
    put_u16(0x0Eu, 0x2F42u); // mutate restored epoch
    put_u16(0x10u, 0x6F22u); // restore stale duplicate epoch B
    put_u16(0x12u, 0x000Bu);
    put_u16(0x14u, 0x0009u);
    put_u32(0x30u, 0x80u);
    put_u32(0x34u, 0x84u);
    put_u32(0x38u, 0x2000u);
    put_u32(0x3Cu, 0x90u);
    put_u32(0x80u, 0u);
    put_u32(0x84u, 0u);
    put_u16(0x90u, 0x000Bu);
    put_u16(0x92u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".duplicate-saved-stack-epoch-restore-then-callback",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{
            {0x00u, 0x16u},
            {0x90u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
suspended_stack_slot_saved_epoch_reload_values() {
    std::vector<std::uint8_t> bytes(0x84u, 0x09u);
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

    put_u16(0x00u, 0x2FF2u); // current [sp] = empty saved r15 epoch
    put_u16(0x02u, 0x62F2u); // retain a separate exact alias in r2
    put_u16(0x04u, 0xD10Au); // handler SP literal 0x30 -> r1
    put_u16(0x06u, 0x6F13u); // switch away from the captured stack
    put_u16(0x08u, 0x6F23u); // restore it through the separate alias
    put_u16(0x0Au, 0x63F2u); // reload the now source-top stack slot
    put_u16(0x0Cu, 0x6F13u); // switch away a second time
    put_u16(0x0Eu, 0x6F33u); // restore through the reloaded alias
    put_u16(0x10u, 0xD408u); // callback literal 0x34 -> r4
    put_u16(0x12u, 0x2F42u); // mutate the restored epoch
    put_u16(0x14u, 0x000Bu);
    put_u16(0x16u, 0x0009u);
    put_u32(0x30u, 0x2000u);
    put_u32(0x34u, 0x80u);
    put_u16(0x80u, 0x000Bu);
    put_u16(0x82u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".suspended-stack-slot-saved-epoch-reload",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{
            {0x00u, 0x18u},
            {0x80u, 0x04u},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries);
}

katana::analysis::FunctionValueAnalysisResult
multi_callee_memory_saved_stack_alias_values() {
    std::vector<std::uint8_t> bytes(0xA4u, 0x09u);
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

    put_u16(0x00u, 0x410Bu); // incomplete guarded callees 0x40/0x60
    put_u16(0x02u, 0x0009u);
    put_u16(0x04u, 0xD406u); // callback literal 0x20 -> r4
    put_u16(0x06u, 0x2F42u); // mutate possible captured epoch
    put_u16(0x08u, 0xD106u); // saved-SP cell 0x80 -> r1
    put_u16(0x0Au, 0x6F12u); // restore possible stale epoch
    put_u16(0x0Cu, 0x000Bu);
    put_u16(0x0Eu, 0x0009u);
    put_u32(0x20u, 0xA0u);
    put_u32(0x24u, 0x80u);

    put_u16(0x40u, 0xD10Bu); // saved-SP cell literal 0x70 -> r1
    put_u16(0x42u, 0x21F2u); // only callee A captures empty epoch
    put_u16(0x44u, 0x000Bu);
    put_u16(0x46u, 0x0009u);
    put_u16(0x60u, 0x000Bu); // callee B does not touch the cell
    put_u16(0x62u, 0x0009u);
    put_u32(0x70u, 0x80u);
    put_u32(0x80u, 0u);
    put_u16(0xA0u, 0x000Bu);
    put_u16(0xA2u, 0x0009u);

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(
        katana::io::GuestCallAbi::SuperHC);
    image.add_segment(
        {".multi-callee-memory-saved-stack-alias",
         0u,
         0u,
         bytes.size(),
         katana::io::SegmentKind::Mixed,
         {true, false, true},
         bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 4u>
        boundaries{{
            {0x00u, 0x10u},
            {0x40u, 0x08u},
            {0x60u, 0x04u},
            {0xA0u, 0x04u},
        }};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 2u>
        edges{{
            {0x00u,
             0x40u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             false},
            {0x00u,
             0x60u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             false},
        }};
    return katana::analysis::analyze_function_values(
        image, lines, boundaries, edges);
}

void verify_delta_staging_and_cold_replacement_contract() {
    std::vector<std::uint8_t> bytes(0x20u, 0x09u);
    bytes[0x00u] = 0x2Bu; // jmp @r1
    bytes[0x01u] = 0x41u;
    bytes[0x02u] = 0x09u; // delay nop
    bytes[0x03u] = 0x00u;
    bytes[0x10u] = 0x0Bu; // rts
    bytes[0x11u] = 0x00u;
    bytes[0x12u] = 0x09u;
    bytes[0x13u] = 0x00u;

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".delta-staging-contract",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0x00u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 2u>
        boundaries{{{0x00u, 0x04u}, {0x10u, 0x04u}}};
    using Session =
        katana::analysis::detail::FunctionValueAnalysisSession;
    using Delta = katana::analysis::detail::FunctionProgramDelta;

    const auto unbound_delta = [&] {
        Delta delta;
        delta.kind =
            katana::analysis::detail::FunctionProgramDeltaKind::Exact;
        delta.result_materialization =
            katana::analysis::FunctionValueResultMaterialization::DeltaOnly;
        delta.image_identity = image.analysis_instance_identity();
        delta.image_revision = image.analysis_revision();
        return delta;
    };

    Session duplicate_delta_session;
    duplicate_delta_session.stage_next_function_program_delta(
        unbound_delta());
    bool duplicate_delta_rejected = false;
    try {
        duplicate_delta_session.stage_next_function_program_delta(
            unbound_delta());
    } catch (const std::logic_error&) {
        duplicate_delta_rejected = true;
    }
    Session duplicate_bypass_session;
    duplicate_bypass_session.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    bool duplicate_bypass_rejected = false;
    try {
        duplicate_bypass_session
            .bypass_all_persistent_analysis_state_once(
                katana::analysis::
                    PersistentAnalysisBypassReason::ExplicitTest);
    } catch (const std::logic_error&) {
        duplicate_bypass_rejected = true;
    }
    Session delta_then_bypass;
    delta_then_bypass.stage_next_function_program_delta(unbound_delta());
    delta_then_bypass.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    Session bypass_then_delta;
    bypass_then_delta.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    bypass_then_delta.stage_next_function_program_delta(unbound_delta());
    Session empty_invocation_session;
    empty_invocation_session.stage_next_function_program_delta(
        unbound_delta());
    katana::analysis::detail::GuardedNativeEntryShapeCache empty_shapes{
        image};
    static_cast<void>(katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            std::span<const katana::sh4::DisassemblyLine>{},
            std::span<const katana::analysis::FunctionBoundary>{},
            std::span<const katana::analysis::ResolvedControlFlowEdge>{},
            {},
            empty_shapes,
            empty_invocation_session));
    bool empty_invocation_preserved_delta = false;
    try {
        empty_invocation_session.stage_next_function_program_delta(
            unbound_delta());
    } catch (const std::logic_error&) {
        empty_invocation_preserved_delta = true;
    }
    Session empty_bypass_session;
    empty_bypass_session.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    katana::analysis::detail::GuardedNativeEntryShapeCache
        empty_bypass_shapes{image};
    static_cast<void>(katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            std::span<const katana::sh4::DisassemblyLine>{},
            std::span<const katana::analysis::FunctionBoundary>{},
            std::span<const katana::analysis::ResolvedControlFlowEdge>{},
            {},
            empty_bypass_shapes,
            empty_bypass_session));
    bool empty_invocation_preserved_bypass = false;
    try {
        empty_bypass_session.bypass_all_persistent_analysis_state_once(
            katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    } catch (const std::logic_error&) {
        empty_invocation_preserved_bypass = true;
    }
    require(duplicate_delta_rejected && duplicate_bypass_rejected,
            "Delta-/Bypass-One-Shot-Slots akzeptierten stilles "
            "Same-Kind-Ueberschreiben oder verboten Mischstaging.");
    require(empty_invocation_preserved_delta &&
                empty_invocation_preserved_bypass,
            "Eine leere Nichtanalyse konsumierte Delta oder Bypass.");

    Session first_delta_session;
    Delta first_delta;
    first_delta.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Unknown;
    first_delta.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::DeltaOnly;
    first_delta.image_identity = image.analysis_instance_identity();
    first_delta.image_revision = image.analysis_revision();
    first_delta_session.stage_next_function_program_delta(
        std::move(first_delta));
    katana::analysis::detail::GuardedNativeEntryShapeCache
        first_delta_shapes{image};
    const auto first_delta_result = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image, lines, boundaries, {}, {}, first_delta_shapes,
            first_delta_session);
    require(
        first_delta_result.result_materialization ==
            katana::analysis::FunctionValueResultMaterialization::DeltaOnly,
        "Eine erste Cold/Unknown-FVA-Runde verlor ihren DeltaOnly-Vertrag.");

    Session session;
    katana::analysis::detail::GuardedNativeEntryShapeCache cold_shapes{image};
    const auto cold = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image, lines, boundaries, {}, {}, cold_shapes, session);
    require(!cold.budget_exhausted &&
                session.published_epoch_version() != 0u,
            "Die Staging-Regression erhielt keine kalte Baseline-Epoch.");

    auto cold_delta = unbound_delta();
    cold_delta.expected_published_epoch_version =
        session.published_epoch_version();
    session.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
    session.stage_next_function_program_delta(std::move(cold_delta));
    katana::analysis::detail::GuardedNativeEntryShapeCache bypass_shapes{
        image};
    const auto replacement = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image, lines, boundaries, {}, {}, bypass_shapes, session);
    std::set<std::uint32_t> summary_owners;
    for (const auto& shard : replacement.summary_replacements) {
        require(shard.owner.kind ==
                    katana::analysis::FunctionValueDependencyNodeKind::
                        Function,
                "Cold-Replacement emittierte einen untypisierten Summary-Owner.");
        summary_owners.insert(shard.owner.address);
    }
    const auto has_baseline_inventory = std::any_of(
        replacement.guarded_code_inventory_replacements.begin(),
        replacement.guarded_code_inventory_replacements.end(),
        [](const auto& shard) {
            return shard.owner.kind ==
                   katana::analysis::FunctionValueDependencyNodeKind::
                       AnalysisBaseline;
        });
    const auto has_function_inventory = std::any_of(
        replacement.guarded_code_inventory_replacements.begin(),
        replacement.guarded_code_inventory_replacements.end(),
        [](const auto& shard) {
            return shard.owner.address == 0x00u &&
                   shard.owner.kind ==
                       katana::analysis::FunctionValueDependencyNodeKind::
                           Function;
        });
    const auto has_function_resolution = std::any_of(
        replacement.resolution_replacements.begin(),
        replacement.resolution_replacements.end(),
        [](const auto& shard) {
            return shard.owner.address == 0x00u &&
                   shard.owner.kind ==
                       katana::analysis::FunctionValueDependencyNodeKind::
                           Function;
        });
    require(
        replacement.result_materialization ==
                katana::analysis::FunctionValueResultMaterialization::
                    DeltaOnly &&
            replacement.persistent_analysis_bypass_reason ==
                katana::analysis::PersistentAnalysisBypassReason::
                    ExplicitTest &&
            replacement.summaries.empty() &&
            replacement.resolutions.empty() &&
            replacement.guarded_code_inventory.stored_code_addresses.empty() &&
            replacement.guarded_code_inventory
                .returned_code_address_tables.empty() &&
            summary_owners == std::set<std::uint32_t>{0x00u, 0x10u} &&
            has_baseline_inventory && has_function_inventory &&
            has_function_resolution,
        "Ein returned Bypass war kein vollstaendiger owner-keyed "
        "Cold-Replacement-Vertrag oder materialisierte flache DeltaOnly-"
        "Ausgaben.");

    const auto semantic_epoch = session.published_epoch_version();
    Delta terminal;
    terminal.kind =
        katana::analysis::detail::FunctionProgramDeltaKind::Unchanged;
    terminal.result_materialization =
        katana::analysis::FunctionValueResultMaterialization::TerminalFull;
    terminal.expected_published_epoch_version = semantic_epoch;
    terminal.image_identity = image.analysis_instance_identity();
    terminal.image_revision = image.analysis_revision();
    session.stage_next_function_program_delta(std::move(terminal));
    std::string terminal_phase;
    katana::analysis::detail::GuardedNativeEntryShapeCache terminal_shapes{
        image};
    const auto materialized = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image,
            lines,
            boundaries,
            {},
            [&](const auto& progress) { terminal_phase = progress.phase; },
            terminal_shapes,
            session);
    require(
        terminal_phase == "terminal-materialized" &&
            materialized.result_materialization ==
                katana::analysis::FunctionValueResultMaterialization::
                    TerminalFull &&
            !materialized.summaries.empty() &&
            materialized.summary_replacements.empty() &&
            materialized.resolution_replacements.empty() &&
            materialized.guarded_code_inventory_replacements.empty() &&
            materialized.final_materialized_functions ==
                materialized.summaries.size() &&
            materialized.final_materialized_blocks != 0u &&
            session.published_epoch_version() == semantic_epoch,
        "TerminalFull materialisierte nicht exakt einmal aus derselben "
        "Epoch oder behielt DeltaOnly-Ledger.");

    // Both one-shot slots were consumed by the real invocation.
    auto consumed_delta = unbound_delta();
    consumed_delta.expected_published_epoch_version = semantic_epoch;
    session.stage_next_function_program_delta(std::move(consumed_delta));
    session.bypass_all_persistent_analysis_state_once(
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest);
}

void verify_candidate_derived_topology_add_replace_withdrawal() {
    std::vector<std::uint8_t> bytes(0x30u, 0x09u);
    bytes[0x00u] = 0x0Bu; // jsr @r1
    bytes[0x01u] = 0x41u;
    bytes[0x02u] = 0x09u; // delay nop
    bytes[0x03u] = 0x00u;
    bytes[0x04u] = 0x0Bu; // rts
    bytes[0x05u] = 0x00u;
    bytes[0x06u] = 0x09u;
    bytes[0x07u] = 0x00u;
    bytes[0x10u] = 0x0Bu;
    bytes[0x11u] = 0x00u;
    bytes[0x12u] = 0x09u;
    bytes[0x13u] = 0x00u;
    bytes[0x20u] = 0x0Bu;
    bytes[0x21u] = 0x00u;
    bytes[0x22u] = 0x09u;
    bytes[0x23u] = 0x00u;

    katana::io::ExecutableImage image;
    image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    image.add_segment({".candidate-derived-topology",
                       0u,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       bytes});
    image.add_entry_point(0u);
    const auto lines = katana::sh4::disassemble(bytes, 0u);
    constexpr std::array<katana::analysis::FunctionBoundary, 3u>
        boundaries{{{0x00u, 0x08u},
                    {0x10u, 0x04u},
                    {0x20u, 0x04u}}};
    using Session =
        katana::analysis::detail::FunctionValueAnalysisSession;
    using Delta = katana::analysis::detail::FunctionProgramDelta;
    using Edge = katana::analysis::ResolvedControlFlowEdge;

    const auto candidate = [](const std::uint32_t target) {
        Edge edge;
        edge.instruction_address = 0u;
        edge.target_address = target;
        edge.kind = katana::analysis::ResolvedControlFlowKind::Call;
        edge.guarded = true;
        edge.evidence =
            katana::analysis::ControlFlowEvidence::GuardedPartial;
        edge.analysis_candidate_carrier = true;
        return edge;
    };
    const auto analyze_fresh = [&](const std::span<const Edge> edges) {
        return katana::analysis::analyze_function_values(
            image, lines, boundaries, edges);
    };

    Session session;
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{image};
    const auto baseline = katana::analysis::detail::
        analyze_function_values_with_guarded_entry_cache(
            image, lines, boundaries, {}, {}, shapes, session);
    require(!baseline.budget_exhausted,
            "Candidate-DerivedTopology erhielt keine Cold-Baseline.");

    const auto run_delta = [&](const std::vector<Edge>& family,
                               const std::string_view description) {
        Delta delta;
        delta.kind =
            katana::analysis::detail::FunctionProgramDeltaKind::Exact;
        delta.result_materialization =
            katana::analysis::FunctionValueResultMaterialization::DeltaOnly;
        delta.expected_published_epoch_version =
            session.published_epoch_version();
        delta.image_identity = image.analysis_instance_identity();
        delta.image_revision = image.analysis_revision();
        delta.changed_candidate_call_sites.push_back(
            {0u, family});
        session.stage_next_function_program_delta(std::move(delta));
        const auto incremental = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                std::span<const katana::sh4::DisassemblyLine>{},
                std::span<const katana::analysis::FunctionBoundary>{},
                std::span<const Edge>{},
                {},
                shapes,
                session);
        require(
            incremental.result_materialization ==
                    katana::analysis::FunctionValueResultMaterialization::
                        DeltaOnly &&
                incremental.persistent_analysis_bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::None &&
                incremental.full_cpu_recompute_fallbacks == 0u &&
                incremental.function_edge_full_scans == 0u &&
                incremental.function_edge_full_sorts == 0u &&
                incremental.candidate_call_edge_full_scans == 0u &&
                incremental.candidate_call_edge_full_sorts == 0u &&
                incremental.candidate_tail_edge_full_scans == 0u &&
                incremental.candidate_tail_edge_full_sorts == 0u &&
                incremental.inventory_topology_entries_visited != 0u &&
                incremental.abi_contract_entries_visited != 0u &&
                incremental.summary_candidate_entries_visited != 0u &&
                incremental.resolution_preparation_entries_visited != 0u,
            std::string{description} +
                " nahm keinen echten fallbackfreien DerivedTopology-Warmpfad.");

        Delta terminal;
        terminal.kind =
            katana::analysis::detail::FunctionProgramDeltaKind::Unchanged;
        terminal.result_materialization =
            katana::analysis::FunctionValueResultMaterialization::TerminalFull;
        terminal.expected_published_epoch_version =
            session.published_epoch_version();
        terminal.image_identity = image.analysis_instance_identity();
        terminal.image_revision = image.analysis_revision();
        session.stage_next_function_program_delta(std::move(terminal));
        const auto materialized = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                std::span<const katana::sh4::DisassemblyLine>{},
                std::span<const katana::analysis::FunctionBoundary>{},
                std::span<const Edge>{},
                {},
                shapes,
                session);
        const auto fresh = analyze_fresh(family);
        require_same_function_value_semantics(
            fresh, materialized, false);
    };

    run_delta(std::vector<Edge>{candidate(0x10u)},
              "Candidate-Addition");
    run_delta(std::vector<Edge>{candidate(0x20u)},
              "Candidate-Replacement");
    run_delta({}, "Candidate-Withdrawal");
}

void verify_boundary_overlay_fallback_contracts() {
    using Boundary = katana::analysis::FunctionBoundary;
    using Delta = katana::analysis::detail::FunctionProgramDelta;
    using Session =
        katana::analysis::detail::FunctionValueAnalysisSession;
    const auto make_image = [](std::vector<std::uint8_t> bytes,
                               const std::string& name) {
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.add_segment({name,
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           std::move(bytes)});
        image.add_entry_point(0u);
        return image;
    };
    const auto nop_bytes = [] {
        std::vector<std::uint8_t> bytes(0x20u, 0u);
        for (std::size_t offset = 0u; offset < bytes.size(); offset += 2u)
            bytes[offset] = 0x09u;
        return bytes;
    };
    const auto verify_fallback = [&](const katana::io::ExecutableImage& image,
                                     const std::span<const Boundary> initial,
                                     const std::span<const Boundary> current,
                                     const Boundary added,
                                     const std::string_view description) {
        const auto lines = katana::sh4::disassemble(
            image.segments().front().bytes, 0u);
        Session session;
        katana::analysis::detail::GuardedNativeEntryShapeCache
            initial_shapes{image};
        const auto baseline = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image, lines, initial, {}, {}, initial_shapes, session);
        require(!baseline.budget_exhausted &&
                    session.published_epoch_version() != 0u,
                "Die Boundary-Fallback-Regression erhielt keine Baseline.");

        Delta delta;
        delta.kind =
            katana::analysis::detail::FunctionProgramDeltaKind::Exact;
        delta.result_materialization =
            katana::analysis::FunctionValueResultMaterialization::TerminalFull;
        delta.expected_published_epoch_version =
            session.published_epoch_version();
        delta.image_identity = image.analysis_instance_identity();
        delta.image_revision = image.analysis_revision();
        delta.changed_boundaries.push_back(
            {added.entry_address, added});
        session.stage_next_function_program_delta(std::move(delta));
        katana::analysis::detail::GuardedNativeEntryShapeCache
            incremental_shapes{image};
        const auto incremental = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image,
                std::span<const katana::sh4::DisassemblyLine>{},
                std::span<const Boundary>{},
                std::span<const katana::analysis::ResolvedControlFlowEdge>{},
                {},
                incremental_shapes,
                session);
        katana::analysis::detail::GuardedNativeEntryShapeCache cold_shapes{
            image};
        Session cold_session;
        const auto cold = katana::analysis::detail::
            analyze_function_values_with_guarded_entry_cache(
                image, lines, current, {}, {}, cold_shapes, cold_session);
        require_same_function_value_semantics(cold, incremental, false);
        require(
            incremental.persistent_analysis_bypass_reason ==
                    katana::analysis::PersistentAnalysisBypassReason::
                        ProgramDeltaUnrepresentable &&
                incremental.full_cpu_recompute_fallbacks == 1u,
            std::string{description} +
                " wurde nicht fail-closed exakt kalt neu aufgebaut.");
    };

    const auto split_image =
        make_image(nop_bytes(), ".boundary-end-inside-block");
    constexpr std::array split_initial{Boundary{0u, 0x10u}};
    constexpr std::array split_current{
        Boundary{0u, 0x10u}, Boundary{0x10u, 0x02u}};
    verify_fallback(split_image,
                    split_initial,
                    split_current,
                    split_current[1],
                    "Ein Boundary-Ende innerhalb eines alten Blocks");

    auto tail_bytes = nop_bytes();
    tail_bytes[0x00u] = 0x06u; // bra 0x10
    tail_bytes[0x01u] = 0xA0u;
    tail_bytes[0x10u] = 0x0Bu; // rts
    tail_bytes[0x11u] = 0x00u;
    const auto tail_image =
        make_image(std::move(tail_bytes), ".tail-owner-boundary");
    constexpr std::array tail_initial{Boundary{0u, 0x04u}};
    constexpr std::array tail_current{
        Boundary{0u, 0x04u}, Boundary{0x10u, 0x04u}};
    verify_fallback(tail_image,
                    tail_initial,
                    tail_current,
                    tail_current[1],
                    "Ein neuer Function-Entry mit altem Tail-Owner");
}

void verify_contextual_semantic_lane_selector() {
    // This narrow selector deliberately reuses the existing contextual
    // contracts. It is not a second fixture matrix: each case continues to
    // validate the semantic output it already defines, while KR-4986 shares
    // only its internal full-state producer work.
    const auto duplicate = duplicate_forwarded_context_values();
    const auto duplicate_candidate = std::find_if(
        duplicate.values.guarded_code_inventory.stored_code_addresses
            .begin(),
        duplicate.values.guarded_code_inventory.stored_code_addresses
            .end(),
        [](const auto& candidate) {
            return candidate.target_address == 0x70u;
        });
    require(
        duplicate_candidate !=
                duplicate.values.guarded_code_inventory
                    .stored_code_addresses.end() &&
            duplicate_candidate->store_instruction_addresses ==
                std::vector<std::uint32_t>{0x20u} &&
            duplicate_candidate->evidence_call_sites ==
                std::vector<std::uint32_t>{0x02u, 0x12u} &&
            !duplicate.values.guarded_code_inventory.walk_diagnostics
                 .truncated(),
        "Der KR-4986-Selector verlor den bestehenden Duplicate-"
        "Forwarded-Context-Vertrag.");

    const auto multi_owner = multi_owner_contextual_return_values();
    const auto* const multi_owner_table_a =
        returned_table_candidate(multi_owner.values, 0x90u);
    const auto* const multi_owner_table_b =
        returned_table_candidate(multi_owner.values, 0x98u);
    require(
        multi_owner_table_a != nullptr &&
            multi_owner_table_a->target_addresses ==
                std::vector<std::uint32_t>{0xC0u, 0xD0u} &&
            multi_owner_table_b != nullptr &&
            multi_owner_table_b->target_addresses ==
                std::vector<std::uint32_t>{0xD0u} &&
            !multi_owner.values.guarded_code_inventory.walk_diagnostics
                 .truncated(),
        "Der KR-4986-Selector verlor den bestehenden Multi-Owner-"
        "Contextual-Return-Vertrag.");

    const auto contextual_budget =
        contextual_read_contract_and_fixpoint_budget_values();
    const auto* const contextual_budget_table =
        returned_table_candidate(contextual_budget, 0x80u);
    const auto& contextual_budget_diagnostics =
        contextual_budget.guarded_code_inventory.walk_diagnostics;
    require(
        contextual_budget_table != nullptr &&
            contextual_budget_table->target_addresses ==
                std::vector<std::uint32_t>{0x90u} &&
            !contextual_budget_diagnostics
                 .contextual_return_context_limited_functions &&
            !contextual_budget_diagnostics
                 .contextual_return_evaluation_limited_functions &&
            !contextual_budget.budget_exhausted,
        "Der KR-4986-Selector verlor den bestehenden Contextual-"
        "Read-/Budget-Vertrag.");

    const auto stale_reference = contextual_stale_error_values();
    verify_contextual_stale_error_regression(stale_reference);
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc > 1) {
        if (argc != 2) {
            std::cerr
                << "Unbekannter Testschalter; erwartet wird genau "
                << "--only=contextual-stale-error oder "
                << "--only=contextual-semantic-lanes.\n";
            return EXIT_FAILURE;
        }
        try {
            const auto selector = std::string_view{argv[1]};
            if (selector == "--only=contextual-stale-error") {
                const auto reference = contextual_stale_error_values();
                verify_contextual_stale_error_regression(reference);
                std::cout
                    << "KR-4985 Contextual-Stale-Error erfolgreich.\n";
            } else if (selector == "--only=contextual-semantic-lanes") {
                verify_contextual_semantic_lane_selector();
                std::cout
                    << "KR-4986 Contextual-Semantic-Lanes erfolgreich.\n";
            } else {
                std::cerr
                    << "Unbekannter Testschalter; erwartet wird genau "
                    << "--only=contextual-stale-error oder "
                    << "--only=contextual-semantic-lanes.\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        } catch (const std::exception& error) {
            std::cerr << "Contextual-Selector-Ausnahme: "
                      << error.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    verify_persistent_function_value_session();
    verify_incremental_resolution_root_reuse();
    verify_delta_staging_and_cold_replacement_contract();
    verify_candidate_derived_topology_add_replace_withdrawal();
    verify_boundary_overlay_fallback_contracts();
    verify_inventory_region_dependency_reuse();
    verify_typed_interfunction_tail_transport();
    verify_unresolved_tail_target_is_fail_closed();
    verify_function_evaluation_cache_telemetry();

    {
        katana::analysis::ParallelWorkExecutor executor(2u);
        std::atomic_size_t outer_count = 0u;
        std::atomic_size_t inner_count = 0u;
        katana::analysis::parallel_analysis_for(
            executor,
            8u,
            2u,
            [&](const std::size_t) {
                outer_count.fetch_add(1u, std::memory_order_relaxed);
                katana::analysis::parallel_analysis_for(
                    executor,
                    3u,
                    2u,
                    [&](const std::size_t) {
                        inner_count.fetch_add(
                            1u, std::memory_order_relaxed);
                    });
            });
        require(
            outer_count.load(std::memory_order_relaxed) == 8u &&
                inner_count.load(std::memory_order_relaxed) == 24u,
            "Der globale Analysis-Executor verlor verschachtelte Arbeit "
            "oder blockierte.");
        std::barrier simultaneous_nested_barrier(2);
        std::atomic_size_t simultaneous_nested_items = 0u;
        katana::analysis::parallel_analysis_for(
            executor,
            2u,
            2u,
            [&](const std::size_t) {
                simultaneous_nested_barrier.arrive_and_wait();
                katana::analysis::parallel_analysis_for(
                    executor,
                    4u,
                    2u,
                    [&](const std::size_t) {
                        simultaneous_nested_items.fetch_add(
                            1u, std::memory_order_relaxed);
                    });
            });
        require(
            simultaneous_nested_items.load(
                std::memory_order_relaxed) == 8u,
            "Der Analysis-Executor deadlockt, wenn alle Worker gleichzeitig "
            "verschachtelte Batches starten.");

        bool exception_propagated = false;
        std::atomic_size_t exception_batch_items = 0u;
        try {
            katana::analysis::parallel_analysis_for(
                executor,
                6u,
                2u,
                [&](const std::size_t index) {
                    exception_batch_items.fetch_add(
                        1u, std::memory_order_relaxed);
                    if (index == 1u)
                        throw std::runtime_error(
                            "parallel-analysis-first");
                    if (index == 3u)
                        throw std::runtime_error(
                            "parallel-analysis-later");
                });
        } catch (const std::runtime_error& error) {
            exception_propagated =
                std::string(error.what()) ==
                "parallel-analysis-first";
        }
        require(
            exception_propagated &&
                exception_batch_items.load(
                    std::memory_order_relaxed) == 6u,
            "Der Analysis-Executor verlor die indexerste Worker-Ausnahme "
            "oder brach den restlichen Batch vorzeitig ab.");
        std::atomic_size_t recovery_items = 0u;
        katana::analysis::parallel_analysis_for(
            executor,
            5u,
            2u,
            [&](const std::size_t) {
                recovery_items.fetch_add(
                    1u, std::memory_order_relaxed);
            });
        require(
            recovery_items.load(std::memory_order_relaxed) == 5u,
            "Der Analysis-Executor blieb nach einer Worker-Ausnahme defekt.");

        std::atomic_size_t roots_ready = 0u;
        std::atomic_size_t active_jobs = 0u;
        std::atomic_size_t peak_jobs = 0u;
        std::atomic_bool start_roots = false;
        const auto root = [&] {
            roots_ready.fetch_add(1u, std::memory_order_release);
            while (!start_roots.load(std::memory_order_acquire))
                std::this_thread::yield();
            katana::analysis::parallel_analysis_for(
                executor,
                6u,
                2u,
                [&](const std::size_t) {
                    const auto active =
                        active_jobs.fetch_add(
                            1u, std::memory_order_acq_rel) +
                        1u;
                    auto peak =
                        peak_jobs.load(std::memory_order_relaxed);
                    while (peak < active &&
                           !peak_jobs.compare_exchange_weak(
                               peak,
                               active,
                               std::memory_order_relaxed))
                        ;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                    active_jobs.fetch_sub(
                        1u, std::memory_order_acq_rel);
                });
        };
        auto first_root =
            std::async(std::launch::async, root);
        auto second_root =
            std::async(std::launch::async, root);
        while (roots_ready.load(std::memory_order_acquire) != 2u)
            std::this_thread::yield();
        start_roots.store(true, std::memory_order_release);
        first_root.get();
        second_root.get();
        require(
            peak_jobs.load(std::memory_order_relaxed) <= 2u &&
                active_jobs.load(std::memory_order_relaxed) == 0u,
            "Der process-weite Analysis-Executor liess zwei "
            "Root-Aufrufer zusammen mehr als zwei schwere Jobs ausfuehren.");

        std::mutex worker_ids_mutex;
        std::set<std::thread::id> initial_worker_ids;
        std::barrier initial_worker_barrier(2);
        katana::analysis::parallel_analysis_for(
            executor,
            2u,
            2u,
            [&](const std::size_t) {
                {
                    std::lock_guard lock(worker_ids_mutex);
                    initial_worker_ids.insert(
                        std::this_thread::get_id());
                }
                initial_worker_barrier.arrive_and_wait();
            });
        std::set<std::thread::id> later_worker_ids;
        for (std::size_t round = 0u; round < 32u; ++round) {
            katana::analysis::parallel_analysis_for(
                executor,
                8u,
                2u,
                [&](const std::size_t) {
                    std::lock_guard lock(worker_ids_mutex);
                    later_worker_ids.insert(
                        std::this_thread::get_id());
                });
        }
        require(
            executor.worker_count() == 2u &&
                initial_worker_ids.size() == 2u &&
                std::all_of(
                    later_worker_ids.begin(),
                    later_worker_ids.end(),
                    [&](const auto id) {
                        return initial_worker_ids.contains(id);
                    }),
            "Der Analysis-Executor erzeugt zwischen Batches neue Worker "
            "statt seinen festen Pool wiederzuverwenden.");
    }
    {
        katana::analysis::ParallelWorkExecutor priority_executor(1u);
        std::mutex order_mutex;
        std::vector<std::size_t> dispatch_order;
        katana::analysis::AnalysisWorkDescriptor throughput;
        throughput.phase =
            katana::analysis::AnalysisWorkPhase::FunctionValue;
        throughput.subject_kind =
            katana::analysis::AnalysisWorkSubjectKind::Root;
        throughput.priority =
            katana::analysis::AnalysisWorkPriorityKind::Throughput;
        throughput.quantum = 1u;
        katana::analysis::parallel_analysis_for(
            priority_executor,
            throughput,
            5u,
            1u,
            nullptr,
            [&](const std::size_t index) {
                {
                    const std::lock_guard lock(order_mutex);
                    dispatch_order.push_back(index);
                }
                if (index != 0u) return;
                katana::analysis::AnalysisWorkDescriptor critical;
                critical.phase =
                    katana::analysis::AnalysisWorkPhase::Resolution;
                critical.subject_kind =
                    katana::analysis::AnalysisWorkSubjectKind::Root;
                critical.priority =
                    katana::analysis::AnalysisWorkPriorityKind::
                        CriticalPrefix;
                critical.critical_prefix = 0u;
                critical.quantum = 1u;
                priority_executor.submit_once(
                    std::move(critical),
                    [&] {
                        const std::lock_guard lock(order_mutex);
                        dispatch_order.push_back(99u);
                    });
            });
        require(
            dispatch_order ==
                std::vector<std::size_t>{0u, 99u, 1u, 2u, 3u, 4u},
            "Ein grober Throughput-Drain verschluckte weiterhin kritische "
            "Nested-Arbeit oder seine begrenzte Continuation.");
    }
    {
        katana::analysis::AnalysisMemoryBudget memory_budget{64u};
        katana::analysis::ParallelWorkExecutor memory_executor(
            2u, memory_budget);
        katana::analysis::AnalysisWorkDescriptor memory_bound;
        memory_bound.phase =
            katana::analysis::AnalysisWorkPhase::FunctionValue;
        memory_bound.priority =
            katana::analysis::AnalysisWorkPriorityKind::Unblocking;
        memory_bound.transient_bytes = 48u;
        memory_bound.quantum = 1u;
        std::atomic_size_t entered = 0u;
        std::atomic_size_t completed = 0u;
        std::atomic_bool release = false;
        auto memory_run = std::async(
            std::launch::async,
            [&] {
                katana::analysis::parallel_analysis_for(
                    memory_executor,
                    memory_bound,
                    2u,
                    2u,
                    nullptr,
                    [&](const std::size_t) {
                        entered.fetch_add(
                            1u, std::memory_order_release);
                        while (!release.load(
                            std::memory_order_acquire))
                            std::this_thread::yield();
                        completed.fetch_add(
                            1u, std::memory_order_relaxed);
                    });
            });
        const auto memory_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds{2};
        katana::analysis::ParallelWorkExecutorSnapshot blocked;
        do {
            blocked = memory_executor.snapshot();
            if (entered.load(std::memory_order_acquire) == 1u &&
                blocked.memory_blocked != 0u)
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < memory_deadline);
        const bool budget_block_visible =
            entered.load(std::memory_order_acquire) == 1u &&
            blocked.running == 1u && blocked.queued != 0u &&
            blocked.memory_blocked != 0u &&
            blocked.memory_used == 48u &&
            blocked.memory_capacity == 64u;
        release.store(true, std::memory_order_release);
        memory_run.get();
        const auto quiescent = memory_executor.snapshot();
        bool oversize_rejected = false;
        try {
            auto oversize = memory_bound;
            oversize.transient_bytes = 65u;
            memory_executor.submit_once(
                std::move(oversize), [] {});
        } catch (const katana::analysis::AnalysisMemoryBudgetExceeded&
                     error) {
            oversize_rejected =
                error.requested() == 65u && error.capacity() == 64u;
        }
        require(
            budget_block_visible && oversize_rejected &&
                completed.load(std::memory_order_relaxed) == 2u &&
                quiescent.running == 0u && quiescent.queued == 0u &&
                quiescent.memory_used == 0u &&
                quiescent.memory_peak == 48u,
            "Das globale Analysis-Speicherbudget liess zwei 48-Byte-Jobs "
            "gleichzeitig in 64 Byte zu, verbarg den Blockzustand oder "
            "lehnte Oversize-Arbeit nicht typisiert ab.");
    }
    {
        katana::analysis::ParallelWorkExecutor utilization_executor(8u);
        std::barrier first_window(8);
        std::barrier release_window(8);
        std::atomic_size_t peak_running = 0u;
        katana::analysis::AnalysisWorkDescriptor work;
        work.phase = katana::analysis::AnalysisWorkPhase::FunctionValue;
        work.priority =
            katana::analysis::AnalysisWorkPriorityKind::Unblocking;
        work.quantum = 1u;
        katana::analysis::parallel_analysis_for(
            utilization_executor,
            work,
            8u,
            8u,
            nullptr,
            [&](const std::size_t) {
                first_window.arrive_and_wait();
                const auto running =
                    utilization_executor.snapshot().running;
                auto peak = peak_running.load(
                    std::memory_order_relaxed);
                while (peak < running &&
                       !peak_running.compare_exchange_weak(
                           peak,
                           running,
                           std::memory_order_relaxed))
                    ;
                release_window.arrive_and_wait();
            });
        require(
            peak_running.load(std::memory_order_relaxed) >= 6u,
            "Ein schweres Executor-Fenster nutzte weniger als 75 Prozent "
            "seiner acht konfigurierten Worker.");
    }
    {
        katana::analysis::ParallelWorkExecutor single_worker(1u);
        katana::analysis::ParallelWorkActivity activity_a;
        katana::analysis::ParallelWorkActivity activity_b;
        std::atomic_size_t nested_items = 0u;
        katana::analysis::parallel_analysis_for(
            single_worker,
            4u,
            1u,
            &activity_a,
            [&](const std::size_t) {
                katana::analysis::parallel_analysis_for(
                    single_worker,
                    3u,
                    1u,
                    &activity_a,
                    [&](const std::size_t) {
                        require(
                            activity_a.active_worker_count() == 1u &&
                                activity_b.active_worker_count() == 0u,
                            "Verschachtelte Arbeit derselben Activity "
                            "wurde doppelt als Worker gezaehlt.");
                        nested_items.fetch_add(
                            1u, std::memory_order_relaxed);
                    });
                require(
                    activity_a.active_worker_count() == 1u &&
                        activity_b.active_worker_count() == 0u,
                    "Die aeussere Activity wurde nach gleichartiger "
                    "Nested-Hilfe nicht wiederhergestellt.");
                katana::analysis::parallel_analysis_for(
                    single_worker,
                    1u,
                    1u,
                    [&](const std::size_t) {
                        require(
                            activity_a.active_worker_count() == 0u &&
                                activity_b.active_worker_count() == 0u,
                            "Ein Null-Domain-Task wurde faelschlich der "
                            "wartenden Activity zugerechnet.");
                    });
                require(
                    activity_a.active_worker_count() == 1u,
                    "Die aeussere Activity wurde nach Null-Domain-Hilfe "
                    "nicht wiederhergestellt.");
                katana::analysis::parallel_analysis_for(
                    single_worker,
                    1u,
                    1u,
                    &activity_b,
                    [&](const std::size_t) {
                        require(
                            activity_a.active_worker_count() == 0u &&
                                activity_b.active_worker_count() == 1u,
                            "Ein Worker wurde beim Activity-Wechsel in "
                            "zwei Domains gleichzeitig gezaehlt.");
                    });
                require(
                    activity_a.active_worker_count() == 1u &&
                        activity_b.active_worker_count() == 0u,
                    "Die aeussere Activity wurde nach fremder Nested-Hilfe "
                    "nicht exakt wiederhergestellt.");
            });
        require(
            nested_items.load(std::memory_order_relaxed) == 12u &&
                activity_a.active_worker_count() == 0u &&
                activity_b.active_worker_count() == 0u &&
                single_worker.active_worker_count() == 0u,
            "Der Analysis-Executor deadlockt oder verliert Nested-Arbeit "
            "mit genau einem Worker beziehungsweise publiziert den "
            "Batchabschluss vor dem Activity-Abbau.");
    }
    {
        katana::analysis::ParallelWorkExecutor four_workers(4u);
        katana::analysis::ParallelWorkActivity local_activity;
        std::atomic_size_t active_jobs = 0u;
        std::atomic_size_t peak_jobs = 0u;
        std::atomic_size_t peak_reported_workers = 0u;
        katana::analysis::parallel_analysis_for(
            four_workers,
            16u,
            2u,
            &local_activity,
            [&](const std::size_t) {
                const auto active =
                    active_jobs.fetch_add(
                        1u, std::memory_order_acq_rel) +
                    1u;
                auto peak =
                    peak_jobs.load(std::memory_order_relaxed);
                while (peak < active &&
                       !peak_jobs.compare_exchange_weak(
                           peak,
                           active,
                           std::memory_order_relaxed))
                    ;
                const auto reported =
                    local_activity.active_worker_count();
                auto reported_peak =
                    peak_reported_workers.load(
                        std::memory_order_relaxed);
                while (reported_peak < reported &&
                       !peak_reported_workers.compare_exchange_weak(
                           reported_peak,
                           reported,
                           std::memory_order_relaxed))
                    ;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                active_jobs.fetch_sub(
                    1u, std::memory_order_acq_rel);
            });
        require(
            peak_jobs.load(std::memory_order_relaxed) <= 2u &&
                active_jobs.load(std::memory_order_relaxed) == 0u &&
                peak_reported_workers.load(
                    std::memory_order_relaxed) >= 1u &&
                peak_reported_workers.load(
                    std::memory_order_relaxed) <= 2u &&
                local_activity.active_worker_count() == 0u &&
                four_workers.active_worker_count() == 0u,
            "Der Analysis-Executor ignoriert das lokale Zwei-Job-Limit "
            "oder meldet verschachtelte Tasks als zusaetzliche Worker.");
    }
    {
        katana::analysis::ParallelWorkExecutor shared_executor(4u);
        katana::analysis::ParallelWorkActivity activity_a;
        katana::analysis::ParallelWorkActivity activity_b;
        std::atomic_size_t started = 0u;
        std::atomic_bool release = false;
        const auto wait_for_release = [&](const std::size_t) {
            started.fetch_add(1u, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        };
        auto first_group = std::async(
            std::launch::async,
            [&] {
                katana::analysis::parallel_analysis_for(
                    shared_executor,
                    1u,
                    1u,
                    &activity_a,
                    wait_for_release);
            });
        auto second_group = std::async(
            std::launch::async,
            [&] {
                katana::analysis::parallel_analysis_for(
                    shared_executor,
                    2u,
                    2u,
                    &activity_b,
                    wait_for_release);
            });
        while (started.load(std::memory_order_acquire) != 3u)
            std::this_thread::yield();
        const bool domains_are_isolated =
            shared_executor.active_worker_count() == 3u &&
            activity_a.active_worker_count() == 1u &&
            activity_b.active_worker_count() == 2u;
        release.store(true, std::memory_order_release);
        first_group.get();
        second_group.get();
        require(
            domains_are_isolated &&
                shared_executor.active_worker_count() == 0u &&
                activity_a.active_worker_count() == 0u &&
                activity_b.active_worker_count() == 0u,
            "Der globale Executor und zwei unabhaengige lokale "
            "Activity-Domains wurden vermischt oder nicht vor dem "
            "Batchabschluss abgebaut.");
    }
    {
        set_stack_diagnostics_for_serial_fixpoint(true);
        const auto serial =
            multi_callee_memory_saved_stack_alias_values();
        set_stack_diagnostics_for_serial_fixpoint(false);
        const auto parallel =
            multi_callee_memory_saved_stack_alias_values();
        require(
            serial.fixpoint_worker_count == 1u,
            "Der erzwungene serielle Differentiallauf benutzte mehr als "
            "einen Fixpoint-Worker.");
        require(
            parallel.fixpoint_worker_count <= 1u ||
                (parallel.maximum_fixpoint_batch_size > 1u &&
                 parallel.fixpoint_parallel_batches > 0u &&
                 parallel.fixpoint_stale_repairs > 0u),
            "Der reale Function-Value-Fixpunkt erreichte trotz "
            "verfuegbarer Mehrworker-Ausfuehrung keinen versionsvalidierten "
            "parallelen Batch mit Stale-Reparatur.");
        require_same_function_value_semantics(
            serial, parallel);
    }

    const auto unique_image =
        image_with_callee({0x10u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u}); // mov #0x10,r0; rts; nop
    const auto unique = katana::analysis::analyze_control_flow(unique_image);
    const auto* unique_site = site(unique, 4u);
    require(unique_site != nullptr &&
                unique_site->status == katana::analysis::ResolutionStatus::Resolved &&
                unique_site->target == 0x10u &&
                unique_site->targets == std::vector<std::uint32_t>{0x10u} &&
                unique_site->evidence_call_sites == std::vector<std::uint32_t>{0u} &&
                unique_site->evidence_callees == std::vector<std::uint32_t>{0x20u},
            "Eindeutiger R0-Return wurde nicht mit Callsite-/Callee-Evidenz aufgeloest.");
    const auto* unique_summary = summary(unique, 0x20u, 0u);
    require(unique_summary != nullptr && unique_summary->complete &&
                unique_summary->values == std::vector<std::uint32_t>{0x10u} &&
                unique_summary->reason == "constant-return",
            "Eindeutige Funktionssummary fehlt oder ist nicht vollstaendig.");
    const auto* preserved = summary(unique, 0x20u, 8u);
    require(preserved != nullptr && preserved->abi_preserved,
            "SH-C-Erhalt von R8 wurde in der Funktionssummary nicht ausgewiesen.");

    {
        std::vector<std::uint8_t> bytes(0x26u, 0x09u);
        bytes[0x00u] = 0x2Bu;
        bytes[0x01u] = 0x41u; // jmp @r1
        bytes[0x02u] = 0x09u;
        bytes[0x03u] = 0x00u;
        bytes[0x20u] = 0x2Au;
        bytes[0x21u] = 0xE0u; // mov #42,r0
        bytes[0x22u] = 0x0Bu;
        bytes[0x23u] = 0x00u;
        bytes[0x24u] = 0x09u;
        bytes[0x25u] = 0x00u;
        auto image = classification_image(bytes);
        const auto lines = katana::sh4::disassemble(bytes, 0u);
        constexpr std::array<std::uint32_t, 1u> function_entries{0u};
        const std::array<katana::analysis::ResolvedControlFlowEdge, 2u> edges{{
            {0x00u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Jump,
             true,
             katana::analysis::ControlFlowEvidence::GuardedComplete,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
            {0x00u,
             0x20u,
             katana::analysis::ResolvedControlFlowKind::Call,
             true,
             katana::analysis::ControlFlowEvidence::GuardedPartial,
             {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot},
             true},
        }};
        const auto values = katana::analysis::analyze_function_values(
            image, lines, function_entries, edges);
        const auto owner =
            std::find_if(values.summaries.begin(),
                         values.summaries.end(),
                         [](const auto& candidate) {
                             return candidate.function_address == 0u;
                         });
        require(owner != values.summaries.end(),
                "Candidate-Carrier entfernte die Owner-Funktionssummary.");
        const auto r0 =
            std::find_if(owner->registers.begin(),
                         owner->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 0u;
                         });
        require(r0 != owner->registers.end() && r0->complete &&
                    r0->values == std::vector<std::uint32_t>{42u},
                "Candidate-Carrier entfernte die reale Jump-Kante mit "
                "identischem Callsite-/Zielpaar.");
    }

    {
        const auto conditional = conditional_shared_tail_values();
        const auto stored =
            std::find_if(
                conditional.guarded_code_inventory.stored_code_addresses.begin(),
                conditional.guarded_code_inventory.stored_code_addresses.end(),
                [](const auto& candidate) {
                    return candidate.target_address == 0x70u;
                });
        require(stored !=
                        conditional.guarded_code_inventory.stored_code_addresses.end() &&
                    stored->guarded &&
                    !conditional.guarded_code_inventory
                         .candidate_inventory_truncated,
                "Bedingter externer Shared-Tail verlor seinen "
                "Callbackstore oder meldete ein falsches vollstaendiges Inventar.");

        const auto tail_cycle = nonisolated_tail_cycle_values();
        require(
            has_stored_code_address(tail_cycle, 0x70u) &&
                !tail_cycle.guarded_code_inventory.walk_diagnostics
                     .forwarded_store_context_limited_functions &&
                !tail_cycle.guarded_code_inventory.walk_diagnostics
                     .inventory_candidate_values_truncated,
            "Der non-isolated Tail-Zyklus konvergierte nicht ueber seinen "
            "kanonisierten Live-in-Zustand oder verlor den Callback.");

        const auto live_tail_contract =
            dead_r3_live_r9_tail_contract_values();
        require(
            has_stored_code_address(live_tail_contract, 0x70u) &&
                !live_tail_contract.guarded_code_inventory.walk_diagnostics
                     .forwarded_store_context_limited_functions,
            "Die Tail-Kanonisierung verlor das live Callee-saved Register r9 "
            "oder konvergierte nach dem sicheren r3-Overwrite nicht.");

        const auto multi_owner = multi_owner_inventory_start_values();
        require(
            has_stored_code_address(multi_owner, 0x70u) &&
                !multi_owner.guarded_code_inventory
                     .candidate_inventory_truncated,
            "Ein Guarded-Inventareinstieg mit mehreren Funktionsownern "
            "wurde verworfen oder faelschlich als trunciert gemeldet.");
        const auto duplicate_forwarded_run =
            duplicate_forwarded_context_values();
        const auto duplicate_forwarded_reference =
            duplicate_forwarded_context_values(
                16'384u,
                1'024u * 1024u * 1024u);
        require_same_function_value_semantics(
            duplicate_forwarded_run.values,
            duplicate_forwarded_reference.values,
            false);
        const auto& duplicate_forwarded =
            duplicate_forwarded_run.values;
        const auto& duplicate_forwarded_progress =
            duplicate_forwarded_run.progress;
        const auto& duplicate_forwarded_session =
            duplicate_forwarded_run.session_statistics;
        const auto& duplicate_forwarded_diagnostics =
            duplicate_forwarded.guarded_code_inventory
                .walk_diagnostics;
        const auto duplicate_forwarded_candidate =
            std::find_if(
                duplicate_forwarded.guarded_code_inventory
                    .stored_code_addresses.begin(),
                duplicate_forwarded.guarded_code_inventory
                    .stored_code_addresses.end(),
                [](const auto& candidate) {
                    return candidate.target_address == 0x70u;
                });
        require(
            duplicate_forwarded_candidate !=
                    duplicate_forwarded.guarded_code_inventory
                        .stored_code_addresses.end() &&
                duplicate_forwarded_candidate->guarded &&
                !duplicate_forwarded_candidate->complete &&
                duplicate_forwarded_candidate
                        ->store_instruction_addresses ==
                    std::vector<std::uint32_t>{0x20u} &&
                duplicate_forwarded_candidate->evidence_call_sites ==
                    std::vector<std::uint32_t>{0x02u, 0x12u} &&
                duplicate_forwarded_candidate->evidence_callees.empty() &&
                duplicate_forwarded_diagnostics
                        .forwarded_store_evaluation_cache_hits ==
                    1u &&
                duplicate_forwarded_diagnostics
                        .forwarded_store_evaluation_cache_misses ==
                    1u &&
                duplicate_forwarded_progress
                        .multi_root_context_requests ==
                    2u &&
                duplicate_forwarded_progress
                        .multi_root_unique_contexts ==
                    1u &&
                duplicate_forwarded_progress
                            .multi_root_ready_reuses +
                        duplicate_forwarded_progress
                            .multi_root_in_flight_reuses ==
                    1u &&
                duplicate_forwarded_progress
                        .multi_root_retained_contexts ==
                    1u &&
                duplicate_forwarded_session.entries == 0u &&
                duplicate_forwarded_session
                        .retained_payload_bytes ==
                    0u &&
                duplicate_forwarded_progress.logical_evaluations ==
                    duplicate_forwarded_progress
                            .session_cache_lookups +
                        duplicate_forwarded_progress
                            .multi_root_ready_reuses +
                        duplicate_forwarded_progress
                            .multi_root_in_flight_reuses &&
                !duplicate_forwarded_diagnostics.truncated(),
            "Zwei semantisch gleiche Forwarded-Kontexte mit verschiedenen "
            "Root-Callsites wurden physisch erneut ausgewertet oder ihre "
            "exakte Provenienz/Fail-closed-Semantik wich beim Replay ab "
            "(candidate=" +
                std::to_string(
                    has_stored_code_address(duplicate_forwarded, 0x70u)) +
                ", hits=" +
                std::to_string(
                    duplicate_forwarded_diagnostics
                        .forwarded_store_evaluation_cache_hits) +
                ", misses=" +
                std::to_string(
                    duplicate_forwarded_diagnostics
                        .forwarded_store_evaluation_cache_misses) +
                ", truncated=" +
                std::to_string(
                    duplicate_forwarded_diagnostics.truncated()) +
                ", guarded=" +
                std::to_string(
                    duplicate_forwarded_candidate !=
                            duplicate_forwarded.guarded_code_inventory
                                .stored_code_addresses.end() &&
                        duplicate_forwarded_candidate->guarded) +
                ", complete=" +
                std::to_string(
                    duplicate_forwarded_candidate !=
                            duplicate_forwarded.guarded_code_inventory
                                .stored_code_addresses.end() &&
                        duplicate_forwarded_candidate->complete) +
                ", stores=" +
                std::to_string(
                    duplicate_forwarded_candidate ==
                            duplicate_forwarded.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : duplicate_forwarded_candidate
                              ->store_instruction_addresses.size()) +
                ", calls=" +
                std::to_string(
                    duplicate_forwarded_candidate ==
                            duplicate_forwarded.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : duplicate_forwarded_candidate
                              ->evidence_call_sites.size()) +
                ", callees=" +
                std::to_string(
                    duplicate_forwarded_candidate ==
                            duplicate_forwarded.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : duplicate_forwarded_candidate
                              ->evidence_callees.size()) +
                ").");

        const auto contextual_stale_error_reference =
            contextual_stale_error_values();
        verify_contextual_stale_error_regression(
            contextual_stale_error_reference);

        const auto duplicate_isolated =
            duplicate_isolated_context_values();
        const auto duplicate_isolated_reference =
            duplicate_isolated_context_values(
                16'384u,
                1'024u * 1024u * 1024u);
        require_same_function_value_semantics(
            duplicate_isolated.values,
            duplicate_isolated_reference.values,
            false);
        const auto& isolated_progress =
            duplicate_isolated.progress;
        const auto& isolated_session =
            duplicate_isolated.session_statistics;
        const auto& isolated_diagnostics =
            duplicate_isolated.values.guarded_code_inventory
                .walk_diagnostics;
        const auto isolated_candidate = std::find_if(
            duplicate_isolated.values.guarded_code_inventory
                .stored_code_addresses.begin(),
            duplicate_isolated.values.guarded_code_inventory
                .stored_code_addresses.end(),
            [](const auto& candidate) {
                return candidate.target_address == 0x70u;
            });
        const auto isolated_reuses =
            isolated_progress.multi_root_ready_reuses +
            isolated_progress.multi_root_in_flight_reuses;
        const auto isolated_lens_index = static_cast<std::size_t>(
            katana::analysis::EvaluationLens::IsolatedObservation);
        require(
            isolated_candidate !=
                    duplicate_isolated.values.guarded_code_inventory
                        .stored_code_addresses.end() &&
                isolated_candidate->guarded &&
                !isolated_candidate->complete &&
                isolated_candidate->store_instruction_addresses ==
                    std::vector<std::uint32_t>{0x20u} &&
                isolated_candidate->evidence_call_sites ==
                    std::vector<std::uint32_t>{0x02u, 0x12u} &&
                isolated_candidate->evidence_callees.empty() &&
                isolated_progress.phase == "complete" &&
                isolated_progress.multi_root_context_requests >= 2u &&
                isolated_progress.multi_root_unique_contexts >= 1u &&
                isolated_reuses >= 1u &&
                isolated_progress.multi_root_context_requests ==
                    isolated_progress.multi_root_unique_contexts +
                        isolated_reuses &&
                isolated_progress.multi_root_retained_contexts ==
                    isolated_progress.multi_root_unique_contexts &&
                isolated_progress.multi_root_retained_payload_bytes != 0u &&
                isolated_progress.multi_root_provenance_links >= 2u &&
                isolated_progress.evaluation_lenses
                        .requests[isolated_lens_index] ==
                    1u &&
                isolated_session.entries == 0u &&
                isolated_session.retained_payload_bytes == 0u &&
                isolated_session.hits == 0u &&
                isolated_session.evictions == 0u &&
                isolated_session.lookups == isolated_session.misses &&
                isolated_progress.logical_evaluations ==
                    isolated_progress.session_cache_lookups +
                        isolated_reuses &&
                isolated_progress.physical_evaluations ==
                    isolated_progress.session_cache_misses +
                        isolated_progress
                            .cache_replay_fallback_recomputes +
                isolated_progress
                            .cache_diagnostic_bypass_evaluations &&
                isolated_progress.cache_replay_fallback_recomputes == 0u &&
                !isolated_diagnostics.truncated(),
            "Initial Isolated Store Harvest fuehrte denselben kanonischen "
            "Kontext trotz cacheloser Session mehrfach aus, verlor echte "
            "Callsite-/Callee-Provenienz oder meldete ein inkonsistentes "
            "Multi-Root-Ledger (requests=" +
                std::to_string(
                    isolated_progress.multi_root_context_requests) +
                ", unique=" +
                std::to_string(
                    isolated_progress.multi_root_unique_contexts) +
                ", reuses=" + std::to_string(isolated_reuses) +
                ", retained=" +
                std::to_string(
                    isolated_progress.multi_root_retained_contexts) +
                ", provenance=" +
                std::to_string(
                    isolated_progress.multi_root_provenance_links) +
                ", session_hits=" +
                std::to_string(isolated_session.hits) +
                ", isolated_lens_requests=" +
                std::to_string(
                    isolated_progress.evaluation_lenses
                        .requests[isolated_lens_index]) +
                ", logical=" +
                std::to_string(isolated_progress.logical_evaluations) +
                ", physical=" +
                std::to_string(isolated_progress.physical_evaluations) +
                ", lookups=" +
                std::to_string(isolated_progress.session_cache_lookups) +
                ", misses=" +
                std::to_string(isolated_progress.session_cache_misses) +
                ", store_sites=" +
                std::to_string(
                    isolated_candidate ==
                            duplicate_isolated.values.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : isolated_candidate->store_instruction_addresses
                              .size()) +
                ", calls=" +
                std::to_string(
                    isolated_candidate ==
                            duplicate_isolated.values.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : isolated_candidate->evidence_call_sites.size()) +
                ", callees=" +
                std::to_string(
                    isolated_candidate ==
                            duplicate_isolated.values.guarded_code_inventory
                                .stored_code_addresses.end()
                        ? 0u
                        : isolated_candidate->evidence_callees.size()) +
                ", guarded=" +
                std::to_string(
                    isolated_candidate !=
                            duplicate_isolated.values.guarded_code_inventory
                                .stored_code_addresses.end() &&
                    isolated_candidate->guarded) +
                ", complete=" +
                std::to_string(
                    isolated_candidate !=
                            duplicate_isolated.values.guarded_code_inventory
                                .stored_code_addresses.end() &&
                    isolated_candidate->complete) +
                ", phase=" + isolated_progress.phase +
                ", retained_bytes=" +
                std::to_string(
                    isolated_progress.multi_root_retained_payload_bytes) +
                ", session_entries=" +
                std::to_string(isolated_session.entries) +
                ", session_bytes=" +
                std::to_string(isolated_session.retained_payload_bytes) +
                ", session_evictions=" +
                std::to_string(isolated_session.evictions) +
                ", fallbacks=" +
                std::to_string(
                    isolated_progress.cache_replay_fallback_recomputes) +
                ", diagnostic_bypasses=" +
                std::to_string(
                    isolated_progress.cache_diagnostic_bypass_evaluations) +
                ", forwarded_hits=" +
                std::to_string(
                    isolated_diagnostics
                        .forwarded_store_evaluation_cache_hits) +
                ", forwarded_misses=" +
                std::to_string(
                    isolated_diagnostics
                        .forwarded_store_evaluation_cache_misses) +
                ", truncated=" +
                std::to_string(isolated_diagnostics.truncated()) +
                ", candidate=" +
                std::to_string(
                    isolated_candidate !=
                    duplicate_isolated.values.guarded_code_inventory
                        .stored_code_addresses.end()) +
                ").");

        const auto parameterized = parameterized_candidate_return_values();
        const auto returned_table = std::find_if(
            parameterized.guarded_code_inventory
                .returned_code_address_tables.begin(),
            parameterized.guarded_code_inventory
                .returned_code_address_tables.end(),
            [](const auto& candidate) {
                return candidate.table_address == 0x60u;
            });
        require(
            returned_table !=
                    parameterized.guarded_code_inventory
                        .returned_code_address_tables.end() &&
                returned_table->target_addresses ==
                    std::vector<std::uint32_t>{0x70u} &&
                !parameterized.budget_exhausted,
            "Begrenzter Candidate-Return-Walk verlor einen "
            "parameterabhaengigen Returned-Table-Codepointer.");
        const auto contextual = contextual_dereference_return_values();
        const auto* contextual_table = returned_table_candidate(contextual, 0x80u);
        const auto& contextual_diagnostics =
            contextual.guarded_code_inventory.walk_diagnostics;
        require(contextual_table != nullptr &&
                    contextual_table->target_addresses ==
                        std::vector<std::uint32_t>{0x90u} &&
                    !contextual_diagnostics
                         .contextual_return_context_limited_functions &&
                    !contextual_diagnostics
                         .contextual_return_evaluation_limited_functions &&
                    !contextual.budget_exhausted,
                "Kontexttaint ging beim Pointer-Dereferenzieren vor dem "
                "direkten Helper-Return verloren.");

        set_stack_diagnostics_for_serial_fixpoint(true);
        const auto multi_owner_contextual_serial =
            multi_owner_contextual_return_values();
        set_stack_diagnostics_for_serial_fixpoint(false);
        const auto multi_owner_contextual_parallel =
            multi_owner_contextual_return_values();
        require_same_function_value_semantics(
            multi_owner_contextual_serial.values,
            multi_owner_contextual_parallel.values);
        require(
                multi_owner_contextual_serial.resolution_roots ==
                    multi_owner_contextual_parallel.resolution_roots &&
                multi_owner_contextual_parallel.resolution_roots == 5u &&
                multi_owner_contextual_parallel.resolution_roots <
                    multi_owner_contextual_parallel.function_count &&
                multi_owner_contextual_parallel.final_progress.phase ==
                    "complete" &&
                multi_owner_contextual_parallel.final_progress
                        .summarized_functions ==
                    multi_owner_contextual_parallel.final_progress.functions &&
                multi_owner_contextual_parallel.final_progress
                        .resolution_functions_committed ==
                    multi_owner_contextual_parallel.final_progress
                        .resolution_functions_total,
            "Die syntaktische Resolution-Root-Auswahl verwarf einen lokalen "
            "Long-Store/Load oder wertete weiterhin reine RTS-Funktionen aus "
            "(roots=" +
                std::to_string(
                    multi_owner_contextual_parallel.resolution_roots) +
                ", functions=" +
                std::to_string(
                    multi_owner_contextual_parallel.function_count) +
                ").");
        const auto* multi_owner_table_a = returned_table_candidate(
            multi_owner_contextual_parallel.values, 0x90u);
        const auto* multi_owner_table_b = returned_table_candidate(
            multi_owner_contextual_parallel.values, 0x98u);
        require(
            multi_owner_table_a != nullptr &&
                multi_owner_table_a->target_addresses ==
                    std::vector<std::uint32_t>{0xC0u, 0xD0u} &&
                multi_owner_table_b != nullptr &&
                multi_owner_table_b->target_addresses ==
                    std::vector<std::uint32_t>{0xD0u} &&
                !multi_owner_contextual_parallel.values
                     .guarded_code_inventory.walk_diagnostics.truncated(),
            "Der globale Contextual-Return-Fixpunkt verlor einen der beiden "
            "Owner am gemeinsamen Helper oder meldete einen falschen "
            "Budgetverlust (table_a=" +
                std::to_string(multi_owner_table_a != nullptr) +
                ", table_b=" +
                std::to_string(multi_owner_table_b != nullptr) +
                ", truncated=" +
                std::to_string(
                    multi_owner_contextual_parallel.values
                        .guarded_code_inventory.walk_diagnostics.truncated()) +
                ").");

        for (const bool stack_argument : {false, true}) {
            const auto overflow =
                contextual_candidate_input_overflow_values(stack_argument);
            const auto& diagnostics =
                overflow.guarded_code_inventory.walk_diagnostics;
            require(
                diagnostics.inventory_candidate_values_truncated &&
                    diagnostics.truncated() && !overflow.budget_exhausted,
                std::string{"Mehr als acht kontextuelle Candidate-"} +
                    (stack_argument ? "Stack-" : "Register-") +
                    "Argumentwerte gingen ohne fail-closed "
                    "Truncation-Diagnose verloren.");
        }

        const auto contextual_budget =
            contextual_read_contract_and_fixpoint_budget_values();
        const auto* contextual_budget_table =
            returned_table_candidate(contextual_budget, 0x80u);
        const auto& contextual_budget_diagnostics =
            contextual_budget.guarded_code_inventory.walk_diagnostics;
        require(
            contextual_budget_table != nullptr &&
                contextual_budget_table->target_addresses ==
                    std::vector<std::uint32_t>{0x90u} &&
                contextual_budget_diagnostics
                        .contextual_return_context_budget == 69u &&
                contextual_budget_diagnostics
                        .contextual_return_evaluation_budget == 65'536u &&
                !contextual_budget_diagnostics
                     .contextual_return_context_limited_functions &&
                !contextual_budget_diagnostics
                     .contextual_return_evaluation_limited_functions &&
                !contextual_budget.budget_exhausted,
            "Read-before-def-Pruning oder der kontextuelle "
            "Rueckgabe-Fixpunkt verlor die echte Dereferenzkette.");

        struct ShiftContractCase {
            std::uint16_t opcode;
            std::uint32_t input;
            std::int32_t expected_slot;
        };
        constexpr std::array<ShiftContractCase, 8u> shift_contracts{{
            {0x4300u, 0x80000004u, 8},
            {0x4301u, 0x00000010u, 8},
            {0x4308u, 0x40000002u, 8},
            {0x4318u, 0x01000001u, 256},
            {0x4328u, 0x00010000u, 0},
            {0x4309u, 0x00000020u, 8},
            {0x4319u, 0x00000800u, 8},
            {0x4329u, 0x00080000u, 8},
        }};
        for (const auto& test : shift_contracts) {
            const auto contract =
                fixed_shift_abi_contract_values(
                    test.opcode, test.input);
            require(
                contract.observed &&
                    contract.stack_reads_complete &&
                    contract.stack_read_slots ==
                        std::vector<std::int32_t>{
                            test.expected_slot} &&
                    !contract.budget_exhausted,
                "Fester SH4-Shift verlor seine Konstante im echten "
                "ABI-Stack-Read-Fixpunkt (Opcode " +
                    std::to_string(test.opcode) + ").");
        }
        const auto maximum_stack_contract =
            fixed_shift_abi_contract_values(
                0x0009u, 65'536u);
        const auto outside_stack_contract =
            fixed_shift_abi_contract_values(
                0x0009u, 65'540u);
        require(
            maximum_stack_contract.observed &&
                maximum_stack_contract.stack_reads_complete &&
                maximum_stack_contract.stack_read_slots ==
                    std::vector<std::int32_t>{65'536} &&
                outside_stack_contract.observed &&
                !outside_stack_contract.stack_reads_complete,
            "Natuerliche ABI-Stackgrenze akzeptierte 65536 nicht exakt "
            "oder behandelte 65540 faelschlich als vollstaendigen Slot.");
    }

    {
        const auto direct_argument = object_field_tail_values(false);
        const auto loaded_field = object_field_tail_values(true);
        require(has_stored_code_address(direct_argument, 0x70u) &&
                    !has_stored_code_address(loaded_field, 0x70u),
                "Objektadress-Provenienz wurde weiterhin als Provenienz "
                "des geladenen Feldinhalts behandelt.");
    }

    {
        const auto forwarded = candidate_call_stack_tail_values();
        require(has_stored_code_address(forwarded, 0x70u),
                "Candidate-only Call -> guarded Frame-Delta -> "
                "Stackspill/Reload -> Candidate-Tail verlor einen explizit "
                "uebergebenen Codepointer.");

        const auto returned = helper_returned_code_pointer_tail_values();
        require(
            has_stored_code_address(returned, 0x70u),
            "Normaler Helper-Return verlor explizite Codepointerprovenienz "
            "vor einem Candidate-Tail-Registrar.");
    }

    {
        const auto direct_literal = direct_literal_global_store_values();
        require(has_stored_code_address(direct_literal, 0x30u),
                "Ein direkter PC-Literal-Callbackstore ohne ABI-Grenze verlor "
                "seinen Guarded-AOT-Eintrag.");

        for (const auto displaced_reload : {false, true}) {
            const auto reloaded_stack =
                reloaded_stack_epoch_values(displaced_reload);
            require(
                has_stored_code_address(reloaded_stack, 0x90u) &&
                    !has_stored_code_address(reloaded_stack, 0x80u) &&
                    !has_stored_code_address(reloaded_stack, 0x88u) &&
                    !reloaded_stack.guarded_code_inventory.walk_diagnostics
                         .truncated(),
                "Ein vollstaendiger 32-Bit-SP-Reload trennte die alte "
                "Stack-Epoch nicht ab, verlor den neuen Callbackslot oder "
                "behielt einen stale Alias.");
        }
        const auto register_reloaded_stack =
            reloaded_stack_epoch_values(false, false, true);
        require(
            has_stored_code_address(register_reloaded_stack, 0x90u) &&
                !has_stored_code_address(register_reloaded_stack, 0x80u) &&
                !has_stored_code_address(register_reloaded_stack, 0x88u) &&
                !register_reloaded_stack.guarded_code_inventory
                     .walk_diagnostics.truncated(),
            "Ein vollstaendiges mov-Rm-r15 trennte die alte Stack-Epoch "
            "nicht sicher ab oder verlor den neuen Callbackslot.");
        const auto aliased_old_stack =
            reloaded_stack_epoch_values(true, true);
        require(
            aliased_old_stack.guarded_code_inventory.walk_diagnostics
                .abi_stack_base_unresolved,
            "Ein vollstaendiger SP-Reload verwarf trotz ueberlebendem Alias "
            "stillschweigend einen alten Inventory-Callback (truncated=" +
                std::to_string(
                    aliased_old_stack.guarded_code_inventory
                        .walk_diagnostics.truncated()) +
                ", stored_80=" +
                std::to_string(
                    has_stored_code_address(aliased_old_stack, 0x80u)) +
                ", stored_88=" +
                std::to_string(
                    has_stored_code_address(aliased_old_stack, 0x88u)) +
                ", stored_90=" +
                std::to_string(
                    has_stored_code_address(aliased_old_stack, 0x90u)) +
                ").");
        const auto saved_self_reload =
            saved_stack_epoch_self_reload_values();
        require(
            has_stored_code_address(saved_self_reload, 0x80u) &&
                !has_stored_code_address(saved_self_reload, 0x88u) &&
                !has_stored_code_address(saved_self_reload, 0x90u) &&
                !saved_self_reload.guarded_code_inventory
                     .walk_diagnostics.truncated(),
            "Gespeicherte Stack-Epoch ging ueber Global-Store, Helper-Call "
            "oder Sonic-Self-Reload mov.l @r15,r15 verloren; alternativ "
            "wurde Handler-/Image-Decoy-Payload als alte Epoch gelesen "
            "(old=" +
                std::to_string(
                    has_stored_code_address(saved_self_reload, 0x80u)) +
                ", handler=" +
                std::to_string(
                    has_stored_code_address(saved_self_reload, 0x88u)) +
                ", decoy=" +
                std::to_string(
                    has_stored_code_address(saved_self_reload, 0x90u)) +
                ", base_loss=" +
                std::to_string(
                    saved_self_reload.guarded_code_inventory
                        .walk_diagnostics.abi_stack_base_unresolved) +
                ", truncated=" +
                std::to_string(
                    saved_self_reload.guarded_code_inventory
                        .walk_diagnostics.truncated()) +
                ").");
        const auto stale_saved_epoch =
            stale_saved_stack_epoch_values();
        require(
            stale_saved_epoch.guarded_code_inventory.walk_diagnostics
                .abi_stack_base_unresolved,
            "Ein nach dem SP-Snapshot neu geschriebener Callback wurde beim "
            "Restore des veralteten Snapshots stillschweigend verloren.");
        const auto omitted_saved_alias =
            omitted_saved_stack_alias_callback_values();
        const auto omitted_saved_alias_callee = std::find_if(
            omitted_saved_alias.analysis.summaries.begin(),
            omitted_saved_alias.analysis.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0x40u;
            });
        require(
            omitted_saved_alias.callee_contract_observed &&
                omitted_saved_alias.callee_stack_reads_complete &&
                omitted_saved_alias.callee_stack_read_slots.empty() &&
                !omitted_saved_alias.analysis.budget_exhausted &&
                omitted_saved_alias_callee !=
                    omitted_saved_alias.analysis.summaries.end() &&
                omitted_saved_alias_callee
                    ->inventory_unresolved_stack_callback_loss,
            "Eine durch das vollstaendige leere ABI-Readset ausgelassene "
            "Saved-SP-Epoche bemerkte den spaeteren Callback-Store des "
            "Callees nicht fail-closed.");
        const auto duplicate_saved_epoch =
            duplicate_saved_stack_epoch_restore_then_callback_values();
        const auto duplicate_saved_epoch_owner = std::find_if(
            duplicate_saved_epoch.summaries.begin(),
            duplicate_saved_epoch.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0u;
            });
        require(
            !duplicate_saved_epoch.budget_exhausted &&
                duplicate_saved_epoch_owner !=
                    duplicate_saved_epoch.summaries.end() &&
                duplicate_saved_epoch_owner
                    ->inventory_unresolved_stack_callback_loss,
            "Zwei leere Saved-SP-Epochen verloren nach Stackwechsel, Restore "
            "und spaeterem Callback-Store die Verbindung zur wieder aktiven "
            "Stackepoche.");
        const auto suspended_stack_slot_alias =
            suspended_stack_slot_saved_epoch_reload_values();
        const auto suspended_stack_slot_alias_owner = std::find_if(
            suspended_stack_slot_alias.summaries.begin(),
            suspended_stack_slot_alias.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0u;
            });
        require(
            !suspended_stack_slot_alias.budget_exhausted &&
                suspended_stack_slot_alias_owner !=
                    suspended_stack_slot_alias.summaries.end() &&
                suspended_stack_slot_alias_owner
                    ->inventory_unresolved_stack_callback_loss,
            "Eine payload-freie Saved-SP-Epoche im suspendierten Stackslot "
            "ging nach Restore, Slot-Reload, erneutem Stackwechsel und "
            "Reload-Restore vor der Callbackmutation verloren.");
        const auto missing_memory_loop =
            saved_stack_epoch_missing_memory_loop_values();
        require(
            !missing_memory_loop.budget_exhausted &&
                missing_memory_loop.fixpoint_iterations <= 2u,
            "Ein bereits geweiteter Saved-Stack-Epoch-Memory-Join meldete "
            "dauerhaft Aenderungen und konvergierte nicht.");
        const auto missing_stack_loop =
            saved_stack_epoch_missing_stack_loop_values();
        require(
            !missing_stack_loop.budget_exhausted &&
                missing_stack_loop.guarded_code_inventory
                        .walk_diagnostics
                        .maximum_local_fixpoint_iterations <=
                    16u,
            "Ein bereits normalisierter Saved-Stack-Epoch-Stack-Join "
            "meldete ohne Zustandsaenderung dauerhaft changed (local=" +
                std::to_string(
                    missing_stack_loop.guarded_code_inventory
                        .walk_diagnostics
                        .maximum_local_fixpoint_iterations) +
                ").");
        const auto regenerated_source_loop =
            saved_stack_epoch_regenerated_source_loop_values();
        const auto& regenerated_source_diagnostics =
            regenerated_source_loop.guarded_code_inventory
                .walk_diagnostics;
        require(
            !regenerated_source_loop.budget_exhausted &&
                regenerated_source_diagnostics
                        .local_fixpoint_limited_evaluations ==
                    0u &&
                regenerated_source_diagnostics
                        .maximum_local_fixpoint_iterations <=
                    32u,
            "Ein payload-freier Saved-Stack-Alias wurde auf einem Looparm "
            "immer neu erzeugt und meldete nach der state-weiten Weitung "
            "weiterhin eine nicht vorhandene Zustandsaenderung (local=" +
                std::to_string(
                    regenerated_source_diagnostics
                        .maximum_local_fixpoint_iterations) +
                ", limited=" +
                std::to_string(
                    regenerated_source_diagnostics
                        .local_fixpoint_limited_evaluations) +
                ").");
        for (const auto direct_local_sink : {false, true}) {
            const auto translated_epoch_loop =
                translated_saved_stack_epoch_loop_values(
                    direct_local_sink);
            const auto& translated_epoch_diagnostics =
                translated_epoch_loop.guarded_code_inventory
                    .walk_diagnostics;
            require(
                !translated_epoch_loop.budget_exhausted &&
                    translated_epoch_diagnostics
                        .abi_stack_base_unresolved &&
                    translated_epoch_diagnostics
                            .maximum_local_fixpoint_iterations <=
                        64u,
                "Ein affin verschobener Saved-Stack-Epoch-Loop konvergierte "
                "nicht schnell auf ein fail-closed Top (local_sink=" +
                    std::to_string(direct_local_sink) +
                    ", local=" +
                    std::to_string(
                        translated_epoch_diagnostics
                            .maximum_local_fixpoint_iterations) +
                    ", base_loss=" +
                    std::to_string(
                        translated_epoch_diagnostics
                            .abi_stack_base_unresolved) +
                    ").");
        }

        const auto recursive_projection =
            recursive_stack_projection_widening_values();
        const auto& recursive_projection_analysis =
            recursive_projection.analysis;
        const auto& recursive_projection_diagnostics =
            recursive_projection_analysis.guarded_code_inventory
                .walk_diagnostics;
        require(
            recursive_projection.recursive_contract_observed &&
                !recursive_projection.recursive_stack_reads_complete &&
                (recursive_projection
                     .recursive_persistent_store_sources &
                 0x10u) != 0u &&
                !recursive_projection_analysis.budget_exhausted &&
                recursive_projection_analysis.fixpoint_iterations <=
                    32u &&
                has_stored_code_address(
                    recursive_projection_analysis, 0xC0u) &&
                recursive_projection_diagnostics
                    .abi_stack_base_unresolved &&
                recursive_projection_diagnostics.truncated(),
            "Ein rekursiver 40-Byte-Frame mit ABI-Readset-Top "
            "akkumulierte am selben Callsite weiterhin leere "
            "Saved-Stack-Provenienz, verlor den endlichen Callback oder "
            "meldete die spaetere echte Snapshot-Mutation nicht fail-closed "
            "(contract=" +
                std::to_string(
                    recursive_projection
                        .recursive_contract_observed) +
                "/" +
                std::to_string(
                    recursive_projection
                        .recursive_stack_reads_complete) +
                ", sources=" +
                std::to_string(
                    recursive_projection
                        .recursive_persistent_store_sources) +
                ", iterations=" +
                std::to_string(
                    recursive_projection_analysis
                        .fixpoint_iterations) +
                ", budget=" +
                std::to_string(
                    recursive_projection_analysis
                        .budget_exhausted) +
                ", candidate=" +
                std::to_string(
                    has_stored_code_address(
                        recursive_projection_analysis,
                        0xC0u)) +
                ", base_loss=" +
                std::to_string(
                    recursive_projection_diagnostics
                        .abi_stack_base_unresolved) +
                ").");

        struct ReturnedLossRegression {
            ReturnedStackCallbackLossCase test_case;
            bool expected_unresolved;
            bool expected_candidate;
        };
        constexpr std::array returned_loss_regressions{
            ReturnedLossRegression{
                ReturnedStackCallbackLossCase::ConsumeReturnedR0,
                true,
                false},
            ReturnedLossRegression{
                ReturnedStackCallbackLossCase::OverwriteReturnedR0,
                false,
                true},
            ReturnedLossRegression{
                ReturnedStackCallbackLossCase::
                    OverwriteReturnedR0WithMoveT,
                false,
                false},
        };
        for (const auto& regression :
             returned_loss_regressions) {
            const auto returned_loss =
                returned_stack_callback_loss_values(
                    regression.test_case);
            const auto owner = std::find_if(
                returned_loss.summaries.begin(),
                returned_loss.summaries.end(),
                [](const auto& summary) {
                    return summary.function_address == 0x40u;
                });
            const katana::analysis::FunctionRegisterValueSummary*
                returned = nullptr;
            if (owner != returned_loss.summaries.end()) {
                const auto found = std::find_if(
                    owner->registers.cbegin(),
                    owner->registers.cend(),
                    [](const auto& value) {
                        return value.register_index == 0u;
                    });
                if (found != owner->registers.cend())
                    returned = &*found;
            }
            const auto& diagnostics =
                returned_loss.guarded_code_inventory
                    .walk_diagnostics;
            require(
                owner != returned_loss.summaries.end() &&
                    returned != nullptr &&
                    returned
                        ->inventory_stack_callback_loss_unresolved &&
                    diagnostics.abi_stack_base_unresolved ==
                        regression.expected_unresolved &&
                    diagnostics.truncated() ==
                        regression.expected_unresolved &&
                    has_stored_code_address(returned_loss, 0x90u) ==
                        regression.expected_candidate,
                "Der r0-spezifische Stack-Callback-Verlust wurde ueber "
                "die Callee-Summary nicht bis zur echten Senke getragen "
                "oder nach einem Register-Overwrite nicht geloescht "
                "(consume=" +
                    std::to_string(
                        regression.expected_unresolved) +
                    ", summary=" +
                    std::to_string(
                        owner != returned_loss.summaries.end() &&
                        returned != nullptr &&
                        returned
                            ->inventory_stack_callback_loss_unresolved) +
                    ", unresolved=" +
                    std::to_string(
                        diagnostics.abi_stack_base_unresolved) +
                    ", candidate=" +
                    std::to_string(
                        has_stored_code_address(
                            returned_loss, 0x90u)) +
                    ", expected_candidate=" +
                    std::to_string(
                        regression.expected_candidate) +
                    ").");
        }

        struct MemoryLossRegression {
            MemoryStackCallbackLossCase test_case;
            bool expected_summary_marker;
            bool expected_latent_marker;
            bool expected_unresolved;
        };
        constexpr std::array memory_loss_regressions{
            MemoryLossRegression{
                MemoryStackCallbackLossCase::ConsumeExactCell,
                true,
                false,
                true},
            MemoryLossRegression{
                MemoryStackCallbackLossCase::ReadUnknownCell,
                true,
                false,
                true},
            MemoryLossRegression{
                MemoryStackCallbackLossCase::
                    ReadUnknownEmptySavedEpoch,
                false,
                true,
                false},
            MemoryLossRegression{
                MemoryStackCallbackLossCase::ReadDifferentCell,
                true,
                false,
                false},
            MemoryLossRegression{
                MemoryStackCallbackLossCase::EmptySavedEpoch,
                false,
                true,
                false},
        };
        for (const auto& regression :
             memory_loss_regressions) {
            const auto memory_loss =
                memory_stack_callback_loss_values(
                    regression.test_case);
            const auto owner = std::find_if(
                memory_loss.summaries.begin(),
                memory_loss.summaries.end(),
                [](const auto& summary) {
                    return summary.function_address == 0x40u;
                });
            const katana::analysis::FunctionMemoryValueSummary*
                memory = nullptr;
            if (owner != memory_loss.summaries.end()) {
                const auto found = std::find_if(
                    owner->memory_values.cbegin(),
                    owner->memory_values.cend(),
                    [](const auto& value) {
                        return value.address == 0x80u;
                    });
                if (found != owner->memory_values.cend())
                    memory = &*found;
            }
            const bool summary_marker =
                owner != memory_loss.summaries.end() &&
                memory != nullptr &&
                memory
                    ->inventory_stack_callback_loss_unresolved;
            const bool latent_marker =
                owner != memory_loss.summaries.end() &&
                memory != nullptr &&
                memory->inventory_saved_stack_alias_latent;
            const auto& diagnostics =
                memory_loss.guarded_code_inventory
                    .walk_diagnostics;
            require(
                summary_marker ==
                        regression.expected_summary_marker &&
                    latent_marker ==
                        regression.expected_latent_marker &&
                    diagnostics.abi_stack_base_unresolved ==
                        regression.expected_unresolved &&
                    diagnostics.truncated() ==
                        regression.expected_unresolved &&
                    has_stored_code_address(memory_loss, 0x90u) ==
                        !regression.expected_unresolved,
                "Der adressspezifische Stack-Callback-Verlust wurde "
                "nicht ueber exakt Speicherzelle 0x80 getragen, faerbte "
                "eine andere Zelle oder verlor eine latente leere Epoche "
                "beziehungsweise ueberlebte ein "
                "Overwrite (summary=" +
                    std::to_string(summary_marker) +
                    ", expected_summary=" +
                    std::to_string(
                        regression.expected_summary_marker) +
                    ", latent=" +
                    std::to_string(latent_marker) +
                    ", expected_latent=" +
                    std::to_string(
                        regression.expected_latent_marker) +
                    ", unresolved=" +
                    std::to_string(
                        diagnostics.abi_stack_base_unresolved) +
                    ", expected_unresolved=" +
                    std::to_string(
                        regression.expected_unresolved) +
                    ", candidate=" +
                    std::to_string(
                        has_stored_code_address(
                            memory_loss, 0x90u)) +
                    ").");
        }

        const auto multi_callee_memory_alias =
            multi_callee_memory_saved_stack_alias_values();
        const auto multi_callee_memory_alias_root = std::find_if(
            multi_callee_memory_alias.summaries.begin(),
            multi_callee_memory_alias.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0u;
            });
        const auto multi_callee_memory_alias_producer = std::find_if(
            multi_callee_memory_alias.summaries.begin(),
            multi_callee_memory_alias.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0x40u;
            });
        const katana::analysis::FunctionMemoryValueSummary*
            multi_callee_memory_alias_cell = nullptr;
        if (multi_callee_memory_alias_producer !=
            multi_callee_memory_alias.summaries.end()) {
            const auto found = std::find_if(
                multi_callee_memory_alias_producer
                    ->memory_values.begin(),
                multi_callee_memory_alias_producer
                    ->memory_values.end(),
                [](const auto& memory) {
                    return memory.address == 0x80u;
                });
            if (found !=
                multi_callee_memory_alias_producer
                    ->memory_values.end())
                multi_callee_memory_alias_cell = &*found;
        }
        require(
            !multi_callee_memory_alias.budget_exhausted &&
                multi_callee_memory_alias_root !=
                    multi_callee_memory_alias.summaries.end() &&
                multi_callee_memory_alias_root
                    ->inventory_unresolved_stack_callback_loss &&
                multi_callee_memory_alias_cell != nullptr &&
                multi_callee_memory_alias_cell
                    ->inventory_saved_stack_alias_latent &&
                multi_callee_memory_alias_cell
                    ->inventory_saved_stack_alias_tracks_current_epoch,
            "Eine nur von einem bekannten Mitglied einer unvollstaendigen "
            "Multi-Callee-Familie gespeicherte leere Saved-SP-Epoche ging "
            "beim Summary-Union vor dem spaeteren Callback-Store verloren.");

        const auto unknown_memory_roundtrip =
            unknown_saved_stack_epoch_memory_roundtrip_values();
        require(
            unknown_memory_roundtrip.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                unknown_memory_roundtrip.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    unknown_memory_roundtrip, 0x80u),
            "Ein Saved-Stack-Epoch ging bei einem unbekannten 32-Bit-"
            "Memory-Store mit anschliessendem Reload still verloren.");
        const auto unknown_empty_epoch_late_callback =
            unknown_saved_stack_epoch_memory_roundtrip_values(true);
        require(
            unknown_empty_epoch_late_callback.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                unknown_empty_epoch_late_callback.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    unknown_empty_epoch_late_callback, 0x80u),
            "Eine zunaechst leere, unbekannt gespeicherte Stack-Epoche "
            "ging nach einem spaeteren Callback-Store still verloren.");

        const auto identity_old_stack_alias =
            identity_transformed_old_stack_alias_values();
        require(
            identity_old_stack_alias.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                identity_old_stack_alias.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    identity_old_stack_alias, 0x80u),
            "Die Identitaet `mov r15,r0; or #0,r0` verlor vor einem "
            "vollstaendigen Stackwechsel den Alias auf den alten "
            "Callback-Stack.");

        const auto merged_old_stack_alias =
            merged_old_stack_alias_values();
        require(
            merged_old_stack_alias.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                merged_old_stack_alias.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    merged_old_stack_alias, 0x80u),
            "Ein nur auf einem CFG-Pfad vorhandener alter Stack-Alias "
            "verlor beim Merge vor dem Stackwechsel seinen "
            "wertgebundenen Callback-Verlustmarker.");

        const auto conditional_memory_loss =
            conditional_memory_stack_callback_loss_values();
        const auto conditional_memory_owner = std::find_if(
            conditional_memory_loss.summaries.begin(),
            conditional_memory_loss.summaries.end(),
            [](const auto& summary) {
                return summary.function_address == 0x40u;
            });
        const bool conditional_memory_marker =
            conditional_memory_owner !=
                conditional_memory_loss.summaries.end() &&
            std::any_of(
                conditional_memory_owner->memory_values.begin(),
                conditional_memory_owner->memory_values.end(),
                [](const auto& value) {
                    return value.address == 0x80u &&
                           value
                               .inventory_stack_callback_loss_unresolved;
                });
        require(
            conditional_memory_marker &&
                conditional_memory_loss.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                conditional_memory_loss.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    conditional_memory_loss, 0x90u),
            "Ein adressspezifischer Stack-Callback-Verlust ging verloren, "
            "wenn nur ein CFG-Vorgaenger die markierte Speicherzelle "
            "besass (summary=" +
                std::to_string(conditional_memory_marker) +
                ", unresolved=" +
                std::to_string(
                    conditional_memory_loss.guarded_code_inventory
                        .walk_diagnostics.abi_stack_base_unresolved) +
                ", candidate=" +
                std::to_string(
                    has_stored_code_address(
                        conditional_memory_loss, 0x90u)) +
                ").");

        const auto conditional_stack_loss =
            conditional_stack_slot_callback_loss_values();
        require(
            conditional_stack_loss.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                conditional_stack_loss.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    conditional_stack_loss, 0x90u),
            "Ein wertgenauer Stack-Callback-Verlust ging verloren, wenn "
            "nur ein CFG-Vorgaenger den markierten Stackslot besass.");

        const auto candidate_only_memory_loss =
            candidate_only_memory_stack_callback_loss_values();
        require(
            candidate_only_memory_loss.guarded_code_inventory
                    .walk_diagnostics.abi_stack_base_unresolved &&
                candidate_only_memory_loss.guarded_code_inventory
                    .walk_diagnostics.truncated() &&
                !has_stored_code_address(
                    candidate_only_memory_loss, 0xD0u),
            "Ein exakter Memory-Verlustmarker erreichte keinen "
            "candidate-only Callee, dessen Senke keine ABI-Quelle liest.");

        katana::analysis::ControlFlowAnalysisResult
            loss_report;
        katana::analysis::FunctionValueSummary
            loss_report_summary;
        loss_report_summary.function_address = 0x40u;
        katana::analysis::FunctionRegisterValueSummary
            loss_report_register;
        loss_report_register.register_index = 0u;
        loss_report_register
            .inventory_stack_callback_loss_unresolved = true;
        loss_report_summary.registers.push_back(
            loss_report_register);
        katana::analysis::FunctionMemoryValueSummary
            loss_report_memory;
        loss_report_memory.address = 0x80u;
        loss_report_memory
            .inventory_stack_callback_loss_unresolved = true;
        loss_report_summary.memory_values.push_back(
            loss_report_memory);
        loss_report.function_value_summaries.push_back(
            loss_report_summary);
        loss_report.guarded_code_inventory_walk
            .local_fixpoint_iteration_budget = 65'536u;
        loss_report.guarded_code_inventory_walk
            .local_fixpoint_limited_evaluations = 1u;
        const auto loss_report_json =
            katana::analysis::format_control_flow_analysis_json(
                loss_report);
        constexpr auto loss_json_marker =
            "\"inventory_stack_callback_loss_unresolved\":true";
        const auto first_loss_json_marker =
            loss_report_json.find(loss_json_marker);
        require(
            first_loss_json_marker != std::string::npos &&
                loss_report_json.find(
                    loss_json_marker,
                    first_loss_json_marker + 1u) !=
                    std::string::npos &&
                loss_report_json.find(
                    "\"guarded_local_fixpoint_iteration_budget\":65536") !=
                    std::string::npos &&
                loss_report_json.find(
                    "\"guarded_local_fixpoint_limited_evaluations\":1") !=
                    std::string::npos,
            "Register- oder Memory-Summary verlor den neuen "
            "Stack-Callback-Verlustmarker beziehungsweise die lokale "
            "Fixpunktgrenze im JSON-Bericht.");

        const auto mixed = mixed_literal_scalar_store_values();
        require(has_stored_code_address(mixed, 0x30u) &&
                    !has_stored_code_address(mixed, 0x34u),
                "Wertgenaue Literalprovenienz vermischte Callback und "
                "decode-validen Scalar vor dem persistenten Store.");

        const auto wide_join = wide_pc_literal_join_store_values();
        constexpr std::array<std::uint32_t, 9u> wide_store_callbacks{
            0xC0u, 0xC4u, 0xC8u, 0xCCu, 0xD0u,
            0xD4u, 0xD8u, 0xDCu, 0xE0u,
        };
        require(std::all_of(
                    wide_store_callbacks.begin(),
                    wide_store_callbacks.end(),
                    [&](const auto candidate) {
                        return has_stored_code_address(wide_join, candidate);
                    }) &&
                    !wide_join.guarded_code_inventory.walk_diagnostics
                         .inventory_candidate_values_truncated,
                "Der 9-Wege-Join verlor Callbackkandidaten am alten "
                "semantischen 8-Werte-Limit.");

        constexpr std::array<std::uint32_t, 9u> wide_callbacks{
            0x140u, 0x144u, 0x148u, 0x14Cu, 0x150u,
            0x154u, 0x158u, 0x15Cu, 0x160u,
        };
        const auto wide_indirect = wide_abi_stack_indirect_call_values();
        const auto wide_dispatch = std::find_if(
            wide_indirect.resolutions.begin(),
            wide_indirect.resolutions.end(),
            [](const auto& candidate) {
                return candidate.instruction_address == 0xC4u;
            });
        const auto& wide_diagnostics =
            wide_indirect.guarded_code_inventory.walk_diagnostics;
        require(
            wide_dispatch != wide_indirect.resolutions.end() &&
                wide_dispatch->targets ==
                    std::vector<std::uint32_t>(wide_callbacks.begin(),
                                               wide_callbacks.end()) &&
                wide_dispatch->guarded && !wide_dispatch->complete &&
                !wide_diagnostics.inventory_candidate_values_truncated &&
                !wide_diagnostics
                     .abi_stack_argument_projection_truncated_functions &&
                !wide_diagnostics
                     .forwarded_store_context_limited_functions &&
                !wide_diagnostics
                     .contextual_return_evaluation_limited_functions,
            "Der 9-Wege-ABI-Callback verlor beim Stackspill/Reload vor "
            "dem Helper-Aufruf und indirekten JSR seine Inventory-Ziele "
            "nach einem FPSCR-abhaengigen FPU-Prolog oder meldete dabei "
            "eine falsche Projektions-/Kontextgrenze.");

        const auto wide_flow = katana::analysis::analyze_control_flow(
            wide_abi_stack_indirect_call_image());
        const auto* wide_flow_dispatch = site(wide_flow, 0xC4u);
        const auto has_wide_resolved_edge = std::any_of(
            wide_flow.resolved_edges.begin(),
            wide_flow.resolved_edges.end(),
            [](const auto& edge) {
                return edge.instruction_address == 0xC4u;
            });
        const auto wide_callbacks_decoded = std::all_of(
            wide_callbacks.begin(),
            wide_callbacks.end(),
            [&](const auto callback) {
                return std::any_of(
                    wide_flow.recursive.instructions.begin(),
                    wide_flow.recursive.instructions.end(),
                    [callback](const auto& line) {
                        return line.address == callback;
                    });
            });
        const auto wide_flow_ir = katana::ir::lower_program(wide_flow);
        const auto wide_callbacks_seeded = std::all_of(
            wide_callbacks.begin(),
            wide_callbacks.end(),
            [&](const auto callback) {
                return std::any_of(
                    wide_flow_ir.begin(),
                    wide_flow_ir.end(),
                    [callback](const auto& function) {
                        return std::any_of(
                            function.blocks.begin(),
                            function.blocks.end(),
                            [callback](const auto& block) {
                                return block.start_address == callback;
                            });
                    });
            });
        require(
            wide_flow_dispatch != nullptr &&
                wide_flow_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                !wide_flow_dispatch->target.has_value() &&
                wide_flow_dispatch->targets.empty() &&
                wide_flow_dispatch->analysis_candidates ==
                    std::vector<std::uint32_t>(wide_callbacks.begin(),
                                               wide_callbacks.end()) &&
                !has_wide_resolved_edge && wide_callbacks_decoded &&
                wide_callbacks_seeded &&
                katana::ir::verify_program(wide_flow_ir).empty(),
            "Der ueber einen Helper weitergereichte ABI-Callback erreichte "
            "RuntimeOnly-CFG, rekursive Dekodierung oder IR-Seeding nicht "
            "ohne semantische Zielkante.");

        const auto mixed_abi = mixed_null_callback_register_argument_values();
        require(has_stored_code_address(mixed_abi, 0x1050u) &&
                    !has_stored_code_address(mixed_abi, 0u),
                "Ein ungueltiges nullptr-Alternativargument loeschte den "
                "gueltigen Callback an der ABI-Grenze.");

        const auto sub_stack = sub_register_fifth_stack_callback_values(true);
        require(has_stored_code_address(sub_stack, 0x50u) &&
                    !sub_stack.guarded_code_inventory.walk_diagnostics
                         .abi_stack_base_unresolved,
                "SUB-Register-Framing verlor das fuenfte ABI-Stackargument.");

        const auto unresolved_stack =
            sub_register_fifth_stack_callback_values(false);
        require(unresolved_stack.guarded_code_inventory.walk_diagnostics
                    .abi_stack_base_unresolved,
                "Unaufgeloeste SUB-Register-Stackbasis blieb undiagnostiziert.");

        const auto harmless_unresolved_stack =
            sub_register_fifth_stack_callback_values(false, false);
        require(!harmless_unresolved_stack.guarded_code_inventory.walk_diagnostics
                     .abi_stack_base_unresolved,
                "Harmloser unbekannter lokaler Stackzugriff blockierte den Export.");

        const auto fifth_argument = fifth_stack_callback_values();
        require(has_stored_code_address(fifth_argument, 0x50u) &&
                    !has_stored_code_address(fifth_argument, 0x54u) &&
                    !fifth_argument.guarded_code_inventory.walk_diagnostics
                         .abi_stack_argument_projection_truncated_functions,
                "Fuenftes ABI-Stackargument oder Caller-lokaler Negativslot "
                "wurde falsch in den Callee transferiert oder loeste eine "
                "falsche Projektionsgrenze aus.");

        const auto exact_stack_projection =
            exact_abi_stack_slot_projection_values();
        require(
            has_stored_code_address(exact_stack_projection, 0x180u) &&
                exact_stack_projection.guarded_code_inventory
                        .walk_diagnostics
                        .abi_stack_argument_slot_budget == 16'385u &&
                !exact_stack_projection.guarded_code_inventory.walk_diagnostics
                     .abi_stack_argument_projection_truncated_functions,
            "Mehr als 64 ausgehende Stackfacts loesten am bekannten "
            "GuardedPartial-Callee mit storefreiem Epilogpfad trotz einzig "
            "relevantem Slot 0 eine ABI-Projektionstrunkierung aus oder "
            "verloren den Callback.");

        const auto fifth_argument_tail = fifth_stack_callback_tail_values();
        require(
            has_stored_code_address(fifth_argument_tail, 0x70u) &&
                !fifth_argument_tail.guarded_code_inventory.walk_diagnostics
                     .abi_stack_argument_projection_truncated_functions,
                "Fuenftes ABI-Stackargument verlor seinen Codepointer durch "
                "einen bewachten Tail-Wrapper oder loeste eine falsche "
                "Projektionsgrenze aus.");

        const auto mixed_destination =
            mixed_stack_object_destination_values();
        const auto mixed_destination_candidate = std::find_if(
            mixed_destination.guarded_code_inventory
                .stored_code_addresses.begin(),
            mixed_destination.guarded_code_inventory
                .stored_code_addresses.end(),
            [](const auto& candidate) {
                return candidate.target_address == 0x90u &&
                       std::find(
                           candidate.store_instruction_addresses.begin(),
                           candidate.store_instruction_addresses.end(),
                           0x40u) !=
                           candidate.store_instruction_addresses.end();
            });
        const auto& mixed_destination_diagnostics =
            mixed_destination.guarded_code_inventory.walk_diagnostics;
        require(
            mixed_destination_candidate !=
                    mixed_destination.guarded_code_inventory
                        .stored_code_addresses.end() &&
                !mixed_destination_diagnostics.truncated(),
            "Ein am selben Callsite gemischtes Stack-/Objektziel "
            "uebersprang den zwingenden exakten Store-Walk, verlor den "
            "Objektpfad-Callback oder meldete eine falsche Stackbasis.");

        const auto helper_mixed = helper_mixed_return_store_values();
        require(has_stored_code_address(helper_mixed, 0x60u) &&
                    !has_stored_code_address(helper_mixed, 0x64u),
                "Gemischte Helper-Rueckgabe verlor wertgenaue "
                "PC-Literalprovenienz oder inventarisierte den Scalar.");
    }

    std::vector<std::uint8_t> guarded_callee_bytes(0x26u, 0x09u);
    const std::array<std::uint8_t, 10u> guarded_caller{
        0x10u,
        0xE4u, // mov #0x10,r4
        0x0Bu,
        0x41u, // jsr @r1
        0x09u,
        0x00u, // nop (delay)
        0x2Bu,
        0x40u, // jmp @r0
        0x09u,
        0x00u // nop (delay)
    };
    const std::array<std::uint8_t, 6u> guarded_callee{
        0x43u,
        0x60u, // mov r4,r0
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(guarded_caller.begin(), guarded_caller.end(), guarded_callee_bytes.begin());
    std::copy(guarded_callee.begin(), guarded_callee.end(), guarded_callee_bytes.begin() + 0x20u);
    katana::io::ExecutableImage guarded_callee_image;
    guarded_callee_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    guarded_callee_image.add_segment({".text",
                                      0u,
                                      0u,
                                      guarded_callee_bytes.size(),
                                      katana::io::SegmentKind::Code,
                                      {true, false, true},
                                      guarded_callee_bytes});
    guarded_callee_image.add_entry_point(0u);
    const auto guarded_callee_lines = katana::sh4::disassemble(guarded_callee_bytes, 0u);
    const std::array<std::uint32_t, 1u> guarded_function_entries{0u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1u> guarded_call_edges{{
        {2u,
         0x20u,
         katana::analysis::ResolvedControlFlowKind::Call,
         true,
         katana::analysis::ControlFlowEvidence::GuardedComplete,
         {katana::analysis::AnalysisEvidenceOrigin::EntrySnapshot}},
    }};
    const auto guarded_values = katana::analysis::analyze_function_values(
        guarded_callee_image, guarded_callee_lines, guarded_function_entries, guarded_call_edges);
    const auto guarded_callee_summary =
        std::find_if(guarded_values.summaries.begin(),
                     guarded_values.summaries.end(),
                     [](const auto& candidate) { return candidate.function_address == 0x20u; });
    require(guarded_callee_summary != guarded_values.summaries.end(),
            "Guarded-complete-Callkante legte ihren exklusiv erreichbaren Callee nicht an.");
    const auto guarded_r0 =
        std::find_if(guarded_callee_summary->registers.begin(),
                     guarded_callee_summary->registers.end(),
                     [](const auto& candidate) { return candidate.register_index == 0u; });
    const auto guarded_return =
        std::find_if(guarded_values.resolutions.begin(),
                     guarded_values.resolutions.end(),
                     [](const auto& candidate) { return candidate.instruction_address == 6u; });
    require(guarded_r0 != guarded_callee_summary->registers.end() && guarded_r0->complete &&
                guarded_r0->guarded && guarded_r0->values == std::vector<std::uint32_t>{0x10u} &&
                guarded_return != guarded_values.resolutions.end() && guarded_return->complete &&
                guarded_return->guarded &&
                guarded_return->evidence ==
                    katana::analysis::ControlFlowEvidence::GuardedComplete &&
                guarded_return->targets == std::vector<std::uint32_t>{0x10u} &&
                guarded_return->call_sites == std::vector<std::uint32_t>{2u} &&
                guarded_return->callees == std::vector<std::uint32_t>{0x20u},
            "Guarded-complete-Callee verlor Ingressguard oder R0-Return-Summary.");

    const auto guarded_return_table_image = [] {
        std::vector<std::uint8_t> bytes(0x60u, 0x09u);
        bytes[0x00u] = 0x05u;
        bytes[0x01u] = 0xD1u; // mov.l @(0x18,pc),r1 -> accessor 0x20
        bytes[0x02u] = 0x0Bu;
        bytes[0x03u] = 0x41u; // jsr @r1
        bytes[0x04u] = 0x09u;
        bytes[0x05u] = 0x00u; // nop (delay)
        bytes[0x06u] = 0x03u;
        bytes[0x07u] = 0x6Cu; // mov r0,r12
        bytes[0x08u] = 0xC2u;
        bytes[0x09u] = 0x63u; // mov.l @r12,r3
        bytes[0x0Au] = 0x0Bu;
        bytes[0x0Bu] = 0x43u; // jsr @r3
        bytes[0x0Cu] = 0x09u;
        bytes[0x0Du] = 0x00u; // nop (delay)
        bytes[0x0Eu] = 0x0Bu;
        bytes[0x0Fu] = 0x00u; // rts
        bytes[0x10u] = 0x09u;
        bytes[0x11u] = 0x00u; // nop (delay)
        bytes[0x18u] = 0x20u;
        bytes[0x19u] = 0x00u;
        bytes[0x1Au] = 0x00u;
        bytes[0x1Bu] = 0x00u;
        bytes[0x20u] = 0x02u;
        bytes[0x21u] = 0xD0u; // mov.l @(0x2c,pc),r0 -> table 0x40
        bytes[0x22u] = 0x0Bu;
        bytes[0x23u] = 0x00u; // rts
        bytes[0x24u] = 0x09u;
        bytes[0x25u] = 0x00u; // nop (delay)
        bytes[0x2Cu] = 0x40u;
        bytes[0x2Du] = 0x00u;
        bytes[0x2Eu] = 0x00u;
        bytes[0x2Fu] = 0x00u;
        bytes[0x40u] = 0x50u;
        bytes[0x41u] = 0x00u;
        bytes[0x42u] = 0x00u;
        bytes[0x43u] = 0x00u;
        bytes[0x44u] = 0x54u;
        bytes[0x45u] = 0x00u;
        bytes[0x46u] = 0x00u;
        bytes[0x47u] = 0x00u;
        bytes[0x48u] = 0x58u;
        bytes[0x49u] = 0x00u;
        bytes[0x4Au] = 0x00u;
        bytes[0x4Bu] = 0x00u;
        bytes[0x50u] = 0x0Bu;
        bytes[0x51u] = 0x00u; // handler: rts
        bytes[0x52u] = 0x09u;
        bytes[0x53u] = 0x00u; // nop (delay)
        bytes[0x54u] = 0x0Bu;
        bytes[0x55u] = 0x00u; // sibling handler: rts
        bytes[0x56u] = 0x09u;
        bytes[0x57u] = 0x00u; // nop (delay)
        bytes[0x58u] = 0x0Bu;
        bytes[0x59u] = 0x00u; // sibling handler: rts
        bytes[0x5Au] = 0x09u;
        bytes[0x5Bu] = 0x00u; // nop (delay)
        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        image.set_initial_snapshot_entry(0x58u);
        image.add_segment({".guarded-return-table",
                           0u,
                           0u,
                           bytes.size(),
                           katana::io::SegmentKind::Mixed,
                           {true, true, true},
                           std::move(bytes),
                           katana::io::ImageSourceKind::DiscBootFile,
                           katana::io::ImageLoadPhase::Initial,
                           "synthetic-guarded-return-table"});
        image.add_entry_point(0u);
        return image;
    }();
    const auto guarded_return_table =
        katana::analysis::analyze_control_flow(guarded_return_table_image);
    const auto* guarded_table_summary = summary(guarded_return_table, 0x20u, 0u);
    const auto* guarded_accessor_dispatch = site(guarded_return_table, 0x02u);
    const auto* guarded_table_dispatch = site(guarded_return_table, 0x0Au);
    constexpr std::array guarded_family{0x50u, 0x54u, 0x58u};
    require(guarded_table_summary != nullptr && !guarded_table_summary->complete &&
                guarded_table_summary->guarded &&
                guarded_table_summary->values == std::vector<std::uint32_t>{0x40u} &&
                guarded_table_summary->reason == "constant-return-candidate",
            "Endliche Guarded-Partial-Rueckgabe ging in der Funktionssummary verloren.");
    require(guarded_accessor_dispatch != nullptr &&
                guarded_accessor_dispatch->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                guarded_accessor_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                guarded_accessor_dispatch->targets.empty() &&
                guarded_accessor_dispatch->analysis_candidates ==
                    std::vector<std::uint32_t>{0x20u},
            "Indirekter Guarded-Accessor verlor seinen reinen Runtime-Kandidaten.");
    require(guarded_table_dispatch != nullptr &&
                guarded_table_dispatch->status ==
                    katana::analysis::ResolutionStatus::Unresolved &&
                guarded_table_dispatch->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                guarded_table_dispatch->targets.empty() &&
                guarded_table_dispatch->analysis_candidates ==
                    std::vector<std::uint32_t>{0x50u} &&
                guarded_table_dispatch->reason == "runtime-contract-function-memory",
            "Guarded-Return-Tabellenload verlor seinen reinen Runtime-Kandidaten.");
    require(std::all_of(guarded_family.begin(),
                        guarded_family.end(),
                        [&](const auto address) {
                            return std::any_of(
                                guarded_return_table.recursive.functions.begin(),
                                guarded_return_table.recursive.functions.end(),
                                [address](const auto& function) {
                                    return function.address == address;
                                });
                        }),
            "Guarded-Return-Tabellenfamilie erreichte das AOT-Inventar nicht vollstaendig.");
    require(std::none_of(guarded_return_table.resolved_edges.begin(),
                         guarded_return_table.resolved_edges.end(),
                         [](const auto& edge) {
                             return (edge.instruction_address == 0x02u &&
                                     edge.target_address == 0x20u) ||
                                    (edge.instruction_address == 0x0Au &&
                                     edge.target_address == 0x50u);
                         }),
            "Guarded-Return-Kandidaten wurden faelschlich zu autoritativen CFG-Kanten.");
    const auto guarded_sibling =
        std::find_if(guarded_return_table.recursive.functions.begin(),
                     guarded_return_table.recursive.functions.end(),
                     [](const auto& function) { return function.address == 0x54u; });
    require(guarded_sibling != guarded_return_table.recursive.functions.end() &&
                guarded_sibling->origins ==
                    std::vector{katana::analysis::FunctionOrigin::GuardedSnapshot},
            "Nicht direkt geladener Tabellenbruder verlor seine Guarded-Snapshot-Herkunft.");

    auto short_return_table_image = guarded_return_table_image;
    short_return_table_image.write_u32_le(0x48u, 1u);
    const auto short_return_table =
        katana::analysis::analyze_control_flow(short_return_table_image);
    require(std::any_of(short_return_table.recursive.functions.begin(),
                        short_return_table.recursive.functions.end(),
                        [](const auto& function) {
                            return function.address == 0x54u &&
                                   function.origins ==
                                       std::vector{katana::analysis::FunctionOrigin::
                                                       GuardedSnapshot};
                        }) &&
                std::none_of(short_return_table.recursive.functions.begin(),
                             short_return_table.recursive.functions.end(),
                             [](const auto& function) {
                                 return function.address == 0x58u;
                             }),
            "Bewiesene Zweiereintragstabelle wurde nicht begrenzt inventarisiert.");

    auto single_return_table_image = short_return_table_image;
    single_return_table_image.write_u32_le(0x44u, 1u);
    const auto single_return_table =
        katana::analysis::analyze_control_flow(single_return_table_image);
    require(std::any_of(single_return_table.recursive.functions.begin(),
                        single_return_table.recursive.functions.end(),
                        [](const auto& function) {
                            return function.address == 0x50u &&
                                   std::find(
                                       function.origins.begin(),
                                       function.origins.end(),
                                       katana::analysis::FunctionOrigin::
                                           GuardedSnapshot) !=
                                       function.origins.end();
                        }) &&
                std::none_of(single_return_table.recursive.functions.begin(),
                             single_return_table.recursive.functions.end(),
                             [](const auto& function) {
                                 return function.address == 0x54u;
                             }),
            "Konkret geladener einzelner Callbackslot erreichte das bewachte AOT-Inventar "
            "nicht.");

    {
        const auto incomplete_family = incomplete_return_family_values();
        const auto accessor = std::find_if(
            incomplete_family.summaries.begin(),
            incomplete_family.summaries.end(),
            [](const auto& candidate) {
                return candidate.function_address == 0x20u;
            });
        require(accessor != incomplete_family.summaries.end(),
                "Incomplete Callee-Familie verlor den bekannten Accessor.");
        const auto accessor_return =
            std::find_if(accessor->registers.begin(),
                         accessor->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 0u;
                         });
        require(accessor_return != accessor->registers.end() &&
                    accessor_return->values ==
                        std::vector<std::uint32_t>{0x40u} &&
                    !accessor_return->may_alias_stack,
                "Bekannter Accessor verlor seinen Non-Stack-Return.");
        const auto owner = std::find_if(
            incomplete_family.summaries.begin(),
            incomplete_family.summaries.end(),
            [](const auto& candidate) {
                return candidate.function_address == 0u;
            });
        require(owner != incomplete_family.summaries.end(),
                "Incomplete Callee-Familie verlor ihre Caller-Summary.");
        const auto returned =
            std::find_if(owner->registers.begin(),
                         owner->registers.end(),
                         [](const auto& candidate) {
                             return candidate.register_index == 8u;
                         });
        const auto* table =
            returned_table_candidate(incomplete_family, 0x40u);
        const auto dispatch = std::find_if(
            incomplete_family.resolutions.begin(),
            incomplete_family.resolutions.end(),
            [](const auto& candidate) {
                return candidate.instruction_address == 0x0Cu;
            });
        require(returned != owner->registers.end() &&
                    returned->abi_preserved && returned->may_alias_stack,
                "Incomplete Callee-Familie verlor ihr semantisches "
                "Stack-May-Alias am bekannten Non-Stack-Return (may_alias=" +
                    std::to_string(returned->may_alias_stack) +
                    ", abi_preserved=" +
                    std::to_string(returned->abi_preserved) +
                    ").");
        require(owner->memory_values.empty(),
                "Inventory-only Non-Stack-Provenienz erzeugte einen normalen "
                "Memory-Summary-Beweis.");
        require(table != nullptr &&
                    table->target_addresses ==
                        std::vector<std::uint32_t>{0x50u},
                "Candidate-Call-Carrier verlor den bewachten Returned-Table-Seed "
                "(tables=" +
                    std::to_string(
                        incomplete_family.guarded_code_inventory
                            .returned_code_address_tables.size()) +
                    ").");
        require(dispatch != incomplete_family.resolutions.end() &&
                    dispatch->guarded && !dispatch->complete &&
                    dispatch->evidence ==
                        katana::analysis::ControlFlowEvidence::GuardedPartial &&
                    !incomplete_family.budget_exhausted &&
                    incomplete_family.fixpoint_iterations <= 2u,
                "Candidate-Call-Carrier erzeugte einen autoritativen CFG-Beweis "
                "oder trat statt des begrenzten Inventory-Passes in den "
                "semantischen Summary-Fixpunkt ein (iterations=" +
                    std::to_string(incomplete_family.fixpoint_iterations) +
                    ").");
    }

    for (const auto isolated_harvest : {false, true}) {
        const auto shifted_stack =
            shifted_stack_alias_values(isolated_harvest);
        require(
            std::none_of(
                shifted_stack.guarded_code_inventory.stored_code_addresses.begin(),
                shifted_stack.guarded_code_inventory.stored_code_addresses.end(),
                [](const auto& candidate) {
                    return candidate.target_address == 0x60u;
                }),
            isolated_harvest
                ? "Isolated Store Harvest lud nach verschobenem Caller-SP einen "
                  "ueberschriebenen Stackslot als Callback."
                : "Candidate-Input-Merge lud nach verschobenem Caller-SP einen "
                  "ueberschriebenen Stackslot als Callback.");
    }

    [] {
        constexpr auto mov_r0_r12 = std::uint16_t{0x6C03u};
        constexpr auto nop = std::uint16_t{0x0009u};
        constexpr auto movt_r0 = std::uint16_t{0x0029u};
        constexpr auto shll2_r0 = std::uint16_t{0x4008u};
        constexpr auto mov_l_at_r12_r3 = std::uint16_t{0x63C2u};
        constexpr auto mov_l_at_r12_post_r3 = std::uint16_t{0x63C6u};
        constexpr auto mov_l_at_4_r12_r3 = std::uint16_t{0x53C1u};
        constexpr auto mov_l_at_r0_r12_r3 = std::uint16_t{0x03CEu};
        constexpr auto mov_l_at_r0_r0_r3 = std::uint16_t{0x030Eu};

        const auto direct = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, {{0x70u, 0xC0u}}));
        const auto* direct_table = returned_table_candidate(direct, 0x70u);
        require(direct_table != nullptr &&
                    direct_table->target_addresses ==
                        std::vector<std::uint32_t>{0xC0u} &&
                    direct_table->load_instruction_addresses ==
                        std::vector<std::uint32_t>{0x08u} &&
                    direct_table->evidence_call_sites ==
                        std::vector<std::uint32_t>{0x00u} &&
                    direct_table->evidence_callees ==
                        std::vector<std::uint32_t>{0x20u},
                "MOV.L @Rm verlor den einzelnen provenance-starken Callbackslot.");

        auto direct_with_unknown_ingress_image = returned_table_load_image(
            {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, {{0x70u, 0xC0u}});
        direct_with_unknown_ingress_image.add_entry_point(0x20u);
        const auto direct_with_unknown_ingress =
            returned_table_values(direct_with_unknown_ingress_image);
        const auto* preserved_direct_table =
            returned_table_candidate(direct_with_unknown_ingress, 0x70u);
        require(preserved_direct_table != nullptr &&
                    preserved_direct_table->target_addresses ==
                        std::vector<std::uint32_t>{0xC0u} &&
                    preserved_direct_table->evidence_call_sites ==
                        std::vector<std::uint32_t>{0x00u},
                "Unbekannter zusaetzlicher Callee-Ingress reduzierte den vorhandenen "
                "Returned-Table-Bestand.");

        const auto post_increment = returned_table_values(
            returned_table_load_image({mov_r0_r12, nop},
                                      mov_l_at_r12_post_r3,
                                      {0x70u},
                                      {{0x70u, 0xC0u}, {0x74u, 0xC4u}}));
        const auto* post_increment_table =
            returned_table_candidate(post_increment, 0x70u);
        require(post_increment_table != nullptr &&
                    post_increment_table->target_addresses ==
                        std::vector<std::uint32_t>({0xC0u, 0xC4u}),
                "MOV.L @Rm+ verwendete nicht den alten Basiswert fuer die "
                "Zweierslottabelle.");

        const auto displaced = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop},
            mov_l_at_4_r12_r3,
            {0x6Cu},
            {{0x70u, 0xC0u}}));
        require(returned_table_candidate(displaced, 0x70u) != nullptr &&
                    returned_table_candidate(displaced, 0x6Cu) == nullptr,
                "MOV.L @(disp,Rm) addierte den skalierten Decoder-Displacement nicht "
                "exakt.");

        const auto indexed = returned_table_values(returned_table_load_image(
            {mov_r0_r12, movt_r0, shll2_r0},
            mov_l_at_r0_r12_r3,
            {0x60u, 0x70u},
            {{0x60u, 0xC0u},
             {0x64u, 0xC4u},
             {0x70u, 0xC8u},
             {0x74u, 0xCCu}}));
        for (const auto address : {0x60u, 0x64u, 0x70u, 0x74u}) {
            const auto* table = returned_table_candidate(indexed, address);
            require(table != nullptr &&
                        table->evidence_call_sites ==
                            std::vector<std::uint32_t>{0x00u} &&
                        table->evidence_callees ==
                            std::vector<std::uint32_t>{0x20u},
                    "MOV.L @(R0,Rm) verlor eine endliche kartesische "
                    "Effektivadresse.");
        }

        const auto same_register = returned_table_values(
            returned_table_load_image({nop, nop},
                                      mov_l_at_r0_r0_r3,
                                      {0x38u, 0x3Cu},
                                      {{0x70u, 0xC0u},
                                       {0x74u, 0xC4u},
                                       {0x78u, 0xC8u}}));
        for (const auto address : {0x70u, 0x78u}) {
            require(returned_table_candidate(same_register, address) != nullptr,
                    "MOV.L @(R0,R0) verlor eine korrelierte Selbstsumme.");
        }
        require(returned_table_candidate(same_register, 0x74u) == nullptr,
                "MOV.L @(R0,R0) erfand die unmoegliche kartesische Summe x+y.");

        const auto sparse = returned_table_values(returned_table_load_image(
            {mov_r0_r12, nop},
            mov_l_at_r12_r3,
            {0x70u},
            {{0x70u, 0u},
             {0x74u, 0xC0u},
             {0x78u, 0xC4u},
             {0x7Cu, 1u},
             {0x80u, 0xC8u}}));
        const auto* sparse_table = returned_table_candidate(sparse, 0x70u);
        require(sparse_table != nullptr &&
                    sparse_table->target_addresses ==
                        std::vector<std::uint32_t>({0xC0u, 0xC4u, 0xC8u}),
                "Eng begrenzte Null-/Reservierungsslots kappten die "
                "provenance-starke Callbacktabelle.");

        const auto excessive_gap = returned_table_values(
            returned_table_load_image({mov_r0_r12, nop},
                                      mov_l_at_r12_r3,
                                      {0x70u},
                                      {{0x70u, 0u},
                                       {0x74u, 1u},
                                       {0x78u, 3u},
                                       {0x7Cu, 0xC0u}}));
        require(excessive_gap.guarded_code_inventory.returned_code_address_tables.empty(),
                "Drei aufeinanderfolgende unbelegte Slots wurden ueber das enge "
                "Callbackfenster hinweg geraten.");

        std::vector<std::pair<std::uint32_t, std::uint32_t>> twelve_slots;
        std::vector<std::uint32_t> twelve_targets;
        for (std::uint32_t index = 0u; index < 12u; ++index) {
            const auto target = 0x400u + index * 4u;
            twelve_slots.emplace_back(0x70u + index * 4u, target);
            twelve_targets.push_back(target);
        }
        const auto twelve_entry_table = returned_table_values(
            returned_table_load_image(
                {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, twelve_slots));
        const auto* twelve_entry_candidate =
            returned_table_candidate(twelve_entry_table, 0x70u);
        require(twelve_entry_candidate != nullptr &&
                    twelve_entry_candidate->target_addresses == twelve_targets &&
                    !twelve_entry_candidate->scan_truncated,
                "Returned-Callbacktabellen blieben auf acht Slots begrenzt.");

        std::vector<std::pair<std::uint32_t, std::uint32_t>> sixty_five_slots;
        for (std::uint32_t index = 0u; index < 65u; ++index)
            sixty_five_slots.emplace_back(0x70u + index * 4u, 0x400u + index * 4u);
        const auto bounded_table = returned_table_values(
            returned_table_load_image(
                {mov_r0_r12, nop}, mov_l_at_r12_r3, {0x70u}, sixty_five_slots));
        const auto* bounded_candidate = returned_table_candidate(bounded_table, 0x70u);
        require(bounded_candidate != nullptr &&
                    bounded_candidate->target_addresses.size() == 64u &&
                    bounded_candidate->scan_truncated &&
                    bounded_table.guarded_code_inventory.table_scan_truncated,
                "Begrenzter Returned-Tabellenscan meldete seine Truncation nicht "
                "maschinenlesbar.");

        const std::vector<std::uint32_t> too_many_returned_bases{
            0x60u, 0x64u, 0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu};
        const auto excessive_cartesian = returned_table_values(
            returned_table_load_image({mov_r0_r12, movt_r0, shll2_r0},
                                      mov_l_at_r0_r12_r3,
                                      too_many_returned_bases,
                                      {{0x60u, 0xC0u}}));
        require(
            excessive_cartesian.guarded_code_inventory.returned_code_address_tables.empty(),
                "Kartesische Effektivadressen ueberschritten das bestehende "
                "Achtkandidatenlimit.");

        std::vector<std::uint8_t> local_bytes(0xE0u, 0x09u);
        const auto put_local_u16 = [&local_bytes](const std::size_t offset,
                                                  const std::uint16_t value) {
            local_bytes[offset] = static_cast<std::uint8_t>(value);
            local_bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto put_local_u32 = [&local_bytes](const std::size_t offset,
                                                  const std::uint32_t value) {
            local_bytes[offset] = static_cast<std::uint8_t>(value);
            local_bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
            local_bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
            local_bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
        };
        put_local_u16(0x00u, 0xEC70u); // mov #0x70,r12
        put_local_u16(0x02u, mov_l_at_r12_r3);
        put_local_u16(0x04u, 0x000Bu);
        put_local_u16(0x06u, nop);
        put_local_u32(0x70u, 0xC0u);
        put_local_u16(0xC0u, 0x000Bu);
        put_local_u16(0xC2u, nop);
        katana::io::ExecutableImage local_image;
        local_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        local_image.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        local_image.add_segment({".local-table",
                                 0u,
                                 0u,
                                 local_bytes.size(),
                                 katana::io::SegmentKind::Mixed,
                                 {true, true, true},
                                 std::move(local_bytes),
                                 katana::io::ImageSourceKind::DiscBootFile,
                                 katana::io::ImageLoadPhase::Initial,
                                 "synthetic-local-table"});
        local_image.add_entry_point(0u);
        const auto local_lines =
            katana::sh4::disassemble(local_image.segments().front().bytes, 0u);
        constexpr std::array<std::uint32_t, 1u> local_entries{0u};
        const auto local_values = katana::analysis::analyze_function_values(
            local_image, local_lines, local_entries);
        require(local_values.guarded_code_inventory.returned_code_address_tables.empty(),
                "Lokaler Tabellenzeiger ohne Call-/Return-Provenienz wurde als "
                "Returned-Callbacktabelle akzeptiert.");
    }();

    [] {
        const auto multi_image =
            image_with_callee({// bt 0x28; mov #0x10,r0; rts; nop; mov #0x14,r0; rts; nop
                               0x02u,
                               0x89u,
                               0x10u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u,
                               0x14u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u});
        const auto multi = katana::analysis::analyze_control_flow(multi_image);
        const auto* multi_site = site(multi, 4u);
        require(multi_site != nullptr &&
                    multi_site->status == katana::analysis::ResolutionStatus::Resolved &&
                    !multi_site->target.has_value() &&
                    multi_site->targets == std::vector<std::uint32_t>({0x10u, 0x14u}) &&
                    multi_site->reason == "interprocedural-return-set",
                "Mehrwertige Return-Summary wurde nicht als endliche Zielmenge aufgeloest.");
        require(std::count_if(multi.resolved_edges.begin(),
                              multi.resolved_edges.end(),
                              [](const auto& edge) { return edge.instruction_address == 4u; }) == 2,
                "Mehrwertige Return-Summary erzeugte nicht genau zwei CFG-Kanten.");
        const auto multi_text = katana::analysis::format_indirect_control_flow_report(
            multi.indirect_control_flow, multi.jump_tables, multi.symbolic_addresses);
        const auto multi_json = katana::analysis::format_control_flow_analysis_json(multi);
        require(
            multi_text.find("interprocedural-return-set; evidence=proven-complete; r0; callees=") !=
                    std::string::npos &&
                multi_json.find("\"targets\":[\"0x00000010\",\"0x00000014\"]") !=
                    std::string::npos &&
                multi_json.find("\"function_value_summaries\":[") != std::string::npos &&
                multi_json.find("\"candidate_inventory_truncated\":false") !=
                    std::string::npos &&
                multi_json.find("\"returned_table_scan_truncated\":false") !=
                    std::string::npos,
            "Mehrziel- oder Summary-Evidenz fehlt im Text-/JSON-Bericht.");

        const auto conflicting_image =
            image_with_callee({// bt 0x28; mov #0x10,r0; rts; nop; rts; nop
                               0x02u,
                               0x89u,
                               0x10u,
                               0xE0u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u,
                               0x0Bu,
                               0x00u,
                               0x09u,
                               0x00u});
        const auto conflicting = katana::analysis::analyze_control_flow(conflicting_image);
        const auto* conflicting_site = site(conflicting, 4u);
        require(conflicting_site != nullptr &&
                    conflicting_site->status == katana::analysis::ResolutionStatus::Unresolved &&
                    conflicting_site->reason == "dynamic-return-value",
                "Widerspruechlicher Return-Pfad wurde nicht sichtbar dynamisch gelassen.");
        const auto* conflicting_summary = summary(conflicting, 0x20u, 0u);
        require(conflicting_summary != nullptr && !conflicting_summary->complete &&
                    conflicting_summary->reason == "return-path-unknown",
                "Widerspruechliche Return-Summary wurde faelschlich als vollstaendig markiert.");

        const auto recursive_image = image_with_callee({// bsr 0x20; nop; rts; nop
                                                        0xFEu,
                                                        0xBFu,
                                                        0x09u,
                                                        0x00u,
                                                        0x0Bu,
                                                        0x00u,
                                                        0x09u,
                                                        0x00u});
        const auto recursive = katana::analysis::analyze_control_flow(recursive_image);
        const auto* recursive_site = site(recursive, 4u);
        require(recursive_site != nullptr &&
                    recursive_site->status == katana::analysis::ResolutionStatus::Unresolved &&
                    recursive_site->reason == "dynamic-return-value" &&
                    recursive_site->origin_class ==
                        katana::analysis::IndirectControlFlowOriginClass::Callback,
                "Rekursive Summary ohne stabilen Return wurde als Zielbeweis verwendet.");

        const auto missing_return_image = image_with_callee({0x09u, 0x00u});
        const auto missing_return = katana::analysis::analyze_control_flow(missing_return_image);
        const auto* missing_return_summary = summary(missing_return, 0x20u, 0u);
        require(missing_return_summary != nullptr && !missing_return_summary->complete &&
                    missing_return_summary->reason == "no-return" &&
                    site(missing_return, 4u)->reason == "dynamic-return-value",
                "Callee ohne Return wurde nicht konservativ als unbekannt klassifiziert.");
    }();

    auto abi_less_image = unique_image;
    abi_less_image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    const auto abi_less = katana::analysis::analyze_control_flow(abi_less_image);
    const auto* abi_less_site = site(abi_less, 4u);
    require(abi_less.function_value_summaries.empty() && abi_less_site != nullptr &&
                abi_less_site->status == katana::analysis::ResolutionStatus::Unresolved,
            "ABI-loses Image erhielt eine SH-C-Return-Summary.");

    const auto parameter =
        katana::analysis::analyze_control_flow(classification_image({0x2Bu, 0x44u, 0x09u, 0x00u}));
    require(site(parameter, 0u)->reason == "dynamic-parameter" &&
                site(parameter, 0u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Parameter,
            "Offener Parameter-Call wurde nicht getrennt klassifiziert.");
    const auto stack = katana::analysis::analyze_control_flow(
        classification_image({0xF2u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(stack, 2u)->reason == "dynamic-stack-target" &&
                site(stack, 2u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Stack,
            "Offenes Stackziel wurde nicht getrennt klassifiziert.");
    const auto unbounded = katana::analysis::analyze_control_flow(
        classification_image({0x22u, 0x61u, 0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(unbounded, 2u)->reason == "dynamic-unbounded-memory" &&
                site(unbounded, 2u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::UnboundedMemory,
            "Unbeschraenkter Speicherzeiger wurde nicht getrennt klassifiziert.");
    const auto vtable = katana::analysis::analyze_control_flow(
        classification_image({0x42u, 0x61u, 0x12u, 0x62u, 0x2Bu, 0x42u, 0x09u, 0x00u}));
    require(site(vtable, 4u)->reason == "dynamic-vtable-target" &&
                site(vtable, 4u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::ObjectVTable,
            "Offenes VTable-Ziel wurde nicht getrennt klassifiziert.");
    const auto runtime_pointer =
        katana::analysis::analyze_control_flow(classification_image({0x2Bu, 0x41u, 0x09u, 0x00u}));
    require(site(runtime_pointer, 0u)->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::RuntimePointer &&
                site(runtime_pointer, 0u)->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                katana::analysis::control_flow_report_status(*site(runtime_pointer, 0u)) ==
                    katana::analysis::ControlFlowReportStatus::RuntimeOnly &&
                site(runtime_pointer, 0u)->reason ==
                    "dynamic-runtime-pointer-register-value-unknown" &&
                site(runtime_pointer, 0u)->evidence_origins ==
                    std::vector{katana::analysis::AnalysisEvidenceOrigin::RuntimeClassification},
            "Allgemeiner unbekannter Zeiger besitzt keinen validierten Runtimevertrag: " +
                std::to_string(static_cast<int>(site(runtime_pointer, 0u)->origin_class)) + "/" +
                std::to_string(static_cast<int>(site(runtime_pointer, 0u)->evidence)) + "/" +
                std::to_string(static_cast<int>(
                    katana::analysis::control_flow_report_status(*site(runtime_pointer, 0u)))) +
                "/" + site(runtime_pointer, 0u)->reason + "/" +
                std::to_string(site(runtime_pointer, 0u)->evidence_origins.size()));
    const auto runtime_pointer_json =
        katana::analysis::format_control_flow_analysis_json(runtime_pointer);
    require(runtime_pointer_json.find("\"instruction_form\":\"Jmp\"") != std::string::npos &&
                runtime_pointer_json.find("\"definition_complete\":false") != std::string::npos &&
                runtime_pointer_json.find("\"preceding_call\":false") != std::string::npos,
            "Der Sitebericht verliert Instruktionsform oder Definitionsprovenienz.");

    std::vector<std::uint8_t> indexed_slice_bytes(24u, 0u);
    for (std::size_t index = 0u; index < indexed_slice_bytes.size(); index += 2u)
        indexed_slice_bytes[index] = 0x09u; // nop
    const std::array<std::uint8_t, 12u> joined_slice{
        0x22u, 0x61u, // mov.l @r2,r1
        0x01u, 0x89u, // bt 0x8
        0x09u, 0x00u, // nop
        0x09u, 0x00u, // nop
        0x2Bu, 0x41u, // jmp @r1
        0x09u, 0x00u  // nop (delay)
    };
    const std::array<std::uint8_t, 6u> disjoint_slice{
        0xF2u, 0x63u, // mov.l @r15,r3
        0x2Bu, 0x43u, // jmp @r3
        0x09u, 0x00u  // nop (delay)
    };
    std::copy(joined_slice.begin(), joined_slice.end(), indexed_slice_bytes.begin());
    std::copy(disjoint_slice.begin(),
              disjoint_slice.end(),
              indexed_slice_bytes.begin() + 16u);
    katana::io::ExecutableImage indexed_slice_image;
    indexed_slice_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    indexed_slice_image.add_segment({".text",
                                     0u,
                                     0u,
                                     indexed_slice_bytes.size(),
                                     katana::io::SegmentKind::Code,
                                     {true, false, true},
                                     std::move(indexed_slice_bytes)});
    indexed_slice_image.add_entry_point(0u);
    indexed_slice_image.add_entry_point(16u);
    const auto indexed_slices = katana::analysis::analyze_control_flow(indexed_slice_image);
    const auto* joined_site = site(indexed_slices, 8u);
    const auto* disjoint_site = site(indexed_slices, 18u);
    require(joined_site != nullptr && joined_site->definition_complete &&
                joined_site->definition_sites == std::vector<std::uint32_t>{0u} &&
                joined_site->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::UnboundedMemory,
            "Writer-Slice-Index verliert die gemeinsame Definition am CFG-Join.");
    require(disjoint_site != nullptr && disjoint_site->definition_complete &&
                disjoint_site->definition_sites == std::vector<std::uint32_t>{16u} &&
                disjoint_site->origin_class ==
                    katana::analysis::IndirectControlFlowOriginClass::Stack,
            "Writer-Slice-Index verwechselt Definitionen getrennter Entry-Bloecke.");

    std::vector<std::uint8_t> guarded_join_bytes(0x40u, 0x09u);
    const std::array<std::uint8_t, 20u> guarded_join_code{
        0x05u, 0xDCu, // mov.l @(0x18,pc),r12
        0x01u, 0x89u, // bt 0x8
        0x09u, 0x00u, // nop
        0x09u, 0x00u, // nop
        0x0Au, 0xB0u, // bsr 0x20
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x4Cu, // jsr @r12
        0x09u, 0x00u, // nop (delay)
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // nop (delay)
    };
    std::copy(guarded_join_code.begin(), guarded_join_code.end(), guarded_join_bytes.begin());
    guarded_join_bytes[0x18u] = 0x30u;
    guarded_join_bytes[0x19u] = 0x00u;
    guarded_join_bytes[0x1Au] = 0x00u;
    guarded_join_bytes[0x1Bu] = 0x00u;
    guarded_join_bytes[0x20u] = 0x0Bu;
    guarded_join_bytes[0x22u] = 0x09u;
    guarded_join_bytes[0x30u] = 0x0Bu;
    guarded_join_bytes[0x32u] = 0x09u;
    katana::io::ExecutableImage guarded_join_image;
    guarded_join_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    guarded_join_image.add_segment({".rwx",
                                    0u,
                                    0u,
                                    guarded_join_bytes.size(),
                                    katana::io::SegmentKind::Code,
                                    {true, true, true},
                                    std::move(guarded_join_bytes)});
    guarded_join_image.add_entry_point(0u);
    const auto guarded_join = katana::analysis::analyze_control_flow(guarded_join_image);
    const auto* guarded_join_site = site(guarded_join, 0x0Cu);
    const auto guarded_join_edge =
        std::find_if(guarded_join.resolved_edges.begin(),
                     guarded_join.resolved_edges.end(),
                     [](const auto& edge) {
                         return edge.instruction_address == 0x0Cu && edge.target_address == 0x30u;
                     });
    require(guarded_join_site != nullptr &&
                guarded_join_site->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                !guarded_join_site->target.has_value() && guarded_join_site->targets.empty() &&
                guarded_join_site->analysis_candidates == std::vector<std::uint32_t>{0x30u} &&
                guarded_join_site->reason == "runtime-contract-function-memory" &&
                guarded_join_edge == guarded_join.resolved_edges.end(),
            "CFG-Join fror einen veraenderlichen Speicherkandidaten statisch ein.");
    const auto guarded_join_ir = katana::ir::lower_program(guarded_join);
    const katana::ir::Instruction* guarded_join_ir_site = nullptr;
    for (const auto& function : guarded_join_ir)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.source_address == 0x0Cu) guarded_join_ir_site = &instruction;
    require(guarded_join_ir_site != nullptr &&
                guarded_join_ir_site->dynamic_target_class ==
                    katana::ir::DynamicTargetClass::RuntimeOnly &&
                guarded_join_ir_site->resolved_targets.empty() &&
                katana::ir::verify_program(guarded_join_ir).empty(),
            "Veraenderlicher Funktionsspeicher erreicht nicht kandidatenfrei den Runtimevertrag.");

    std::vector<std::uint8_t> parameter_candidate_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 10u> parameter_caller{
        0x40u,
        0xE4u, // mov #0x40,r4
        0x0Du,
        0xB0u, // bsr 0x20
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    const std::array<std::uint8_t, 12u> parameter_callee{
        0x42u,
        0x61u, // mov.l @r4,r1
        0x0Bu,
        0x41u, // jsr @r1
        0x09u,
        0x00u, // nop (delay)
        0x00u,
        0xE0u, // mov #0,r0: complete return must not suppress context
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(parameter_caller.begin(), parameter_caller.end(), parameter_candidate_bytes.begin());
    std::copy(parameter_callee.begin(),
              parameter_callee.end(),
              parameter_candidate_bytes.begin() + 0x20u);
    parameter_candidate_bytes[0x40u] = 0x50u;
    parameter_candidate_bytes[0x41u] = 0x00u;
    parameter_candidate_bytes[0x42u] = 0x00u;
    parameter_candidate_bytes[0x43u] = 0x00u;
    parameter_candidate_bytes[0x50u] = 0x0Bu;
    parameter_candidate_bytes[0x51u] = 0x00u;
    parameter_candidate_bytes[0x52u] = 0x09u;
    parameter_candidate_bytes[0x53u] = 0x00u;
    katana::io::ExecutableImage parameter_candidate_image;
    parameter_candidate_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    parameter_candidate_image.add_segment({".rwx",
                                           0u,
                                           0u,
                                           parameter_candidate_bytes.size(),
                                           katana::io::SegmentKind::Code,
                                           {true, true, true},
                                           std::move(parameter_candidate_bytes)});
    parameter_candidate_image.add_entry_point(0u);
    const auto parameter_candidate =
        katana::analysis::analyze_control_flow(parameter_candidate_image);
    const auto* parameter_candidate_site = site(parameter_candidate, 0x22u);
    require(parameter_candidate_site != nullptr &&
                parameter_candidate_site->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                parameter_candidate_site->analysis_candidates ==
                    std::vector<std::uint32_t>{0x50u} &&
                parameter_candidate_site->reason == "runtime-contract-function-memory" &&
                parameter_candidate_site->evidence_call_sites.empty(),
            "Direkter Call verlor seinen Parameterkandidaten oder uebertrug "
            "Objektadress-Provenienz auf den geladenen Funktionswert.");

    auto unknown_caller_image = parameter_candidate_image;
    unknown_caller_image.add_entry_point(0x20u);
    const auto unknown_caller = katana::analysis::analyze_control_flow(unknown_caller_image);
    const auto* unknown_caller_site = site(unknown_caller, 0x22u);
    require(unknown_caller_site != nullptr &&
                !katana::analysis::control_flow_evidence_complete(
                    unknown_caller_site->evidence) &&
                !unknown_caller_site->target.has_value() &&
                unknown_caller_site->targets.empty() &&
                unknown_caller_site->analysis_candidates.empty() &&
                std::none_of(unknown_caller.resolved_edges.begin(),
                             unknown_caller.resolved_edges.end(),
                             [](const auto& edge) {
                                 return edge.instruction_address == 0x22u &&
                                        edge.target_address == 0x50u;
                             }),
            "Partielle Callerwerte veraenderten trotz unbekanntem Ingress "
            "globale Resolutions oder CFG-Kanten.");

    std::vector<std::uint8_t> indirect_parameter_bytes(0x60u, 0x09u);
    const std::array<std::uint8_t, 18u> indirect_parameter_caller{
        0x40u,
        0xE4u, // mov #0x40,r4
        0x29u,
        0x00u, // movt r0 -> {0,1}
        0x08u,
        0x40u, // shll2 r0 -> {0,4}
        0x0Cu,
        0x34u, // add r0,r4 -> {0x40,0x44}
        0x04u,
        0xDCu, // mov.l @(0x1c,pc),r12
        0x0Bu,
        0x4Cu, // jsr @r12
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(indirect_parameter_caller.begin(),
              indirect_parameter_caller.end(),
              indirect_parameter_bytes.begin());
    std::copy(
        parameter_callee.begin(), parameter_callee.end(), indirect_parameter_bytes.begin() + 0x20u);
    indirect_parameter_bytes[0x1Cu] = 0x20u;
    indirect_parameter_bytes[0x1Du] = 0x00u;
    indirect_parameter_bytes[0x1Eu] = 0x00u;
    indirect_parameter_bytes[0x1Fu] = 0x00u;
    indirect_parameter_bytes[0x40u] = 0x60u;
    indirect_parameter_bytes[0x41u] = 0x00u;
    indirect_parameter_bytes[0x42u] = 0x00u;
    indirect_parameter_bytes[0x43u] = 0x00u;
    indirect_parameter_bytes[0x44u] = 0x50u;
    indirect_parameter_bytes[0x45u] = 0x00u;
    indirect_parameter_bytes[0x46u] = 0x00u;
    indirect_parameter_bytes[0x47u] = 0x00u;
    indirect_parameter_bytes[0x50u] = 0x0Bu;
    indirect_parameter_bytes[0x51u] = 0x00u;
    indirect_parameter_bytes[0x52u] = 0x09u;
    indirect_parameter_bytes[0x53u] = 0x00u;
    katana::io::ExecutableImage indirect_parameter_image;
    indirect_parameter_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    indirect_parameter_image.add_segment({".rwx",
                                          0u,
                                          0u,
                                          indirect_parameter_bytes.size(),
                                          katana::io::SegmentKind::Code,
                                          {true, true, true},
                                          std::move(indirect_parameter_bytes)});
    indirect_parameter_image.add_entry_point(0u);
    const auto indirect_parameter =
        katana::analysis::analyze_control_flow(indirect_parameter_image);
    const auto* indirect_parameter_site = site(indirect_parameter, 0x22u);
    require(indirect_parameter_site != nullptr &&
                indirect_parameter_site->evidence ==
                    katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                indirect_parameter_site->analysis_candidates == std::vector<std::uint32_t>{0x50u} &&
                indirect_parameter_site->evidence_call_sites.empty(),
            "Bewachter indirekter Call verlor seinen gueltigen Parameterkandidaten "
            "neben einer ungueltigen Alternative oder "
            "uebertrug Objektadress-Provenienz auf den geladenen Funktionswert.");

    std::vector<std::uint8_t> finite_index_bytes(0x38u, 0x09u);
    const std::array<std::uint8_t, 16u> finite_index_code{
        0x29u,
        0x00u, // movt r0 -> {0,1}
        0x08u,
        0x40u, // shll2 r0 -> {0,4}
        0x02u,
        0xD1u, // mov.l @(0x10,pc),r1
        0x1Eu,
        0x02u, // mov.l @(r0,r1),r2
        0x0Bu,
        0x42u, // jsr @r2
        0x09u,
        0x00u, // nop (delay)
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // nop (delay)
    };
    std::copy(finite_index_code.begin(), finite_index_code.end(), finite_index_bytes.begin());
    finite_index_bytes[0x10u] = 0x18u;
    finite_index_bytes[0x11u] = 0x00u;
    finite_index_bytes[0x12u] = 0x00u;
    finite_index_bytes[0x13u] = 0x00u;
    finite_index_bytes[0x18u] = 0x30u;
    finite_index_bytes[0x19u] = 0x00u;
    finite_index_bytes[0x1Au] = 0x00u;
    finite_index_bytes[0x1Bu] = 0x00u;
    finite_index_bytes[0x1Cu] = 0x34u;
    finite_index_bytes[0x1Du] = 0x00u;
    finite_index_bytes[0x1Eu] = 0x00u;
    finite_index_bytes[0x1Fu] = 0x00u;
    finite_index_bytes[0x30u] = 0x0Bu;
    finite_index_bytes[0x31u] = 0x00u;
    finite_index_bytes[0x32u] = 0x09u;
    finite_index_bytes[0x33u] = 0x00u;
    finite_index_bytes[0x34u] = 0x0Bu;
    finite_index_bytes[0x35u] = 0x00u;
    finite_index_bytes[0x36u] = 0x09u;
    finite_index_bytes[0x37u] = 0x00u;
    katana::io::ExecutableImage finite_index_image;
    finite_index_image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    finite_index_image.add_segment({".rwx",
                                    0u,
                                    0u,
                                    finite_index_bytes.size(),
                                    katana::io::SegmentKind::Code,
                                    {true, true, true},
                                    std::move(finite_index_bytes)});
    finite_index_image.add_entry_point(0u);
    const auto finite_index = katana::analysis::analyze_control_flow(finite_index_image);
    const auto* finite_index_site = site(finite_index, 0x08u);
    require(finite_index_site != nullptr &&
                finite_index_site->evidence == katana::analysis::ControlFlowEvidence::RuntimeOnly &&
                finite_index_site->targets.empty() &&
                finite_index_site->analysis_candidates ==
                    std::vector<std::uint32_t>({0x30u, 0x34u}) &&
                finite_index_site->reason == "runtime-contract-function-memory",
            "Endlicher, veraenderlicher Indexpfad wurde als vollstaendige Zielmenge eingefroren.");
    require(
        finite_index.function_scc_count != 0u && finite_index.function_summary_iterations != 0u &&
            finite_index.instruction_arena != nullptr &&
            finite_index.instruction_arena->size() == finite_index.recursive.instructions.size() &&
            !finite_index.block_spans.empty() && finite_index.evidence_ids.size() != 0u,
        "SCC-Summaries, immutable Arena, Blockspans oder Evidence-Interning fehlen.");

    [] {
        std::vector<std::uint8_t> stack_spill_bytes(0x24u, 0x09u);
        const std::array<std::uint8_t, 14u> stack_spill_code{
            0x03u,
            0xD8u, // mov.l @(0x10,pc),r8
            0x86u,
            0x2Fu, // mov.l r8,@-r15
            0xF6u,
            0x6Du, // mov.l @r15+,r13
            0x0Bu,
            0x4Du, // jsr @r13
            0x09u,
            0x00u, // nop (delay)
            0x0Bu,
            0x00u, // rts
            0x09u,
            0x00u // nop (delay)
        };
        std::copy(stack_spill_code.begin(), stack_spill_code.end(), stack_spill_bytes.begin());
        stack_spill_bytes[0x10u] = 0x20u;
        stack_spill_bytes[0x11u] = 0x00u;
        stack_spill_bytes[0x12u] = 0x00u;
        stack_spill_bytes[0x13u] = 0x00u;
        stack_spill_bytes[0x20u] = 0x0Bu;
        stack_spill_bytes[0x21u] = 0x00u;
        stack_spill_bytes[0x22u] = 0x09u;
        stack_spill_bytes[0x23u] = 0x00u;
        const auto stack_spill = katana::analysis::analyze_control_flow(
            classification_image(std::move(stack_spill_bytes)));
        const auto* stack_spill_site = site(stack_spill, 0x06u);
        if (stack_spill_site == nullptr) {
            require(false, "Stackspill/Reload-Callsite fehlt.");
            return;
        }
        require(stack_spill_site->target == 0x20u,
                "Fester Stackspill/Reload verliert sein R13-Ziel.");
        require(stack_spill_site->status == katana::analysis::ResolutionStatus::Resolved,
                "Fester Stackspill/Reload verliert seinen vollstaendigen Beweis: Status " +
                    std::to_string(static_cast<int>(stack_spill_site->status)) + ", Evidenz " +
                    std::to_string(static_cast<int>(stack_spill_site->evidence)) + ", Grund " +
                    stack_spill_site->reason + ".");
    }();

    [] {
        const auto object_image = [](const bool invalidate_with_byte,
                                     const bool invalidate_with_prefetch) {
            std::vector<std::uint8_t> text(0x34u, 0x09u);
            text[0x00u] = 0x07u;
            text[0x01u] = 0xD4u; // mov.l @(0x20,pc),r4 -> Objekt 0x40
            text[0x02u] = 0x08u;
            text[0x03u] = 0xD1u; // mov.l @(0x24,pc),r1 -> Callback 0x30
            text[0x04u] = 0x12u;
            text[0x05u] = 0x24u; // mov.l r1,@r4
            std::size_t cursor = 0x06u;
            if (invalidate_with_byte) {
                text[cursor++] = 0x00u;
                text[cursor++] = 0x24u; // mov.b r0,@r4 ueberlappt das Feld
            } else if (invalidate_with_prefetch) {
                text[cursor++] = 0x83u;
                text[cursor++] = 0x04u; // pref @r4 invalidiert unbekannte Mutation
            }
            const auto load_address = cursor;
            text[cursor++] = 0x42u;
            text[cursor++] = 0x62u; // mov.l @r4,r2
            const auto call_address = cursor;
            text[cursor++] = 0x0Bu;
            text[cursor++] = 0x42u; // jsr @r2
            text[cursor++] = 0x09u;
            text[cursor++] = 0x00u;
            text[cursor++] = 0x0Bu;
            text[cursor++] = 0x00u;
            text[cursor++] = 0x09u;
            text[cursor++] = 0x00u;
            text[0x20u] = 0x40u;
            text[0x21u] = 0x00u;
            text[0x22u] = 0x00u;
            text[0x23u] = 0x00u;
            text[0x24u] = 0x30u;
            text[0x25u] = 0x00u;
            text[0x26u] = 0x00u;
            text[0x27u] = 0x00u;
            text[0x30u] = 0x0Bu;
            text[0x31u] = 0x00u;
            text[0x32u] = 0x09u;
            text[0x33u] = 0x00u;
            katana::io::ExecutableImage image;
            image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
            image.add_segment({".text",
                               0u,
                               0u,
                               text.size(),
                               katana::io::SegmentKind::Code,
                               {true, false, true},
                               std::move(text)});
            image.add_segment({".object",
                               0x40u,
                               0x34u,
                               4u,
                               katana::io::SegmentKind::Data,
                               {true, true, false},
                               std::vector<std::uint8_t>(4u)});
            image.add_entry_point(0u);
            return std::pair{std::move(image),
                             std::pair{static_cast<std::uint32_t>(load_address),
                                       static_cast<std::uint32_t>(call_address)}};
        };
        auto [dominant_object_image, dominant_addresses] = object_image(false, false);
        const auto dominant_object = katana::analysis::analyze_control_flow(dominant_object_image);
        const auto* dominant_site = site(dominant_object, dominant_addresses.second);
        require(dominant_site != nullptr && dominant_site->target == 0x30u &&
                    dominant_site->status == katana::analysis::ResolutionStatus::Guarded &&
                    dominant_site->evidence ==
                        katana::analysis::ControlFlowEvidence::GuardedComplete &&
                    dominant_site->origin_class ==
                        katana::analysis::IndirectControlFlowOriginClass::ObjectVTable &&
                    dominant_object.function_summary_iterations <=
                        dominant_object.function_iteration_budget &&
                    !dominant_object.function_budget_exhausted,
                "Dominanter Objektfeldstore erzeugt keine begrenzte vollstaendige Zielmenge.");
        auto [overlap_image, overlap_addresses] = object_image(true, false);
        const auto overlap = katana::analysis::analyze_control_flow(overlap_image);
        require(site(overlap, overlap_addresses.second) != nullptr &&
                    !katana::analysis::control_flow_evidence_complete(
                        site(overlap, overlap_addresses.second)->evidence),
                "Ueberlappender Teilstore laesst einen stale Objektfeldbeweis bestehen.");
        auto [prefetch_image, prefetch_addresses] = object_image(false, true);
        const auto prefetch = katana::analysis::analyze_control_flow(prefetch_image);
        require(site(prefetch, prefetch_addresses.second) != nullptr &&
                    !katana::analysis::control_flow_evidence_complete(
                        site(prefetch, prefetch_addresses.second)->evidence),
                "PREF laesst einen stale Objektfeldbeweis bestehen.");
    }();

    {
        constexpr std::size_t inventory_budget = 1'024u;
        constexpr std::uint32_t handler_base = 0x1'0000u;
        const auto exact_budget =
            guarded_inventory_budget_values(inventory_budget);
        require(
            exact_budget.guarded_code_inventory.candidate_budget ==
                    inventory_budget &&
                exact_budget.guarded_code_inventory.candidate_count ==
                    inventory_budget &&
                !exact_budget.guarded_code_inventory
                     .candidate_inventory_truncated &&
                exact_budget.guarded_code_inventory.stored_code_addresses
                        .size() == inventory_budget &&
                std::all_of(
                    exact_budget.guarded_code_inventory.stored_code_addresses
                        .begin(),
                    exact_budget.guarded_code_inventory.stored_code_addresses
                        .end(),
                    [](const auto& candidate) { return candidate.guarded; }),
            "Exakt 1.024 eindeutige Guarded-Code-Ziele verletzten Budget, "
            "Zaehler oder Guard-Vertrag.");

        const auto over_budget =
            guarded_inventory_budget_values(inventory_budget + 1u);
        const auto& retained =
            over_budget.guarded_code_inventory.stored_code_addresses;
        const auto deterministic_prefix =
            retained.size() == inventory_budget &&
            std::all_of(
                retained.begin(),
                retained.end(),
                [&](const auto& candidate) {
                    const auto index = static_cast<std::size_t>(
                        &candidate - retained.data());
                    return candidate.target_address ==
                           handler_base +
                               static_cast<std::uint32_t>(index * 4u);
                });
        require(
            over_budget.guarded_code_inventory.candidate_budget ==
                    inventory_budget &&
                over_budget.guarded_code_inventory.candidate_count ==
                    inventory_budget &&
                over_budget.guarded_code_inventory
                    .candidate_inventory_truncated &&
                deterministic_prefix &&
                std::none_of(
                    retained.begin(),
                    retained.end(),
                    [](const auto& candidate) {
                        return candidate.target_address ==
                               handler_base +
                                   static_cast<std::uint32_t>(
                                       inventory_budget * 4u);
                    }),
            "Das 1.025. eindeutige Guarded-Code-Ziel wurde nicht waehrend "
            "der Sammlung mit korrekter Truncation und stabilem Prefix "
            "abgewiesen.");

        const auto returned_method_priority =
            guarded_inventory_budget_values(inventory_budget, true);
        const auto& prioritized_inventory =
            returned_method_priority.guarded_code_inventory;
        const auto returned_handler =
            handler_base +
            static_cast<std::uint32_t>(inventory_budget * 4u);
        const auto prioritized_stored_prefix =
            prioritized_inventory.stored_code_addresses.size() ==
                inventory_budget - 1u &&
            std::all_of(
                prioritized_inventory.stored_code_addresses.begin(),
                prioritized_inventory.stored_code_addresses.end(),
                [&](const auto& candidate) {
                    const auto index = static_cast<std::size_t>(
                        &candidate -
                        prioritized_inventory.stored_code_addresses.data());
                    return candidate.target_address ==
                           handler_base +
                               static_cast<std::uint32_t>(index * 4u);
                });
        require(
            prioritized_inventory.candidate_budget == inventory_budget &&
                prioritized_inventory.candidate_count == inventory_budget &&
                prioritized_inventory.candidate_inventory_truncated &&
                prioritized_inventory.returned_code_address_tables.size() ==
                    1u &&
                prioritized_inventory.returned_code_address_tables.front()
                        .target_addresses ==
                    std::vector<std::uint32_t>{returned_handler} &&
                prioritized_stored_prefix,
            "Konkrete Accessor-Returned-Methodentabelle verlor gegen das "
            "breite Stored-Inventar oder verdraengte keinen deterministischen "
            "Stored-Suffix (candidate_count=" +
                std::to_string(prioritized_inventory.candidate_count) +
                ", truncated=" +
                std::to_string(
                    prioritized_inventory.candidate_inventory_truncated) +
                ", stored=" +
                std::to_string(
                    prioritized_inventory.stored_code_addresses.size()) +
                ", tables=" +
                std::to_string(
                    prioritized_inventory.returned_code_address_tables.size()) +
                ", table_targets=" +
                std::to_string(
                    prioritized_inventory.returned_code_address_tables.empty()
                        ? 0u
                        : prioritized_inventory.returned_code_address_tables
                              .front()
                              .target_addresses.size()) +
                ", prefix=" +
                std::to_string(prioritized_stored_prefix) + ").");

        std::vector<
            katana::analysis::detail::GuardedCodeInventoryPriorityTarget>
            returned_table_pressure;
        returned_table_pressure.reserve(inventory_budget + 1u);
        for (std::size_t index = 0u; index < inventory_budget; ++index) {
            returned_table_pressure.push_back({
                static_cast<std::uint32_t>(0x80'000u + index * 4u),
                katana::analysis::detail::
                    GuardedCodeInventoryPriorityKind::
                        TruncatedReturnedTable});
        }
        constexpr std::uint32_t direct_stored_callback = 0x60'000u;
        returned_table_pressure.push_back({
            direct_stored_callback,
            katana::analysis::detail::
                GuardedCodeInventoryPriorityKind::CompleteStored});
        const auto pressure_order =
            katana::analysis::detail::
                guarded_code_inventory_priority_order(
                    returned_table_pressure, 256u);
        require(
            pressure_order.size() == inventory_budget + 1u &&
                pressure_order.front() == direct_stored_callback &&
                std::find(pressure_order.begin(),
                          pressure_order.begin() +
                              static_cast<std::ptrdiff_t>(
                                  inventory_budget),
                          direct_stored_callback) !=
                    pressure_order.begin() +
                        static_cast<std::ptrdiff_t>(inventory_budget),
            "Breite oder abgeschnittene Returned-Methodentabellen "
            "verdraengten einen direkt gespeicherten Callback aus dem "
            "Guarded-AOT-Inventar.");
    }

    std::cout << "KR-4713 interprozedurale Zielwertsummaries erfolgreich.\n";
    return EXIT_SUCCESS;
}
