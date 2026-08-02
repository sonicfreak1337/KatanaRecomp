#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/progress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace katana::codegen::detail {

class StructuredControlFlowProgress final {
  public:
    StructuredControlFlowProgress(
        const katana::ProgressReporter& reporter,
        std::string label)
        : control_(reporter.begin(
              katana::ProgressOperation::ControlFlowAnalysis,
              katana::ProgressUnit::Steps,
              std::nullopt,
              std::move(label))) {}

    void update(
        const katana::analysis::ControlFlowAnalysisProgress& progress) {
        ensure_round(progress);
        ensure_candidate_iteration(progress);

        katana::ProgressCounterSnapshot control_counters;
        control_counters.iteration = progress.iteration;
        if (progress.candidate_contract_iteration != 0u)
            control_counters.pass =
                progress.candidate_contract_iteration;
        control_counters.discovered = progress.seeds;
        control_counters.started = progress.instructions;
        control_counters.queued_work = progress.contexts;
        control_counters.planned_work = progress.seeds;
        control_counters.added_work =
            progress.round_added_seeds;
        append_seed_round_counters(
            control_counters, progress);
        append_control_physical_work_counters(
            control_counters, progress);
        control_counters.growing_workset =
            progress.growing_workset;
        if (progress.function_value_active) {
            control_counters.configured_workers =
                progress.function_value_configured_workers;
            append_executor_counters(control_counters, progress);
        }
        control_.update(
            progress.iteration,
            std::move(control_counters));
        update_round(progress);

        const auto failed =
            progress.phase.find("budget-exhausted") !=
                std::string_view::npos ||
            progress.phase.find("cycle-exhausted") !=
                std::string_view::npos ||
            progress.phase.find("boundary-contract-stale") !=
                std::string_view::npos;
        const auto function_values_complete =
            progress.phase.starts_with(
                "function-values-complete") ||
            progress.phase ==
                "function-values-terminal-materialized";
        if (!progress.function_value_active) {
            if (failed) {
                close_function_values(false);
                close_candidate_iteration(false);
                round_failed_ = true;
            } else if (function_values_complete) {
                // A producer may report its terminal phase without carrying
                // the just-finished FunctionValue snapshot. Preserve the
                // previous monotonic counters and close the nested scope
                // instead of updating the candidate back to zero.
                close_function_values(true);
            }
            return;
        }
        update_candidate_iteration(progress);

        if (!function_values_) {
            const auto parent =
                candidate_iteration_
                    ? candidate_iteration_->child_reporter()
                    : round_
                          ? round_->child_reporter()
                          : control_.child_reporter();
            function_values_.emplace(
                parent.begin(
                    katana::ProgressOperation::FunctionValueAnalysis,
                    katana::ProgressUnit::Steps,
                    std::nullopt,
                    "function-value-analysis"));
        }
        update_function_subphase(progress);
        katana::ProgressCounterSnapshot function_counters;
        function_counters.iteration =
            progress.function_value_iterations;
        function_counters.pass = progress.iteration;
        function_counters.active_workers =
            progress.function_value_active_workers;
        function_counters.queued_work =
            progress.function_value_pending;
        function_counters.planned_work =
            progress.function_value_functions;
        function_counters.committed_work =
            progress.function_value_summarized_functions;
        function_counters.configured_workers =
            progress.function_value_configured_workers;
        append_executor_counters(function_counters, progress);
        function_counters.discovered =
            progress.function_value_functions;
        function_counters.started =
            progress.function_value_physical_evaluations;
        append_cache_counters(function_counters, progress);
        append_incremental_epoch_counters(
            function_counters, progress);
        append_function_physical_work_counters(
            function_counters, progress);
        function_values_->update(
            progress.function_value_logical_evaluations,
            std::move(function_counters));

        if (progress.function_value_resolution_functions_total !=
                0u &&
            !resolution_) {
            resolution_.emplace(
                function_values_->child_reporter().begin(
                    katana::ProgressOperation::CandidateResolution,
                    katana::ProgressUnit::Functions,
                    progress
                        .function_value_resolution_functions_total,
                    "function-value-resolution"));
        }
        if (resolution_) {
            katana::ProgressCounterSnapshot resolution_counters;
            resolution_counters.pass = progress.iteration;
            resolution_counters.active_workers =
                progress.function_value_active_workers;
            resolution_counters.queued_work =
                progress
                    .function_value_resolution_functions_total -
                std::min(
                    progress
                        .function_value_resolution_functions_total,
                    progress
                        .function_value_resolution_functions_started);
            resolution_counters.discovered =
                progress
                    .function_value_resolution_functions_total;
            resolution_counters.started =
                progress
                    .function_value_resolution_functions_started;
            resolution_counters.planned_work =
                progress
                    .function_value_resolution_functions_total;
            resolution_counters.ready_work =
                progress
                    .function_value_resolution_functions_ready;
            resolution_counters.committed_work =
                progress.function_value_resolution_functions_committed;
            resolution_counters.configured_workers =
                progress.function_value_configured_workers;
            append_executor_counters(resolution_counters, progress);
            // The producer maintains `resolution_functions_ready` as the
            // current ready-queue occupancy: it increments when a result is
            // published and decrements when that result leaves the queue for
            // canonical commit. It is therefore already the exact
            // head-of-line lead and must not be reduced by the cumulative
            // committed count a second time.
            resolution_counters.ready_ahead =
                progress.function_value_resolution_functions_ready;
            if (progress.function_value_resolution_functions_committed <
                progress
                    .function_value_resolution_functions_total) {
                resolution_counters.head_of_line_index =
                    progress
                        .function_value_resolution_head_of_line_index;
                resolution_counters
                    .head_of_line_elapsed_milliseconds =
                    progress
                        .function_value_resolution_head_of_line_elapsed_milliseconds;
            }
            append_cache_counters(
                resolution_counters, progress);
            append_incremental_epoch_counters(
                resolution_counters, progress);
            append_function_physical_work_counters(
                resolution_counters, progress);
            resolution_->update(
                std::min(
                    progress
                        .function_value_resolution_functions_total,
                    progress
                        .function_value_resolution_functions_committed),
                std::move(resolution_counters));
        }

        if (failed) {
            close_function_values(false);
            close_candidate_iteration(false);
            round_failed_ = true;
        } else if (function_values_complete) {
            close_function_values(true);
        }
    }

    void complete(const std::size_t iterations) {
        close_function_values(!round_failed_);
        close_candidate_iteration(!round_failed_);
        close_round(!round_failed_);
        if (round_failed_)
            control_.fail();
        else
            control_.complete(iterations);
    }

  private:
    static void append_seed_round_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        counters.round_seed_facts_added =
            progress.round_seed_facts_added;
        counters.round_seed_targets_changed =
            progress.round_seed_targets_changed;
        counters.round_decode_targets =
            progress.round_decode_targets;
        counters.round_metadata_targets =
            progress.round_metadata_targets;
        counters.round_full_cpu_fallbacks =
            progress.round_full_cpu_fallbacks;
    }

    static void append_executor_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress& progress) {
        counters.executor_running_workers =
            progress.function_value_executor_running_workers;
        counters.executor_waiting_workers =
            progress.function_value_executor_waiting_workers;
        counters.executor_idle_workers =
            progress.function_value_executor_idle_workers;
        counters.executor_queued_work =
            progress.function_value_executor_queued_work;
        counters.executor_memory_blocked_work =
            progress.function_value_executor_memory_blocked_work;
        counters.executor_continuations =
            progress.function_value_executor_continuations;
        counters.analysis_memory_capacity_bytes =
            progress.function_value_analysis_memory_capacity_bytes;
        counters.analysis_memory_used_bytes =
            progress.function_value_analysis_memory_used_bytes;
        counters.analysis_memory_peak_bytes =
            progress.function_value_analysis_memory_peak_bytes;
    }

    static void append_incremental_epoch_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        counters.analysis_epochs_published =
            progress.function_value_analysis_epochs_published;
        counters.analysis_epochs_discarded =
            progress.function_value_analysis_epochs_discarded;
        counters.incremental_epochs_started =
            progress.function_value_incremental_epochs_started;
        counters.resolution_root_artifacts_total =
            progress.function_value_resolution_root_artifacts_total;
        counters.resolution_root_artifacts_reused =
            progress.function_value_resolution_root_artifacts_reused;
        counters.resolution_root_artifacts_recomputed =
            progress.function_value_resolution_root_artifacts_recomputed;
        counters.resolution_root_artifacts_retained =
            progress.function_value_resolution_root_artifacts_retained;
        counters.resolution_epoch_retained_bytes =
            progress.function_value_resolution_epoch_retained_bytes;
        counters.resolution_retention_limit_reason = std::string{
            katana::analysis::resolution_retention_limit_reason_name(
                progress
                    .function_value_resolution_retention_limit_reason)};
        counters.dirty_sccs =
            progress.function_value_dirty_sccs;
        counters.dirty_functions =
            progress.function_value_dirty_functions;
        counters.dirty_inventory_sinks =
            progress.function_value_dirty_inventory_sinks;
        counters.full_cpu_recompute_fallbacks =
            progress.function_value_full_cpu_recompute_fallbacks;
    }

    static void append_control_physical_work_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        counters.persistent_analysis_bypass_reason = std::string{
            katana::analysis::persistent_analysis_bypass_reason_name(
                progress.persistent_analysis_bypass_reason)};
        counters.recursive_snapshot_epochs =
            progress.recursive_snapshot_epochs;
        counters.recursive_final_materializations =
            progress.recursive_final_materializations;
        const auto& recursive = progress.recursive_physical_work;
        counters.recursive_trusted_snapshot_validations =
            recursive.trusted_snapshot_validations;
        counters.recursive_seed_arena_copy_items =
            recursive.seed_arena_copy_items;
        counters.recursive_seed_arena_copy_bytes =
            recursive.seed_arena_copy_bytes;
        counters.recursive_seed_arena_shift_items =
            recursive.seed_arena_shift_items;
        counters.recursive_seed_arena_shift_bytes =
            recursive.seed_arena_shift_bytes;
        counters.epoch_index_lookups =
            recursive.epoch_index_lookups;
        counters.epoch_index_updates =
            recursive.epoch_index_updates;
        counters.terminal_epoch_fold_items =
            recursive.terminal_epoch_fold_items;
        counters.recursive_seed_contract_items_visited =
            recursive.seed_contract_items_visited;
        counters.recursive_decoded_work_items =
            recursive.decoded_work_items;
        counters.recursive_canonical_context_updates =
            recursive.canonical_context_updates;
        counters.recursive_canonical_instruction_updates =
            recursive.canonical_instruction_updates;
        counters.recursive_canonical_function_updates =
            recursive.canonical_function_updates;
        counters.recursive_public_baseline_hash_bytes =
            recursive.public_baseline_hash_bytes;
        counters.recursive_public_baseline_copy_items =
            recursive.public_baseline_copy_items;
        counters.recursive_public_sort_items =
            recursive.public_sort_items;
        counters.recursive_public_materialized_items =
            recursive.public_materialized_items;
        counters.recursive_public_materializations =
            recursive.public_materializations;
        counters.runtime_copy_instruction_visits =
            progress.runtime_copy_instruction_visits;
        counters.runtime_copy_result_entries_visited =
            progress.runtime_copy_result_entries_visited;
        counters.runtime_copy_result_entries_rebuilt =
            progress.runtime_copy_result_entries_rebuilt;
        counters.local_control_flow_instruction_visits =
            progress.local_control_flow_instruction_visits;
        counters.local_control_flow_result_entries_visited =
            progress.local_control_flow_result_entries_visited;
        counters.local_control_flow_result_entries_rebuilt =
            progress.local_control_flow_result_entries_rebuilt;
        counters.dispatch_index_entries_visited =
            progress.dispatch_index_entries_visited;
        counters.dispatch_index_entries_rebuilt =
            progress.dispatch_index_entries_rebuilt;
        counters.jump_table_instruction_visits =
            progress.jump_table_instruction_visits;
        counters.jump_table_result_entries_visited =
            progress.jump_table_result_entries_visited;
        counters.jump_table_result_entries_rebuilt =
            progress.jump_table_result_entries_rebuilt;
        counters.function_boundary_entries_visited =
            progress.function_boundary_entries_visited;
        counters.function_boundary_entries_rebuilt =
            progress.function_boundary_entries_rebuilt;
        counters.function_edge_family_entries_visited =
            progress.function_edge_family_entries_visited;
        counters.function_edge_family_entries_rebuilt =
            progress.function_edge_family_entries_rebuilt;
        counters.function_edge_state_encode_items =
            progress.function_edge_state_encode_items;
        counters.function_edge_state_copy_items =
            progress.function_edge_state_copy_items;
        counters.function_edge_state_exact_compare_items =
            progress.function_edge_state_exact_compare_items;
        counters.result_index_copy_items =
            progress.result_index_copy_items;
        counters.result_index_sort_items =
            progress.result_index_sort_items;
        counters.result_index_materialized_items =
            progress.result_index_materialized_items;
    }

    static void append_function_physical_work_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        counters.persistent_analysis_bypass_reason = std::string{
            katana::analysis::persistent_analysis_bypass_reason_name(
                progress
                    .function_value_persistent_analysis_bypass_reason)};
        counters.program_delta_entries_visited =
            progress.function_value_program_delta_entries_visited;
        counters.function_edge_full_scans =
            progress.function_value_function_edge_full_scans;
        counters.function_edge_full_sorts =
            progress.function_value_function_edge_full_sorts;
        counters.candidate_call_edge_full_scans =
            progress.function_value_candidate_call_edge_full_scans;
        counters.candidate_call_edge_full_sorts =
            progress.function_value_candidate_call_edge_full_sorts;
        counters.candidate_tail_edge_full_scans =
            progress.function_value_candidate_tail_edge_full_scans;
        counters.candidate_tail_edge_full_sorts =
            progress.function_value_candidate_tail_edge_full_sorts;
        counters.program_graph_blocks_built =
            progress.function_value_graph_blocks_built;
        counters.program_graph_blocks_reused =
            progress.function_value_graph_blocks_reused;
        counters.program_graph_sccs_built =
            progress.function_value_graph_sccs_built;
        counters.program_graph_sccs_reused =
            progress.function_value_graph_sccs_reused;
        counters.resolution_dependency_nodes_built =
            progress.function_value_resolution_dependency_nodes_built;
        counters.resolution_dependency_nodes_reused =
            progress.function_value_resolution_dependency_nodes_reused;
        counters.resolution_dependency_sccs_built =
            progress.function_value_resolution_dependency_sccs_built;
        counters.resolution_dependency_sccs_reused =
            progress.function_value_resolution_dependency_sccs_reused;
        counters.abi_contract_entries_visited =
            progress.function_value_abi_contract_entries_visited;
        counters.abi_contract_entries_rebuilt =
            progress.function_value_abi_contract_entries_rebuilt;
        counters.summary_candidate_entries_visited =
            progress.function_value_summary_candidate_entries_visited;
        counters.summary_candidate_entries_rebuilt =
            progress.function_value_summary_candidate_entries_rebuilt;
        counters.inventory_topology_entries_visited =
            progress.function_value_inventory_topology_entries_visited;
        counters.resolution_preparation_entries_visited =
            progress.function_value_resolution_preparation_entries_visited;
        counters.final_materialized_blocks =
            progress.function_value_final_materialized_blocks;
        counters.final_materialized_functions =
            progress.function_value_final_materialized_functions;
    }

    static bool is_explicit_function_subphase(
        const std::string_view subphase) noexcept {
        constexpr std::array<std::string_view, 13u> names{
            "inventory-region-closure",
            "inventory-region-sink-sources",
            "abi-return-signatures",
            "abi-stack-reads",
            "abi-register-reads",
            "persistent-store-signatures",
            "inventory-reachability",
            "cache-key-plan",
            "resolution-root-dependencies",
            "resolution-root-scc-order",
            "resolution-root-scc-components",
            "resolution-root-contracts",
            "resolution-root-plan"};
        return std::find(names.begin(), names.end(), subphase) !=
               names.end();
    }

    void update_function_subphase(
        const katana::analysis::ControlFlowAnalysisProgress& progress) {
        const auto& subphase = progress.function_value_subphase;
        if (!function_values_ ||
            !is_explicit_function_subphase(subphase)) {
            close_function_subphase(true);
            return;
        }
        if (!function_subphase_ || current_function_subphase_ != subphase) {
            close_function_subphase(true);
            current_function_subphase_ = std::string(subphase);
            function_subphase_.emplace(
                function_values_->child_reporter().begin(
                    katana::ProgressOperation::FunctionValueAnalysis,
                    katana::ProgressUnit::Steps,
                    std::nullopt,
                    current_function_subphase_));
        }
        katana::ProgressCounterSnapshot counters;
        counters.iteration =
            progress.function_value_subphase_iterations;
        counters.pass = progress.iteration;
        counters.planned_work =
            progress.function_value_subphase_planned;
        counters.queued_work =
            progress.function_value_subphase_queued;
        counters.committed_work =
            progress.function_value_subphase_processed;
        counters.active_workers =
            progress.function_value_active_workers;
        counters.configured_workers =
            progress.function_value_configured_workers;
        append_executor_counters(counters, progress);
        append_cache_counters(counters, progress);
        append_function_physical_work_counters(counters, progress);
        function_subphase_->update(
            progress.function_value_subphase_processed,
            std::move(counters));
        if (progress.function_value_subphase_processed >=
                progress.function_value_subphase_planned &&
            progress.function_value_subphase_queued == 0u)
            close_function_subphase(true);
    }

    void close_function_subphase(const bool success) {
        if (!function_subphase_) return;
        if (success)
            function_subphase_->complete();
        else
            function_subphase_->fail();
        function_subphase_.reset();
        current_function_subphase_.clear();
    }

    static void append_cache_counters(
        katana::ProgressCounterSnapshot& counters,
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        counters.evaluation_requests =
            progress.function_value_logical_evaluations;
        counters.active_evaluation_requests =
            progress.function_value_active_evaluation_requests;
        counters.evaluation_request_nanoseconds =
            progress.function_value_evaluation_request_nanoseconds;
        counters.maximum_evaluation_request_nanoseconds =
            progress
                .function_value_maximum_evaluation_request_nanoseconds;
        counters.cache_key_builds =
            progress.function_value_cache_key_builds;
        counters.active_cache_key_builds =
            progress.function_value_active_cache_key_builds;
        counters.cache_key_build_nanoseconds =
            progress.function_value_cache_key_build_nanoseconds;
        counters.maximum_cache_key_build_nanoseconds =
            progress.function_value_maximum_cache_key_build_nanoseconds;
        counters.cache_waits =
            progress.function_value_cache_waits;
        counters.active_cache_waits =
            progress.function_value_active_cache_waits;
        counters.cache_wait_nanoseconds =
            progress.function_value_cache_wait_nanoseconds;
        counters.maximum_cache_wait_nanoseconds =
            progress.function_value_maximum_cache_wait_nanoseconds;
        counters.cache_replays =
            progress.function_value_cache_replays;
        counters.active_cache_replays =
            progress.function_value_active_cache_replays;
        counters.cache_replay_nanoseconds =
            progress.function_value_cache_replay_nanoseconds;
        counters.maximum_cache_replay_nanoseconds =
            progress.function_value_maximum_cache_replay_nanoseconds;
        counters.physical_evaluations =
            progress.function_value_physical_evaluations;
        counters.active_physical_evaluations =
            progress.function_value_active_physical_evaluations;
        counters.physical_evaluation_nanoseconds =
            progress.function_value_physical_evaluation_nanoseconds;
        counters.maximum_physical_evaluation_nanoseconds =
            progress
                .function_value_maximum_physical_evaluation_nanoseconds;
        counters.cache_commits =
            progress.function_value_cache_commits;
        counters.active_cache_commits =
            progress.function_value_active_cache_commits;
        counters.cache_commit_nanoseconds =
            progress.function_value_cache_commit_nanoseconds;
        counters.maximum_cache_commit_nanoseconds =
            progress.function_value_maximum_cache_commit_nanoseconds;
        counters.cache_lookups =
            progress.function_value_session_cache_lookups;
        counters.cache_ready_hits =
            progress.function_value_session_cache_ready_hits;
        counters.cache_in_flight_coalesces =
            progress
                .function_value_session_cache_in_flight_coalesces;
        counters.cache_hits =
            progress.function_value_session_cache_hits;
        counters.cache_misses =
            progress.function_value_session_cache_misses;
        counters.cache_replay_fallback_recomputes =
            progress
                .function_value_session_cache_replay_fallback_recomputes;
        counters.cache_diagnostic_bypass_evaluations =
            progress
                .function_value_session_cache_diagnostic_bypass_evaluations;
        counters.multi_root_context_requests =
            progress.function_value_multi_root_context_requests;
        counters.multi_root_unique_contexts =
            progress.function_value_multi_root_unique_contexts;
        counters.multi_root_ready_reuses =
            progress.function_value_multi_root_ready_reuses;
        counters.multi_root_in_flight_reuses =
            progress.function_value_multi_root_in_flight_reuses;
        counters.multi_root_provenance_links =
            progress.function_value_multi_root_provenance_links;
        counters.multi_root_retained_contexts =
            progress.function_value_multi_root_retained_contexts;
        counters.multi_root_retained_payload_bytes =
            progress
                .function_value_multi_root_retained_payload_bytes;
        counters.multi_root_evictions =
            progress.function_value_multi_root_evictions;
        counters.cache_evictions =
            progress.function_value_session_cache_evictions;
        counters.cache_entries =
            progress.function_value_session_cache_entries;
        counters.cache_retained_payload_bytes =
            progress
                .function_value_session_cache_retained_payload_bytes;
        counters.cache_miss_cold =
            progress.function_value_session_cache_miss_cold;
        counters.cache_miss_evicted =
            progress.function_value_session_cache_miss_evicted;
        counters.cache_miss_oversize_or_no_exact_replay =
            progress
                .function_value_session_cache_miss_oversize_or_no_exact_replay;
        counters.cache_miss_function_shape_changed =
            progress
                .function_value_session_cache_miss_function_shape_changed;
        counters.cache_miss_projected_ingress_changed =
            progress
                .function_value_session_cache_miss_projected_ingress_changed;
        counters.cache_miss_summary_dependency_changed =
            progress
                .function_value_session_cache_miss_summary_dependency_changed;
        counters.cache_miss_abi_contract_changed =
            progress
                .function_value_session_cache_miss_abi_contract_changed;
        counters.cache_miss_resolution_lens_changed =
            progress
                .function_value_session_cache_miss_resolution_lens_changed;
        counters.cache_miss_inventory_sink_changed =
            progress
                .function_value_session_cache_miss_inventory_sink_changed;
        counters.cache_miss_isolation_partition_changed =
            progress
                .function_value_session_cache_miss_isolation_partition_changed;
        counters.cache_miss_contextual_summary_changed =
            progress
                .function_value_session_cache_miss_contextual_summary_changed;
        counters.cache_miss_tail_ingress_changed =
            progress
                .function_value_session_cache_miss_tail_ingress_changed;
        const auto lens_index = [](
            const katana::analysis::EvaluationLens lens) noexcept {
            return static_cast<std::size_t>(lens);
        };
        const auto& lenses =
            progress.function_value_evaluation_lenses;
        counters.evaluation_lens_full_state_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::FullState)];
        counters.evaluation_lens_summary_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::Summary)];
        counters.evaluation_lens_candidate_contract_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::CandidateContract)];
        counters.evaluation_lens_guarded_inventory_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::GuardedInventory)];
        counters.evaluation_lens_contextual_return_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::ContextualReturn)];
        counters.evaluation_lens_isolated_observation_requests =
            lenses.requests[lens_index(
                katana::analysis::EvaluationLens::IsolatedObservation)];
        counters.evaluation_lens_full_state_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::FullState)];
        counters.evaluation_lens_summary_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::Summary)];
        counters.evaluation_lens_candidate_contract_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::CandidateContract)];
        counters.evaluation_lens_guarded_inventory_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::GuardedInventory)];
        counters.evaluation_lens_contextual_return_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::ContextualReturn)];
        counters.evaluation_lens_isolated_observation_cache_hits =
            lenses.cache_hits[lens_index(
                katana::analysis::EvaluationLens::IsolatedObservation)];
        counters
            .evaluation_lens_full_state_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::FullState)];
        counters
            .evaluation_lens_summary_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::Summary)];
        counters
            .evaluation_lens_candidate_contract_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::CandidateContract)];
        counters
            .evaluation_lens_guarded_inventory_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::GuardedInventory)];
        counters
            .evaluation_lens_contextual_return_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::ContextualReturn)];
        counters
            .evaluation_lens_isolated_observation_avoided_evaluation_nanoseconds =
            lenses.avoided_evaluation_nanoseconds[lens_index(
                katana::analysis::EvaluationLens::IsolatedObservation)];
        counters.full_state_fallbacks =
            lenses.full_state_fallbacks;
        counters.projected_evaluations =
            lenses.projected_evaluations;
        counters.reconstructed_results =
            lenses.reconstructed_results;
        counters.key_interned_sets = lenses.key_interned_sets;
        counters.key_interned_references =
            lenses.key_interned_references;
        counters.program_graph_builds =
            progress.function_value_program_graph_builds;
        counters.program_graph_reuses =
            progress.function_value_program_graph_reuses;
        counters.program_graph_functions_built =
            progress.function_value_program_graph_functions_built;
        counters.program_graph_functions_reused =
            progress.function_value_program_graph_functions_reused;
        counters.caller_scc_invalidations =
            progress.function_value_caller_scc_invalidations;
        counters.abi_contract_epoch_reuses =
            progress.function_value_abi_contract_epoch_reuses;
        counters.summary_state_reuses =
            progress.function_value_summary_state_reuses;
    }

    void ensure_round(
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        if (progress.iteration == 0u ||
            current_round_ == progress.iteration)
            return;
        close_function_values(!round_failed_);
        close_candidate_iteration(!round_failed_);
        close_round(!round_failed_);
        current_round_ = progress.iteration;
        round_failed_ = false;
        round_.emplace(
            control_.child_reporter().begin(
                katana::ProgressOperation::ControlFlowRound,
                katana::ProgressUnit::Steps,
                std::nullopt,
                "cfg-round-" +
                    std::to_string(current_round_)));
    }

    void update_round(
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        if (!round_) return;
        katana::ProgressCounterSnapshot counters;
        counters.iteration = progress.iteration;
        counters.planned_work = progress.seeds;
        counters.discovered = progress.seeds;
        counters.added_work = progress.round_added_seeds;
        append_seed_round_counters(counters, progress);
        append_control_physical_work_counters(counters, progress);
        counters.growing_workset =
            progress.growing_workset;
        counters.committed_work = progress.instructions;
        if (progress.function_value_active) {
            counters.configured_workers =
                progress.function_value_configured_workers;
            append_executor_counters(counters, progress);
        }
        round_->update(progress.instructions,
                       std::move(counters));
    }

    void ensure_candidate_iteration(
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        if (progress.candidate_contract_iteration == 0u) {
            // A zero iteration is an explicit boundary between the
            // candidate-contract fixpoint and later summary/TerminalFull
            // work.  Close the candidate before the next FVA epoch resets
            // its local counters to zero.
            if (candidate_iteration_) {
                close_function_values(
                    !candidate_failed_ && !round_failed_);
                close_candidate_iteration(
                    !candidate_failed_ && !round_failed_);
            }
            return;
        }
        if (round_failed_ ||
            current_candidate_iteration_ ==
                progress.candidate_contract_iteration)
            return;
        close_function_values(!candidate_failed_);
        close_candidate_iteration(!candidate_failed_);
        current_candidate_iteration_ =
            progress.candidate_contract_iteration;
        candidate_failed_ = false;
        const auto parent =
            round_ ? round_->child_reporter()
                   : control_.child_reporter();
        candidate_iteration_.emplace(
            parent.begin(
                katana::ProgressOperation::
                    CandidateContractIteration,
                katana::ProgressUnit::Steps,
                std::nullopt,
                "candidate-contract-" +
                    std::to_string(
                        current_candidate_iteration_)));
    }

    void update_candidate_iteration(
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        if (!candidate_iteration_) return;
        katana::ProgressCounterSnapshot counters;
        counters.iteration =
            progress.candidate_contract_iteration;
        counters.pass = progress.iteration;
        counters.planned_work =
            progress.function_value_resolution_functions_total;
        counters.started =
            progress.function_value_resolution_functions_started;
        counters.active_workers =
            progress.function_value_active_workers;
        counters.ready_work =
            progress.function_value_resolution_functions_ready;
        counters.committed_work =
            progress.function_value_resolution_functions_committed;
        counters.configured_workers =
            progress.function_value_configured_workers;
        append_executor_counters(counters, progress);
        counters.added_work = progress.round_added_seeds;
        append_seed_round_counters(counters, progress);
        counters.growing_workset =
            progress.growing_workset;
        append_cache_counters(counters, progress);
        append_incremental_epoch_counters(counters, progress);
        append_function_physical_work_counters(counters, progress);
        candidate_iteration_->update(
            progress.function_value_logical_evaluations,
            std::move(counters));
    }

    void close_function_values(const bool success) {
        close_function_subphase(success);
        if (resolution_) {
            if (success)
                resolution_->complete();
            else
                resolution_->fail();
            resolution_.reset();
        }
        if (function_values_) {
            if (success)
                function_values_->complete();
            else
                function_values_->fail();
            function_values_.reset();
        }
    }

    void close_candidate_iteration(const bool success) {
        if (!candidate_iteration_) return;
        if (success)
            candidate_iteration_->complete();
        else
            candidate_iteration_->fail();
        candidate_iteration_.reset();
        current_candidate_iteration_ = 0u;
        candidate_failed_ = !success;
    }

    void close_round(const bool success) {
        if (!round_) return;
        if (success)
            round_->complete();
        else
            round_->fail();
        round_.reset();
        current_round_ = 0u;
    }

    katana::ProgressScope control_;
    std::optional<katana::ProgressScope> round_;
    std::optional<katana::ProgressScope> candidate_iteration_;
    std::optional<katana::ProgressScope> function_values_;
    std::optional<katana::ProgressScope> function_subphase_;
    std::optional<katana::ProgressScope> resolution_;
    std::string current_function_subphase_;
    std::size_t current_round_ = 0u;
    std::size_t current_candidate_iteration_ = 0u;
    bool candidate_failed_ = false;
    bool round_failed_ = false;
};

} // namespace katana::codegen::detail
