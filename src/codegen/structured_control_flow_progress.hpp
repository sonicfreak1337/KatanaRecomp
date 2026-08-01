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
        control_counters.growing_workset =
            progress.growing_workset;
        if (progress.function_value_active)
            control_counters.configured_workers =
                progress.function_value_configured_workers;
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
                "function-values-complete");
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
        function_counters.discovered =
            progress.function_value_functions;
        function_counters.started =
            progress.function_value_physical_evaluations;
        append_cache_counters(function_counters, progress);
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
            resolution_counters.ready_ahead =
                progress.function_value_resolution_functions_ready -
                std::min(
                    progress.function_value_resolution_functions_ready,
                    progress
                        .function_value_resolution_functions_committed);
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
    static bool is_explicit_function_subphase(
        const std::string_view subphase) noexcept {
        constexpr std::array<std::string_view, 7u> names{
            "inventory-region-closure",
            "abi-return-signatures",
            "abi-stack-reads",
            "abi-register-reads",
            "persistent-store-signatures",
            "inventory-reachability",
            "cache-key-plan"};
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
        append_cache_counters(counters, progress);
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
        counters.growing_workset =
            progress.growing_workset;
        counters.committed_work = progress.instructions;
        if (progress.function_value_active)
            counters.configured_workers =
                progress.function_value_configured_workers;
        round_->update(progress.instructions,
                       std::move(counters));
    }

    void ensure_candidate_iteration(
        const katana::analysis::ControlFlowAnalysisProgress&
            progress) {
        if (round_failed_ ||
            progress.candidate_contract_iteration == 0u ||
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
        counters.added_work = progress.round_added_seeds;
        counters.growing_workset =
            progress.growing_workset;
        append_cache_counters(counters, progress);
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
