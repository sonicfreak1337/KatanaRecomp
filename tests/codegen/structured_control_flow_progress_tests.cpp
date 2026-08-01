#include "structured_control_flow_progress.hpp"

#include <algorithm>
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
                       katana::progress_event_telemetry_complete(
                           event);
            });
    require(
        observed_resolution_metrics,
        "Die strukturierte Candidate-Resolution verlor planned/ready/"
        "committed, HOL, Worker- oder Cachebilanz.");

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
        "Ein inaktives FunctionValue-Abschlussevent setzte den "
        "Candidate-Fortschritt auf null zurueck.");

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
