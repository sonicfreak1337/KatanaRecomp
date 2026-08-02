#include "structured_control_flow_progress.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

katana::analysis::ControlFlowAnalysisProgress
resolution_progress(const std::size_t round,
                    const std::size_t candidate_iteration,
                    const std::string_view phase,
                    const std::size_t started,
                    const std::size_t ready,
                    const std::size_t committed) {
    katana::analysis::ControlFlowAnalysisProgress progress;
    progress.phase = phase;
    progress.iteration = round;
    progress.seeds = 1'400u;
    progress.instructions = 22'400u;
    progress.contexts = 84'000u;
    progress.resolutions = 1'400u;
    progress.candidate_contract_iteration =
        candidate_iteration;
    progress.candidate_contract_iteration_budget = 64u;
    progress.round_seed_baseline = 1'390u;
    progress.round_added_seeds = 10u;
    progress.round_seed_facts_added = 12u;
    progress.round_seed_targets_changed = 10u;
    progress.round_decode_targets = 8u;
    progress.round_metadata_targets = 2u;
    progress.round_full_cpu_fallbacks = 0u;
    progress.persistent_analysis_bypass_reason =
        katana::analysis::PersistentAnalysisBypassReason::
            ProgramDeltaUnrepresentable;
    progress.recursive_snapshot_epochs = 31u;
    progress.recursive_final_materializations = 32u;
    progress.recursive_physical_work.trusted_snapshot_validations = 33u;
    progress.recursive_physical_work.seed_arena_copy_items = 34u;
    progress.recursive_physical_work.seed_arena_copy_bytes = 35u;
    progress.recursive_physical_work.seed_arena_shift_items = 36u;
    progress.recursive_physical_work.seed_arena_shift_bytes = 37u;
    progress.recursive_physical_work.epoch_index_lookups = 132u;
    progress.recursive_physical_work.epoch_index_updates = 131u;
    progress.recursive_physical_work.terminal_epoch_fold_items = 133u;
    progress.recursive_physical_work.seed_contract_items_visited = 38u;
    progress.recursive_physical_work.decoded_work_items = 39u;
    progress.recursive_physical_work.canonical_context_updates = 40u;
    progress.recursive_physical_work.canonical_instruction_updates = 41u;
    progress.recursive_physical_work.canonical_function_updates = 42u;
    progress.recursive_physical_work.public_baseline_hash_bytes = 43u;
    progress.recursive_physical_work.public_baseline_copy_items = 44u;
    progress.recursive_physical_work.public_sort_items = 45u;
    progress.recursive_physical_work.public_materialized_items = 46u;
    progress.recursive_physical_work.public_materializations = 47u;
    progress.runtime_copy_instruction_visits = 48u;
    progress.runtime_copy_result_entries_visited = 135u;
    progress.runtime_copy_result_entries_rebuilt = 134u;
    progress.local_control_flow_instruction_visits = 49u;
    progress.local_control_flow_result_entries_visited = 137u;
    progress.local_control_flow_result_entries_rebuilt = 136u;
    progress.dispatch_index_entries_visited = 139u;
    progress.dispatch_index_entries_rebuilt = 138u;
    progress.jump_table_instruction_visits = 51u;
    progress.jump_table_result_entries_visited = 141u;
    progress.jump_table_result_entries_rebuilt = 140u;
    progress.function_boundary_entries_visited = 53u;
    progress.function_boundary_entries_rebuilt = 52u;
    progress.function_edge_family_entries_visited = 55u;
    progress.function_edge_family_entries_rebuilt = 54u;
    progress.function_edge_state_encode_items = 142u;
    progress.function_edge_state_copy_items = 143u;
    progress.function_edge_state_exact_compare_items = 144u;
    progress.result_index_copy_items = 145u;
    progress.result_index_sort_items = 146u;
    progress.result_index_materialized_items = 147u;
    progress.growing_workset = true;
    progress.function_value_active = true;
    progress.function_value_functions = 1'600u;
    progress.function_value_blocks = 22'400u;
    progress.function_value_iterations = 7u;
    progress.function_value_summarized_functions = 1'600u;
    progress.function_value_pending = 11u;
    progress.function_value_active_workers = 24u;
    progress.function_value_logical_evaluations = 13u;
    progress.function_value_physical_evaluations = 5u;
    progress.function_value_active_evaluation_requests = 7u;
    progress.function_value_evaluation_request_nanoseconds =
        8'400'000u;
    progress.function_value_maximum_evaluation_request_nanoseconds =
        1'000u;
    progress.function_value_cache_key_builds = 10u;
    progress.function_value_active_cache_key_builds = 2u;
    progress.function_value_cache_key_build_nanoseconds = 840'000u;
    progress.function_value_maximum_cache_key_build_nanoseconds = 500u;
    progress.function_value_cache_waits = 3u;
    progress.function_value_active_cache_waits = 1u;
    progress.function_value_cache_wait_nanoseconds = 2'000u;
    progress.function_value_maximum_cache_wait_nanoseconds = 1'500u;
    progress.function_value_cache_replays = 2u;
    progress.function_value_active_cache_replays = 1u;
    progress.function_value_cache_replay_nanoseconds = 1'000u;
    progress.function_value_maximum_cache_replay_nanoseconds = 1'000u;
    progress.function_value_active_physical_evaluations = 4u;
    progress.function_value_physical_evaluation_nanoseconds =
        8'000'000u;
    progress.function_value_maximum_physical_evaluation_nanoseconds =
        2'000u;
    progress.function_value_cache_commits = 5u;
    progress.function_value_active_cache_commits = 1u;
    progress.function_value_cache_commit_nanoseconds = 500u;
    progress.function_value_maximum_cache_commit_nanoseconds = 200u;
    progress.function_value_resolution_functions_total = 3u;
    progress.function_value_resolution_functions_started = started;
    progress.function_value_resolution_functions_ready = ready;
    progress.function_value_resolution_functions_committed = committed;
    progress.function_value_resolution_head_of_line_index =
        committed;
    progress
        .function_value_resolution_head_of_line_elapsed_milliseconds =
        12'345u;
    progress.function_value_configured_workers = 24u;
    progress.function_value_session_cache_lookups = 10u;
    progress.function_value_session_cache_ready_hits = 2u;
    progress
        .function_value_session_cache_in_flight_coalesces = 3u;
    progress.function_value_session_cache_hits = 5u;
    progress.function_value_session_cache_misses = 5u;
    progress.function_value_multi_root_context_requests = 5u;
    progress.function_value_multi_root_unique_contexts = 2u;
    progress.function_value_multi_root_ready_reuses = 2u;
    progress.function_value_multi_root_in_flight_reuses = 1u;
    progress.function_value_multi_root_provenance_links = 6u;
    progress.function_value_multi_root_retained_contexts = 2u;
    progress.function_value_multi_root_retained_payload_bytes =
        2'048u;
    progress.function_value_session_cache_evictions = 1u;
    progress.function_value_session_cache_entries = 8u;
    progress.function_value_session_cache_retained_payload_bytes =
        4'096u;
    progress.function_value_session_cache_miss_cold = 5u;
    progress.function_value_analysis_epochs_published = 1u;
    progress.function_value_analysis_epochs_discarded = 0u;
    progress.function_value_incremental_epochs_started = 1u;
    progress.function_value_resolution_root_artifacts_total = 3u;
    progress.function_value_resolution_root_artifacts_reused = 2u;
    progress.function_value_resolution_root_artifacts_recomputed = 1u;
    progress.function_value_resolution_root_artifacts_retained = 3u;
    progress.function_value_resolution_epoch_retained_bytes = 4'096u;
    progress.function_value_resolution_retention_limit_reason =
        katana::analysis::ResolutionRetentionLimitReason::None;
    progress.function_value_dirty_sccs = 1u;
    progress.function_value_dirty_functions = 2u;
    progress.function_value_dirty_inventory_sinks = 1u;
    progress.function_value_full_cpu_recompute_fallbacks = 0u;
    progress.function_value_persistent_analysis_bypass_reason =
        katana::analysis::PersistentAnalysisBypassReason::ExplicitTest;
    progress.function_value_program_delta_entries_visited = 61u;
    progress.function_value_function_edge_full_scans = 62u;
    progress.function_value_function_edge_full_sorts = 63u;
    progress.function_value_candidate_call_edge_full_scans = 64u;
    progress.function_value_candidate_call_edge_full_sorts = 65u;
    progress.function_value_candidate_tail_edge_full_scans = 66u;
    progress.function_value_candidate_tail_edge_full_sorts = 67u;
    progress.function_value_graph_blocks_built = 68u;
    progress.function_value_graph_blocks_reused = 69u;
    progress.function_value_graph_sccs_built = 70u;
    progress.function_value_graph_sccs_reused = 71u;
    progress.function_value_resolution_dependency_nodes_built = 72u;
    progress.function_value_resolution_dependency_nodes_reused = 73u;
    progress.function_value_resolution_dependency_sccs_built = 74u;
    progress.function_value_resolution_dependency_sccs_reused = 75u;
    progress.function_value_abi_contract_entries_visited = 77u;
    progress.function_value_abi_contract_entries_rebuilt = 76u;
    progress.function_value_summary_candidate_entries_visited = 79u;
    progress.function_value_summary_candidate_entries_rebuilt = 78u;
    progress.function_value_inventory_topology_entries_visited = 82u;
    progress.function_value_resolution_preparation_entries_visited = 83u;
    progress.function_value_final_materialized_blocks = 80u;
    progress.function_value_final_materialized_functions = 81u;
    return progress;
}

katana::analysis::ControlFlowAnalysisProgress
inactive_function_value_completion(
    const std::size_t round,
    const std::size_t candidate_iteration) {
    katana::analysis::ControlFlowAnalysisProgress progress;
    progress.phase = "function-values-complete";
    progress.iteration = round;
    progress.seeds = 1'400u;
    progress.instructions = 22'400u;
    progress.contexts = 84'000u;
    progress.resolutions = 1'400u;
    progress.candidate_contract_iteration =
        candidate_iteration;
    progress.candidate_contract_iteration_budget = 64u;
    progress.round_seed_baseline = 1'390u;
    progress.round_added_seeds = 10u;
    progress.round_seed_facts_added = 12u;
    progress.round_seed_targets_changed = 10u;
    progress.round_decode_targets = 8u;
    progress.round_metadata_targets = 2u;
    progress.round_full_cpu_fallbacks = 0u;
    progress.growing_workset = true;
    return progress;
}

} // namespace

int main() {
    const auto retained_ephemeral_phase = [] {
        const std::string ephemeral =
            "function-values-ephemeral-owned";
        return resolution_progress(
            1u, 1u, ephemeral, 0u, 0u, 0u);
    }();
    require(
        retained_ephemeral_phase.phase ==
            "function-values-ephemeral-owned",
        "Control-Flow-Progress behielt einen dangling string_view auf "
        "kurzlebigen Callsite-Text.");
    std::mutex events_mutex;
    std::vector<katana::ProgressEvent> events;
    const katana::ProgressReporter reporter(
        [&](const katana::ProgressEvent& event) {
            const std::lock_guard lock(events_mutex);
            events.push_back(event);
        },
        std::chrono::milliseconds(0),
        std::chrono::seconds(1));

    {
        katana::codegen::detail::StructuredControlFlowProgress
            progress(reporter, "synthetic-cold-build");
        auto abi_stack_progress = resolution_progress(
            1u,
            1u,
            "function-values-abi-stack-reads-progress",
            3u,
            2u,
            0u);
        abi_stack_progress.function_value_subphase =
            "abi-stack-reads";
        abi_stack_progress.function_value_subphase_planned = 5u;
        abi_stack_progress.function_value_subphase_processed = 2u;
        abi_stack_progress.function_value_subphase_queued = 3u;
        abi_stack_progress.function_value_subphase_iterations = 2u;
        progress.update(abi_stack_progress);
        abi_stack_progress.phase =
            "function-values-abi-stack-reads-complete";
        abi_stack_progress.function_value_subphase_processed = 5u;
        abi_stack_progress.function_value_subphase_queued = 0u;
        abi_stack_progress.function_value_subphase_iterations = 5u;
        progress.update(abi_stack_progress);
        auto inventory_sink_progress = resolution_progress(
            1u,
            1u,
            "function-values-inventory-region-sink-sources-complete",
            0u,
            0u,
            0u);
        inventory_sink_progress.function_value_subphase =
            "inventory-region-sink-sources";
        inventory_sink_progress.function_value_subphase_planned = 3u;
        inventory_sink_progress.function_value_subphase_processed = 3u;
        inventory_sink_progress.function_value_subphase_queued = 0u;
        inventory_sink_progress.function_value_subphase_iterations = 3u;
        progress.update(inventory_sink_progress);
        constexpr std::array<std::string_view, 5u>
            resolution_root_subphases{
                "resolution-root-dependencies",
                "resolution-root-scc-order",
                "resolution-root-scc-components",
                "resolution-root-contracts",
                "resolution-root-plan"};
        for (const auto label : resolution_root_subphases) {
            auto root_progress = resolution_progress(
                1u,
                1u,
                "function-values-resolution-root-progress",
                0u,
                0u,
                0u);
            root_progress.function_value_subphase = label;
            root_progress.function_value_subphase_planned = 2u;
            root_progress.function_value_subphase_processed = 2u;
            root_progress.function_value_subphase_queued = 0u;
            root_progress.function_value_subphase_iterations = 2u;
            progress.update(root_progress);
        }
        progress.update(
            resolution_progress(
                1u,
                1u,
                "function-values-resolution-progress",
                3u,
                0u,
                3u));
        progress.update(
            inactive_function_value_completion(1u, 1u));
        auto terminal_start =
            inactive_function_value_completion(1u, 0u);
        terminal_start.phase =
            "analysis-terminal-materialization-start";
        progress.update(terminal_start);
        auto terminal_materialization = resolution_progress(
            1u,
            0u,
            "function-values-terminal-materialized",
            0u,
            0u,
            0u);
        terminal_materialization.function_value_logical_evaluations = 0u;
        terminal_materialization.function_value_physical_evaluations = 0u;
        progress.update(terminal_materialization);
        progress.update(
            resolution_progress(
                2u,
                1u,
                "function-values-resolution-progress",
                3u,
                2u,
                1u));
        progress.update(
            resolution_progress(
                2u,
                1u,
                "function-values-budget-exhausted",
                3u,
                2u,
                1u));
        progress.complete(2u);
    }
    require(
        reporter.flush(),
        "Der strukturierte Progress-Test konnte seine asynchronen "
        "Beobachtungen nicht vollstaendig flushen.");

    const std::lock_guard lock(events_mutex);
    const auto observed_resolution_metrics =
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateResolution &&
                       event.counters.planned_work ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.ready_work ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.committed_work ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters.configured_workers ==
                           std::optional<std::uint64_t>{24u} &&
                       event.counters.head_of_line_index ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters
                               .head_of_line_elapsed_milliseconds ==
                           std::optional<std::uint64_t>{12'345u} &&
                       event.counters.cache_lookups ==
                           std::optional<std::uint64_t>{10u} &&
                       event.counters.cache_ready_hits ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters
                               .cache_in_flight_coalesces ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.evaluation_requests ==
                           std::optional<std::uint64_t>{13u} &&
                       event.counters.active_evaluation_requests ==
                           std::optional<std::uint64_t>{7u} &&
                       event.counters.cache_key_builds ==
                           std::optional<std::uint64_t>{10u} &&
                       event.counters.cache_waits ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.cache_replays ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.physical_evaluations ==
                           std::optional<std::uint64_t>{5u} &&
                       event.counters.cache_commits ==
                           std::optional<std::uint64_t>{5u} &&
                       event.counters.multi_root_context_requests ==
                           std::optional<std::uint64_t>{5u} &&
                       event.counters.multi_root_unique_contexts ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.multi_root_ready_reuses ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.multi_root_in_flight_reuses ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.multi_root_provenance_links ==
                           std::optional<std::uint64_t>{6u} &&
                       event.counters.multi_root_retained_contexts ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters
                               .multi_root_retained_payload_bytes ==
                           std::optional<std::uint64_t>{2'048u} &&
                       event.counters.incremental_epochs_started ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.analysis_epochs_published ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.analysis_epochs_discarded ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters.resolution_root_artifacts_total ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.resolution_root_artifacts_reused ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.resolution_root_artifacts_recomputed ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.resolution_root_artifacts_retained ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.resolution_epoch_retained_bytes ==
                           std::optional<std::uint64_t>{4'096u} &&
                       event.counters.resolution_retention_limit_reason ==
                           std::optional<std::string>{"none"} &&
                       event.counters.dirty_sccs ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.dirty_functions ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.dirty_inventory_sinks ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.full_cpu_recompute_fallbacks ==
                           std::optional<std::uint64_t>{0u} &&
                       katana::progress_event_telemetry_complete(
                           event);
            });
    require(
        observed_resolution_metrics,
        "Die strukturierte Candidate-Resolution verlor planned/ready/"
        "committed, HOL, Worker-, Cache- oder Inkrementalbilanz.");
    require(
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.counters
                           .resolution_retention_limit_reason ==
                           std::optional<std::string>{"none"} &&
                       katana::format_progress_event_json(event).find(
                           "\"resolution_retention_limit_reason\":\"none\"") !=
                           std::string::npos &&
                       katana::format_progress_event_human(event).find(
                           " resolution_epoch_retained_bytes=4096") !=
                           std::string::npos;
            }),
        "Retention-Grund oder exakte Retained-Bytes gingen im "
        "strukturierten Progress verloren.");

    const auto observed_seed_round_metrics =
        std::find_if(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::ControlFlowRound &&
                       event.counters.round_seed_facts_added ==
                           std::optional<std::uint64_t>{12u} &&
                       event.counters.round_seed_targets_changed ==
                           std::optional<std::uint64_t>{10u} &&
                       event.counters.round_decode_targets ==
                           std::optional<std::uint64_t>{8u} &&
                       event.counters.round_metadata_targets ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.round_full_cpu_fallbacks ==
                           std::optional<std::uint64_t>{0u};
            });
    require(
        observed_seed_round_metrics != events.end() &&
            katana::format_progress_event_json(
                *observed_seed_round_metrics)
                    .find("\"round_seed_facts_added\":12") !=
                std::string::npos &&
            katana::format_progress_event_human(
                *observed_seed_round_metrics)
                    .find(" round_seed_targets_changed=10") !=
                std::string::npos,
        "Die typisierte Seed-Rundenbilanz erreichte JSON-/Human-Progress "
        "nicht vollstaendig.");

    const auto observed_cfa_physical_work = std::find_if(
        events.begin(), events.end(), [](const auto& event) {
            const auto& counters = event.counters;
            return event.operation ==
                       katana::ProgressOperation::ControlFlowRound &&
                   counters.persistent_analysis_bypass_reason ==
                       std::optional<std::string>{
                           "program-delta-unrepresentable"} &&
                   counters.recursive_snapshot_epochs ==
                       std::optional<std::uint64_t>{31u} &&
                   counters.recursive_final_materializations ==
                       std::optional<std::uint64_t>{32u} &&
                   counters.recursive_trusted_snapshot_validations ==
                       std::optional<std::uint64_t>{33u} &&
                   counters.recursive_seed_arena_copy_items ==
                       std::optional<std::uint64_t>{34u} &&
                   counters.recursive_seed_arena_copy_bytes ==
                       std::optional<std::uint64_t>{35u} &&
                   counters.recursive_seed_arena_shift_items ==
                       std::optional<std::uint64_t>{36u} &&
                   counters.recursive_seed_arena_shift_bytes ==
                       std::optional<std::uint64_t>{37u} &&
                   counters.epoch_index_lookups ==
                       std::optional<std::uint64_t>{132u} &&
                   counters.epoch_index_updates ==
                       std::optional<std::uint64_t>{131u} &&
                   counters.terminal_epoch_fold_items ==
                       std::optional<std::uint64_t>{133u} &&
                   counters.recursive_seed_contract_items_visited ==
                       std::optional<std::uint64_t>{38u} &&
                   counters.recursive_decoded_work_items ==
                       std::optional<std::uint64_t>{39u} &&
                   counters.recursive_canonical_context_updates ==
                       std::optional<std::uint64_t>{40u} &&
                   counters.recursive_canonical_instruction_updates ==
                       std::optional<std::uint64_t>{41u} &&
                   counters.recursive_canonical_function_updates ==
                       std::optional<std::uint64_t>{42u} &&
                   counters.recursive_public_baseline_hash_bytes ==
                       std::optional<std::uint64_t>{43u} &&
                   counters.recursive_public_baseline_copy_items ==
                       std::optional<std::uint64_t>{44u} &&
                   counters.recursive_public_sort_items ==
                       std::optional<std::uint64_t>{45u} &&
                   counters.recursive_public_materialized_items ==
                       std::optional<std::uint64_t>{46u} &&
                   counters.recursive_public_materializations ==
                       std::optional<std::uint64_t>{47u} &&
                   counters.runtime_copy_instruction_visits ==
                       std::optional<std::uint64_t>{48u} &&
                   counters.runtime_copy_result_entries_visited ==
                       std::optional<std::uint64_t>{135u} &&
                   counters.runtime_copy_result_entries_rebuilt ==
                       std::optional<std::uint64_t>{134u} &&
                   counters.local_control_flow_instruction_visits ==
                       std::optional<std::uint64_t>{49u} &&
                   counters.local_control_flow_result_entries_visited ==
                       std::optional<std::uint64_t>{137u} &&
                   counters.local_control_flow_result_entries_rebuilt ==
                       std::optional<std::uint64_t>{136u} &&
                   counters.dispatch_index_entries_visited ==
                       std::optional<std::uint64_t>{139u} &&
                   counters.dispatch_index_entries_rebuilt ==
                       std::optional<std::uint64_t>{138u} &&
                   counters.jump_table_instruction_visits ==
                       std::optional<std::uint64_t>{51u} &&
                   counters.jump_table_result_entries_visited ==
                       std::optional<std::uint64_t>{141u} &&
                   counters.jump_table_result_entries_rebuilt ==
                       std::optional<std::uint64_t>{140u} &&
                   counters.function_boundary_entries_visited ==
                       std::optional<std::uint64_t>{53u} &&
                   counters.function_boundary_entries_rebuilt ==
                       std::optional<std::uint64_t>{52u} &&
                   counters.function_edge_family_entries_visited ==
                       std::optional<std::uint64_t>{55u} &&
                   counters.function_edge_family_entries_rebuilt ==
                       std::optional<std::uint64_t>{54u} &&
                   counters.function_edge_state_encode_items ==
                       std::optional<std::uint64_t>{142u} &&
                   counters.function_edge_state_copy_items ==
                       std::optional<std::uint64_t>{143u} &&
                   counters.function_edge_state_exact_compare_items ==
                       std::optional<std::uint64_t>{144u} &&
                   counters.result_index_copy_items ==
                       std::optional<std::uint64_t>{145u} &&
                   counters.result_index_sort_items ==
                       std::optional<std::uint64_t>{146u} &&
                   counters.result_index_materialized_items ==
                       std::optional<std::uint64_t>{147u};
        });
    require(
        observed_cfa_physical_work != events.end() &&
            katana::format_progress_event_json(
                *observed_cfa_physical_work)
                    .find("\"recursive_seed_arena_shift_bytes\":37") !=
                std::string::npos &&
            katana::format_progress_event_json(
                *observed_cfa_physical_work)
                    .find("\"terminal_epoch_fold_items\":133") !=
                std::string::npos &&
            katana::format_progress_event_human(
                *observed_cfa_physical_work)
                    .find(" result_index_materialized_items=147") !=
                std::string::npos,
        "Recursive-/CFA-PhysicalWork erreichte Control/Round oder seine "
        "serialisierten Progressformen nicht vollstaendig.");
    constexpr std::array<katana::ProgressOperation, 2u>
        cfa_physical_work_scopes{
            katana::ProgressOperation::ControlFlowAnalysis,
            katana::ProgressOperation::ControlFlowRound};
    require(
        std::all_of(
            cfa_physical_work_scopes.begin(),
            cfa_physical_work_scopes.end(),
            [&](const auto operation) {
                return std::any_of(
                    events.begin(), events.end(),
                    [operation](const auto& event) {
                        return event.operation == operation &&
                               event.counters
                                       .persistent_analysis_bypass_reason ==
                                   std::optional<std::string>{
                                       "program-delta-unrepresentable"} &&
                               event.counters.epoch_index_lookups ==
                                   std::optional<std::uint64_t>{132u} &&
                               event.counters
                                       .function_edge_state_copy_items ==
                                   std::optional<std::uint64_t>{143u} &&
                               event.counters
                                       .result_index_materialized_items ==
                                   std::optional<std::uint64_t>{147u};
                    });
            }),
        "CFA-PhysicalWork war nicht in Control- und Round-Scope identisch "
        "verdrahtet.");

    const auto observed_fva_physical_work = std::find_if(
        events.begin(), events.end(), [](const auto& event) {
            const auto& counters = event.counters;
            return event.operation ==
                       katana::ProgressOperation::CandidateResolution &&
                   counters.persistent_analysis_bypass_reason ==
                       std::optional<std::string>{"explicit-test"} &&
                   counters.program_delta_entries_visited ==
                       std::optional<std::uint64_t>{61u} &&
                   counters.function_edge_full_scans ==
                       std::optional<std::uint64_t>{62u} &&
                   counters.function_edge_full_sorts ==
                       std::optional<std::uint64_t>{63u} &&
                   counters.candidate_call_edge_full_scans ==
                       std::optional<std::uint64_t>{64u} &&
                   counters.candidate_call_edge_full_sorts ==
                       std::optional<std::uint64_t>{65u} &&
                   counters.candidate_tail_edge_full_scans ==
                       std::optional<std::uint64_t>{66u} &&
                   counters.candidate_tail_edge_full_sorts ==
                       std::optional<std::uint64_t>{67u} &&
                   counters.program_graph_blocks_built ==
                       std::optional<std::uint64_t>{68u} &&
                   counters.program_graph_blocks_reused ==
                       std::optional<std::uint64_t>{69u} &&
                   counters.program_graph_sccs_built ==
                       std::optional<std::uint64_t>{70u} &&
                   counters.program_graph_sccs_reused ==
                       std::optional<std::uint64_t>{71u} &&
                   counters.resolution_dependency_nodes_built ==
                       std::optional<std::uint64_t>{72u} &&
                   counters.resolution_dependency_nodes_reused ==
                       std::optional<std::uint64_t>{73u} &&
                   counters.resolution_dependency_sccs_built ==
                       std::optional<std::uint64_t>{74u} &&
                   counters.resolution_dependency_sccs_reused ==
                       std::optional<std::uint64_t>{75u} &&
                   counters.abi_contract_entries_visited ==
                       std::optional<std::uint64_t>{77u} &&
                   counters.abi_contract_entries_rebuilt ==
                       std::optional<std::uint64_t>{76u} &&
                   counters.summary_candidate_entries_visited ==
                       std::optional<std::uint64_t>{79u} &&
                   counters.summary_candidate_entries_rebuilt ==
                       std::optional<std::uint64_t>{78u} &&
                   counters.inventory_topology_entries_visited ==
                       std::optional<std::uint64_t>{82u} &&
                   counters.resolution_preparation_entries_visited ==
                       std::optional<std::uint64_t>{83u} &&
                   counters.final_materialized_blocks ==
                       std::optional<std::uint64_t>{80u} &&
                   counters.final_materialized_functions ==
                       std::optional<std::uint64_t>{81u};
        });
    require(
        observed_fva_physical_work != events.end() &&
            katana::format_progress_event_json(
                *observed_fva_physical_work)
                    .find("\"persistent_analysis_bypass_reason\":"
                          "\"explicit-test\"") != std::string::npos &&
            katana::format_progress_event_human(
                *observed_fva_physical_work)
                    .find(" final_materialized_functions=81") !=
                std::string::npos,
        "FVA-PhysicalWork erreichte Function/Candidate/Resolution oder "
        "seine serialisierten Progressformen nicht vollstaendig.");
    constexpr std::array<katana::ProgressOperation, 3u>
        fva_physical_work_scopes{
            katana::ProgressOperation::FunctionValueAnalysis,
            katana::ProgressOperation::CandidateContractIteration,
            katana::ProgressOperation::CandidateResolution};
    require(
        std::all_of(
            fva_physical_work_scopes.begin(),
            fva_physical_work_scopes.end(),
            [&](const auto operation) {
                return std::any_of(
                    events.begin(), events.end(),
                    [operation](const auto& event) {
                        return event.operation == operation &&
                               event.counters
                                       .persistent_analysis_bypass_reason ==
                                   std::optional<std::string>{
                                       "explicit-test"} &&
                               event.counters
                                       .program_delta_entries_visited ==
                                   std::optional<std::uint64_t>{61u} &&
                               event.counters
                                       .final_materialized_functions ==
                                   std::optional<std::uint64_t>{81u};
                    });
            }),
        "FVA-PhysicalWork war nicht in Function-, Candidate- und "
        "Resolution-Scope identisch verdrahtet.");

    const auto observed_saturated_ready_ahead =
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateResolution &&
                       event.counters.ready_work ==
                           std::optional<std::uint64_t>{2u} &&
                       event.counters.committed_work ==
                           std::optional<std::uint64_t>{1u} &&
                       event.counters.ready_ahead ==
                           std::optional<std::uint64_t>{2u};
            });
    require(
        observed_saturated_ready_ahead,
        "Candidate-Resolution zog den kumulativen Commitstand erneut von "
        "der aktuellen Ready-Queue-Belegung ab.");

    const auto observed_drained_ready_queue =
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateResolution &&
                       event.counters.ready_work ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters.committed_work ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.ready_ahead ==
                           std::optional<std::uint64_t>{0u};
            });
    require(
        observed_drained_ready_queue,
        "Candidate-Resolution verlor den leeren Ready-Queue-Endzustand "
        "nach dem letzten kanonischen Commit.");

    const auto observed_real_subphase =
        std::find_if(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               FunctionValueAnalysis &&
                       event.label == "abi-stack-reads" &&
                       event.state ==
                           katana::ProgressState::Completed &&
                       event.completed == 5u &&
                       event.counters.planned_work ==
                           std::optional<std::uint64_t>{5u} &&
                       event.counters.committed_work ==
                           std::optional<std::uint64_t>{5u} &&
                       event.counters.queued_work ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters.iteration ==
                           std::optional<std::uint64_t>{5u} &&
                       !event.counters.started.has_value();
            });
    require(
        observed_real_subphase != events.end() &&
            katana::format_progress_event_json(
                *observed_real_subphase)
                    .find("\"started\":") == std::string::npos &&
            katana::format_progress_event_human(
                *observed_real_subphase)
                    .find(" started=") == std::string::npos,
        "Eine echte FVA-Unterphase verlor planned/processed/queued/"
        "iterations, ihren terminalen Structured-Scope oder erfand einen "
        "Started-Zaehler.");
    require(
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               FunctionValueAnalysis &&
                       event.label ==
                           "inventory-region-sink-sources" &&
                       event.state ==
                           katana::ProgressState::Completed &&
                       event.completed == 3u &&
                       event.counters.planned_work ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.committed_work ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters.queued_work ==
                           std::optional<std::uint64_t>{0u} &&
                       event.counters.iteration ==
                           std::optional<std::uint64_t>{3u} &&
                       event.counters
                               .persistent_analysis_bypass_reason ==
                           std::optional<std::string>{"explicit-test"} &&
                       event.counters.program_delta_entries_visited ==
                           std::optional<std::uint64_t>{61u} &&
                       event.counters.final_materialized_functions ==
                           std::optional<std::uint64_t>{81u};
            }),
        "Inventory-Region-Sink-Quellen erreichten den strukturierten "
        "Produktfortschritt nicht terminal.");
    constexpr std::array<std::string_view, 5u>
        expected_resolution_root_subphases{
            "resolution-root-dependencies",
            "resolution-root-scc-order",
            "resolution-root-scc-components",
            "resolution-root-contracts",
            "resolution-root-plan"};
    require(
        std::all_of(
            expected_resolution_root_subphases.begin(),
            expected_resolution_root_subphases.end(),
            [&](const auto label) {
                return std::any_of(
                    events.begin(),
                    events.end(),
                    [&](const auto& event) {
                        return event.operation ==
                                   katana::ProgressOperation::
                                       FunctionValueAnalysis &&
                               event.label == label &&
                               event.state ==
                                   katana::ProgressState::Completed &&
                               event.completed == 2u &&
                               event.counters.planned_work ==
                                   std::optional<std::uint64_t>{2u} &&
                               event.counters.committed_work ==
                                   std::optional<std::uint64_t>{2u};
                    });
            }),
        "Mindestens eine Resolution-Root-Unterphase erreichte den "
        "strukturierten Produktfortschritt nicht terminal.");

    const auto failed_resolution =
        std::find_if(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateResolution &&
                       event.state ==
                           katana::ProgressState::Failed;
            });
    require(
        failed_resolution != events.end(),
        "Ein budgetierter Resolution-Abbruch wurde nicht als Failed "
        "beendet.");
    const auto false_success =
        std::find_if(
            events.begin(),
            events.end(),
            [&](const auto& event) {
                return event.scope_id ==
                           failed_resolution->scope_id &&
                       event.state ==
                           katana::ProgressState::Completed;
            });
    require(
        false_success == events.end(),
        "Ein budgetierter Resolution-Abbruch wurde zusaetzlich als "
        "100-Prozent-Erfolg gemeldet.");

    const auto completed_candidate_iterations =
        std::count_if(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateContractIteration &&
                       event.state ==
                           katana::ProgressState::Completed;
            });
    const auto failed_candidate_iterations =
        std::count_if(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               CandidateContractIteration &&
                       event.state ==
                           katana::ProgressState::Failed;
            });
    require(
        completed_candidate_iterations == 1u &&
            failed_candidate_iterations == 1u,
        "TerminalFull setzte den abgeschlossenen Candidate-Fortschritt "
        "auf null zurueck oder verlor dessen terminalen Abschluss.");

    require(
        std::any_of(
            events.begin(),
            events.end(),
            [](const auto& event) {
                return event.operation ==
                           katana::ProgressOperation::
                               ControlFlowRound;
            }) &&
            std::any_of(
                events.begin(),
                events.end(),
                [](const auto& event) {
                    return event.operation ==
                               katana::ProgressOperation::
                                   CandidateContractIteration;
                }) &&
            std::any_of(
                events.begin(),
                events.end(),
                [](const auto& event) {
                    return event.operation ==
                               katana::ProgressOperation::
                                   ControlFlowAnalysis &&
                           event.state ==
                               katana::ProgressState::Failed;
                }),
        "CFG-Runden-/Candidate-Scopes oder der fail-closed "
        "Kontrollflussabschluss fehlen.");

    std::cout << "KR-4974 strukturierter CFG-/HOL-Progress "
                 "erfolgreich.\n";
    return EXIT_SUCCESS;
}
