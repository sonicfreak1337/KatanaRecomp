#include "katana/progress.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace progress_allocation_fault {
std::atomic_bool armed = false;
std::atomic_bool consumed = false;
std::atomic_size_t minimum_size = 0u;
std::atomic_size_t maximum_size = 0u;

void arm(const std::size_t minimum, const std::size_t maximum) noexcept {
    minimum_size.store(minimum, std::memory_order_relaxed);
    maximum_size.store(maximum, std::memory_order_relaxed);
    consumed.store(false, std::memory_order_relaxed);
    armed.store(true, std::memory_order_release);
}

void disarm() noexcept {
    armed.store(false, std::memory_order_release);
}

[[nodiscard]] bool consume(const std::size_t size) noexcept {
    if (!armed.load(std::memory_order_acquire) ||
        size < minimum_size.load(std::memory_order_relaxed) ||
        size > maximum_size.load(std::memory_order_relaxed))
        return false;
    auto expected = true;
    if (!armed.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel, std::memory_order_acquire))
        return false;
    consumed.store(true, std::memory_order_release);
    return true;
}
} // namespace progress_allocation_fault

void* operator new(const std::size_t size) {
    if (progress_allocation_fault::consume(size)) throw std::bad_alloc();
    if (auto* const allocation = std::malloc(std::max<std::size_t>(size, 1u)))
        return allocation;
    throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* const allocation) noexcept {
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept {
    ::operator delete(allocation);
}

void operator delete(void* const allocation, const std::size_t) noexcept {
    ::operator delete(allocation);
}

void operator delete[](void* const allocation, const std::size_t) noexcept {
    ::operator delete(allocation);
}

namespace {
void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    std::vector<katana::ProgressEvent> events;
    const katana::ProgressReporter reporter(
        [&](const katana::ProgressEvent& event) { events.push_back(event); },
        std::chrono::milliseconds(0));

    auto root = reporter.begin(katana::ProgressOperation::ControlFlowAnalysis,
                               katana::ProgressUnit::None,
                               std::nullopt,
                               "analysis");
    auto analysis = root.child_reporter().begin(katana::ProgressOperation::FunctionValueAnalysis,
                                                katana::ProgressUnit::Functions,
                                                std::nullopt,
                                                "function-values");
    katana::ProgressCounterSnapshot first;
    first.iteration = 17u;
    first.pass = 2u;
    first.active_workers = 24u;
    first.queued_work = 133u;
    first.discovered = 1436u;
    first.started = 1307u;
    first.requeued = 12u;
    first.cache_hits = 42u;
    first.cache_misses = 7u;
    first.planned_work = 150u;
    first.ready_work = 5u;
    first.committed_work = 17u;
    first.configured_workers = 24u;
    first.added_work = 9u;
    first.growing_workset = true;
    first.head_of_line_index = 17u;
    first.head_of_line_elapsed_milliseconds = 9000u;
    first.ready_ahead = 5u;
    first.cache_lookups = 49u;
    first.cache_ready_hits = 35u;
    first.cache_in_flight_coalesces = 7u;
    first.cache_replay_fallback_recomputes = 3u;
    first.cache_evictions = 2u;
    first.cache_entries = 128u;
    first.cache_retained_payload_bytes = 4096u;
    first.cache_miss_cold = 3u;
    first.cache_miss_evicted = 2u;
    first.cache_miss_function_shape_changed = 1u;
    first.cache_miss_projected_ingress_changed = 1u;
    analysis.update(3861u, first);
    auto heartbeat = first;
    heartbeat.queued_work = 80u;
    heartbeat.started = 1361u;
    heartbeat.cache_replay_fallback_recomputes = 4u;
    analysis.heartbeat(heartbeat);
    analysis.complete(4163u);

    auto cached = root.child_reporter().begin(
        katana::ProgressOperation::Packaging, katana::ProgressUnit::Files, 3u, "cached-package");
    cached.cached();
    auto skipped = root.child_reporter().begin(katana::ProgressOperation::Linking,
                                               katana::ProgressUnit::Steps,
                                               std::nullopt,
                                               "skipped-link");
    skipped.skipped();
    auto invalid = root.child_reporter().begin(katana::ProgressOperation::Compilation,
                                               katana::ProgressUnit::TranslationUnits,
                                               1u,
                                               "invalid-counter");
    invalid.update(2u);
    root.complete();
    require(reporter.seal_and_flush(),
            "Vollstaendiger Progresslauf kann nicht atomar versiegelt werden.");

    require(events.size() >= 12u, "Progress-Core verliert erforderliche Ereignisse.");
    std::uint64_t previous_elapsed = 0u;
    for (std::size_t index = 0u; index < events.size(); ++index) {
        require(events[index].sequence == index + 1u,
                "Progress-Core serialisiert Sequenzen nicht lueckenlos.");
        require(events[index].elapsed_milliseconds >= previous_elapsed,
                "Progress-Core meldet rueckwaerts laufende Zeit.");
        previous_elapsed = events[index].elapsed_milliseconds;
    }
    const auto heartbeat_event = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.state == katana::ProgressState::Heartbeat;
    });
    require(heartbeat_event != events.end() && !heartbeat_event->total &&
                heartbeat_event->completed == 3861u && heartbeat_event->counters.iteration == 17u &&
                heartbeat_event->counters.active_workers == 24u &&
                heartbeat_event->counters.queued_work == 80u &&
                heartbeat_event->counters.started == 1361u &&
                heartbeat_event->counters.planned_work == 150u &&
                heartbeat_event->counters.ready_work == 5u &&
                heartbeat_event->counters.committed_work == 17u &&
                heartbeat_event->counters.configured_workers == 24u &&
                heartbeat_event->counters.added_work == 9u &&
                heartbeat_event->counters.growing_workset == true &&
                heartbeat_event->counters.head_of_line_index == 17u &&
                heartbeat_event->counters.ready_ahead == 5u &&
                heartbeat_event->counters.cache_lookups == 49u &&
                heartbeat_event->counters.cache_ready_hits == 35u &&
                heartbeat_event->counters.cache_in_flight_coalesces == 7u &&
                heartbeat_event->counters.cache_replay_fallback_recomputes == 4u &&
                katana::progress_event_telemetry_complete(*heartbeat_event),
            "Dynamischer Heartbeat verliert Zaehler oder erfindet einen Gesamtwert.");
    const auto cached_event = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.state == katana::ProgressState::Cached;
    });
    require(cached_event != events.end() && cached_event->completed == 3u &&
                cached_event->total == 3u,
            "Cacheabschluss meldet keinen exakten bekannten Umfang.");
    const auto skipped_event = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.state == katana::ProgressState::Skipped;
    });
    require(skipped_event != events.end() && !skipped_event->total &&
                skipped_event->completed == 0u,
            "Uebersprungener dynamischer Vorgang erfindet Fortschritt.");
    const auto failed_event = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.state == katana::ProgressState::Failed;
    });
    require(failed_event != events.end() && failed_event->completed == 0u &&
                failed_event->total == 1u,
            "Ungueltige Progressdaten beeinflussen den Vorgang statt fail-closed zu melden.");
    require(analysis.total() == std::nullopt &&
                progress_operation_name(katana::ProgressOperation::FunctionValueAnalysis) ==
                    "function-value-analysis" &&
                progress_operation_name(katana::ProgressOperation::PortBuild) == "port-build" &&
                progress_operation_name(katana::ProgressOperation::ControlFlowRound) ==
                    "control-flow-round" &&
                progress_operation_name(katana::ProgressOperation::CandidateContractIteration) ==
                    "candidate-contract-iteration" &&
                progress_state_name(katana::ProgressState::Heartbeat) == "heartbeat",
            "Dynamische Progressmetadaten sind nicht stabil benannt.");

    katana::ProgressEvent formatted;
    formatted.operation = katana::ProgressOperation::CandidateContractIteration;
    formatted.state = katana::ProgressState::Heartbeat;
    formatted.unit = katana::ProgressUnit::Functions;
    formatted.sequence = 9u;
    formatted.elapsed_milliseconds = 1234u;
    formatted.scope_id = 44u;
    formatted.parent_scope_id = 7u;
    formatted.completed = 8u;
    formatted.total = 10u;
    formatted.scope_elapsed_milliseconds = 456u;
    formatted.label = std::string("candidate \"A\"\n") + static_cast<char>(1);
    formatted.counters.active_workers = 2u;
    formatted.counters.evaluation_requests = 13u;
    formatted.counters.active_evaluation_requests = 0u;
    formatted.counters.evaluation_request_nanoseconds = 13u;
    formatted.counters.maximum_evaluation_request_nanoseconds = 1u;
    formatted.counters.physical_evaluations = 5u;
    formatted.counters.active_physical_evaluations = 0u;
    formatted.counters.physical_evaluation_nanoseconds = 5u;
    formatted.counters.maximum_physical_evaluation_nanoseconds = 1u;
    formatted.counters.queued_work = 4u;
    formatted.counters.cache_hits = 8u;
    formatted.counters.cache_misses = 2u;
    formatted.counters.planned_work = 10u;
    formatted.counters.ready_work = 1u;
    formatted.counters.committed_work = 3u;
    formatted.counters.configured_workers = 24u;
    formatted.counters.added_work = 2u;
    formatted.counters.growing_workset = true;
    formatted.counters.head_of_line_index = 3u;
    formatted.counters.head_of_line_elapsed_milliseconds = 75u;
    formatted.counters.ready_ahead = 1u;
    formatted.counters.cache_lookups = 10u;
    formatted.counters.cache_ready_hits = 6u;
    formatted.counters.cache_in_flight_coalesces = 2u;
    formatted.counters.cache_replay_fallback_recomputes = 3u;
    formatted.counters.cache_diagnostic_bypass_evaluations = 0u;
    formatted.counters.multi_root_context_requests = 5u;
    formatted.counters.multi_root_unique_contexts = 2u;
    formatted.counters.multi_root_ready_reuses = 2u;
    formatted.counters.multi_root_in_flight_reuses = 1u;
    formatted.counters.multi_root_provenance_links = 6u;
    formatted.counters.multi_root_retained_contexts = 2u;
    formatted.counters.multi_root_retained_payload_bytes = 2048u;
    formatted.counters.multi_root_evictions = 0u;
    formatted.counters.cache_evictions = 1u;
    formatted.counters.cache_entries = 64u;
    formatted.counters.cache_retained_payload_bytes = 4096u;
    formatted.counters.cache_miss_cold = 1u;
    formatted.counters.cache_miss_evicted = 1u;
    const auto formatted_json = katana::format_progress_event_json(formatted);
    const auto formatted_human = katana::format_progress_event_human(formatted);
    require(formatted_json == "{\"schema\":\"katana-progress-v1\",\"schema_version\":1,"
                              "\"operation\":\"candidate-contract-iteration\","
                              "\"state\":\"heartbeat\",\"unit\":\"functions\","
                              "\"sequence\":9,\"elapsed_ms\":1234,"
                              "\"scope_elapsed_ms\":456,\"scope_id\":44,"
                              "\"parent_scope_id\":7,\"completed\":8,\"total\":10,"
                              "\"counters\":{\"active_workers\":2,"
                              "\"evaluation_requests\":13,"
                              "\"active_evaluation_requests\":0,"
                              "\"evaluation_request_nanoseconds\":13,"
                              "\"maximum_evaluation_request_nanoseconds\":1,"
                              "\"physical_evaluations\":5,"
                              "\"active_physical_evaluations\":0,"
                              "\"physical_evaluation_nanoseconds\":5,"
                              "\"maximum_physical_evaluation_nanoseconds\":1,"
                              "\"queued_work\":4,"
                              "\"cache_hits\":8,\"cache_misses\":2,\"planned_work\":10,"
                              "\"ready_work\":1,\"committed_work\":3,"
                              "\"configured_workers\":24,\"added_work\":2,"
                              "\"growing_workset\":true,\"head_of_line_index\":3,"
                              "\"head_of_line_elapsed_milliseconds\":75,"
                              "\"ready_ahead\":1,\"cache_lookups\":10,"
                              "\"cache_ready_hits\":6,\"cache_in_flight_coalesces\":2,"
                              "\"cache_replay_fallback_recomputes\":3,"
                              "\"cache_diagnostic_bypass_evaluations\":0,"
                              "\"multi_root_context_requests\":5,"
                              "\"multi_root_unique_contexts\":2,"
                              "\"multi_root_ready_reuses\":2,"
                              "\"multi_root_in_flight_reuses\":1,"
                              "\"multi_root_provenance_links\":6,"
                              "\"multi_root_retained_contexts\":2,"
                              "\"multi_root_retained_payload_bytes\":2048,"
                              "\"multi_root_evictions\":0,"
                              "\"cache_evictions\":1,\"cache_entries\":64,"
                              "\"cache_retained_payload_bytes\":4096,"
                              "\"cache_miss_cold\":1,"
                              "\"cache_miss_evicted\":1},\"dropped_observations\":0,"
                              "\"telemetry_complete\":true,"
                              "\"label\":\"candidate \\\"A\\\"\\n\\u0001\"}",
            "JSONL-Formatter ist nicht schema- oder reihenfolgestabil.");
    require(formatted_human == "KATANA_PROGRESS operation=candidate-contract-iteration "
                               "state=heartbeat schema=katana-progress-v1 schema_version=1 "
                               "elapsed_ms=1234 scope_elapsed_ms=456 scope=44 parent=7 "
                               "unit=functions completed=8 total=10 percent_milli=80000 "
                               "active_workers=2 evaluation_requests=13 "
                               "active_evaluation_requests=0 "
                               "evaluation_request_nanoseconds=13 "
                               "maximum_evaluation_request_nanoseconds=1 "
                               "physical_evaluations=5 active_physical_evaluations=0 "
                               "physical_evaluation_nanoseconds=5 "
                               "maximum_physical_evaluation_nanoseconds=1 "
                               "queued_work=4 cache_hits=8 cache_misses=2 "
                               "planned_work=10 ready_work=1 committed_work=3 "
                               "configured_workers=24 added_work=2 growing_workset=true "
                               "head_of_line_index=3 "
                               "head_of_line_elapsed_milliseconds=75 ready_ahead=1 "
                               "cache_lookups=10 cache_ready_hits=6 "
                               "cache_in_flight_coalesces=2 "
                               "cache_replay_fallback_recomputes=3 "
                               "cache_diagnostic_bypass_evaluations=0 "
                               "multi_root_context_requests=5 "
                               "multi_root_unique_contexts=2 "
                               "multi_root_ready_reuses=2 "
                               "multi_root_in_flight_reuses=1 "
                               "multi_root_provenance_links=6 "
                               "multi_root_retained_contexts=2 "
                               "multi_root_retained_payload_bytes=2048 "
                               "multi_root_evictions=0 "
                               "cache_evictions=1 "
                               "cache_entries=64 "
                               "cache_retained_payload_bytes=4096 "
                               "cache_miss_cold=1 "
                               "cache_miss_evicted=1 dropped_observations=0 "
                               "telemetry_complete=true label=\"candidate \\\"A\\\"\\n\\u0001\"",
            "Menschenlesbarer Formatter ist nicht stabil oder kompatibel.");
    require(katana::progress_event_schema_version == 1u &&
                katana::progress_event_schema == "katana-progress-v1" &&
                katana::progress_cache_accounting_valid(formatted.counters) &&
                katana::progress_activity_accounting_valid(
                    formatted.counters),
            "Schema-v1-, Replay-Fallback- oder Cache-Ledger-Vertrag ist nicht stabil.");
    katana::ProgressEvent executor_event;
    executor_event.counters.executor_running_workers = 12u;
    executor_event.counters.executor_waiting_workers = 4u;
    executor_event.counters.executor_idle_workers = 0u;
    executor_event.counters.executor_queued_work = 7u;
    executor_event.counters.executor_memory_blocked_work = 2u;
    executor_event.counters.executor_continuations = 3u;
    executor_event.counters.analysis_memory_capacity_bytes =
        8'589'934'592u;
    executor_event.counters.analysis_memory_used_bytes =
        1'610'612'736u;
    executor_event.counters.analysis_memory_peak_bytes =
        3'221'225'472u;
    const auto executor_json =
        katana::format_progress_event_json(executor_event);
    const auto executor_human =
        katana::format_progress_event_human(executor_event);
    require(
        katana::progress_event_telemetry_complete(executor_event) &&
            executor_json.find("\"executor_running_workers\":12") !=
                std::string::npos &&
            executor_json.find("\"executor_memory_blocked_work\":2") !=
                std::string::npos &&
            executor_json.find("\"analysis_memory_used_bytes\":1610612736") !=
                std::string::npos &&
            executor_human.find("executor_continuations=3") !=
                std::string::npos &&
            executor_human.find("analysis_memory_peak_bytes=3221225472") !=
                std::string::npos,
        "Executor-/Speichertelemetrie erreichte JSONL/Human nicht vollstaendig.");
    auto incomplete_executor_event = executor_event;
    incomplete_executor_event.counters.executor_queued_work.reset();
    require(
        !katana::progress_event_telemetry_complete(
            incomplete_executor_event),
        "Ein unvollstaendiges Executor-Ledger blieb gruen.");
    auto impossible_executor_event = executor_event;
    impossible_executor_event.counters.executor_idle_workers = 5u;
    require(
        !katana::progress_event_telemetry_complete(
            impossible_executor_event),
        "Mehr Idle- als wartende Executor-Worker blieben gruen.");
    impossible_executor_event = executor_event;
    impossible_executor_event.counters.analysis_memory_peak_bytes =
        9'000'000'000u;
    require(
        !katana::progress_event_telemetry_complete(
            impossible_executor_event),
        "Ein Speicherpeak oberhalb des Executor-Budgets blieb gruen.");
    katana::ProgressCounterSnapshot activity;
    activity.evaluation_requests = 5u;
    activity.active_evaluation_requests = 2u;
    activity.evaluation_request_nanoseconds = 30u;
    activity.maximum_evaluation_request_nanoseconds = 15u;
    activity.cache_key_builds = 5u;
    activity.active_cache_key_builds = 0u;
    activity.cache_key_build_nanoseconds = 20u;
    activity.maximum_cache_key_build_nanoseconds = 8u;
    activity.cache_waits = 1u;
    activity.active_cache_waits = 1u;
    activity.cache_wait_nanoseconds = 0u;
    activity.maximum_cache_wait_nanoseconds = 0u;
    activity.cache_replays = 2u;
    activity.active_cache_replays = 0u;
    activity.cache_replay_nanoseconds = 7u;
    activity.maximum_cache_replay_nanoseconds = 4u;
    activity.physical_evaluations = 3u;
    activity.active_physical_evaluations = 1u;
    activity.physical_evaluation_nanoseconds = 12u;
    activity.maximum_physical_evaluation_nanoseconds = 8u;
    activity.cache_commits = 2u;
    activity.active_cache_commits = 0u;
    activity.cache_commit_nanoseconds = 5u;
    activity.maximum_cache_commit_nanoseconds = 3u;
    require(
        katana::progress_activity_accounting_valid(activity),
        "Gueltige parallele Aktivitaets-/Zeitzaehler wurden abgelehnt.");
    auto partial_activity = activity;
    partial_activity.cache_wait_nanoseconds.reset();
    require(
        !katana::progress_activity_accounting_valid(partial_activity),
        "Ein unvollstaendiges Aktivitaetsquartett blieb gruen.");
    auto zero_first_then_partial = partial_activity;
    zero_first_then_partial.evaluation_requests = 0u;
    zero_first_then_partial.active_evaluation_requests = 0u;
    zero_first_then_partial.evaluation_request_nanoseconds = 0u;
    zero_first_then_partial.maximum_evaluation_request_nanoseconds = 0u;
    require(
        !katana::progress_activity_accounting_valid(
            zero_first_then_partial),
        "Ein gueltiges erstes Nulltupel uebersprang einen spaeteren "
        "unvollstaendigen Aktivitaetsvertrag.");
    auto impossible_activity = activity;
    impossible_activity.active_cache_replays = 3u;
    require(
        !katana::progress_activity_accounting_valid(impossible_activity),
        "Mehr aktive als gestartete Replay-Operationen blieben gruen.");
    auto impossible_duration = activity;
    impossible_duration.maximum_cache_commit_nanoseconds = 6u;
    require(
        !katana::progress_activity_accounting_valid(impossible_duration),
        "Eine Maximaldauer oberhalb der kumulativen Dauer blieb gruen.");
    katana::ProgressEvent activity_event;
    activity_event.counters = activity;
    const auto activity_json =
        katana::format_progress_event_json(activity_event);
    const auto activity_human =
        katana::format_progress_event_human(activity_event);
    require(
        activity_json.find("\"active_evaluation_requests\":2") !=
                std::string::npos &&
            activity_json.find("\"cache_key_build_nanoseconds\":20") !=
                std::string::npos &&
            activity_json.find("\"physical_evaluations\":3") !=
                std::string::npos &&
            activity_json.find("\"cache_commit_nanoseconds\":5") !=
                std::string::npos &&
            activity_human.find("cache_replays=2") != std::string::npos &&
            katana::progress_event_telemetry_complete(activity_event),
        "Aktivitaets-/Zeitmetriken erreichten JSONL/Human-Terminal nicht.");
    auto invalid_accounting = formatted;
    invalid_accounting.counters.cache_lookups = 11u;
    require(!katana::progress_cache_accounting_valid(invalid_accounting.counters) &&
                !katana::progress_event_telemetry_complete(invalid_accounting) &&
                katana::format_progress_event_json(invalid_accounting)
                        .find("\"telemetry_complete\":false") != std::string::npos,
            "Unvollstaendige Cachezaehlung wird als vollstaendige Telemetrie ausgegeben.");
    auto invalid_incremental_ledger = formatted;
    invalid_incremental_ledger.state =
        katana::ProgressState::Completed;
    invalid_incremental_ledger.counters.analysis_epochs_published = 1u;
    invalid_incremental_ledger.counters.analysis_epochs_discarded = 0u;
    invalid_incremental_ledger.counters.incremental_epochs_started = 1u;
    invalid_incremental_ledger.counters.resolution_root_artifacts_total = 2u;
    invalid_incremental_ledger.counters.resolution_root_artifacts_reused = 2u;
    invalid_incremental_ledger.counters.resolution_root_artifacts_recomputed = 1u;
    invalid_incremental_ledger.counters.resolution_root_artifacts_retained = 2u;
    invalid_incremental_ledger.counters.resolution_epoch_retained_bytes =
        2'048u;
    invalid_incremental_ledger.counters.resolution_retention_limit_reason =
        "none";
    invalid_incremental_ledger.counters.persistent_analysis_bypass_reason =
        "none";
    invalid_incremental_ledger.counters.dirty_sccs = 1u;
    invalid_incremental_ledger.counters.dirty_functions = 1u;
    invalid_incremental_ledger.counters.dirty_inventory_sinks = 0u;
    invalid_incremental_ledger.counters.full_cpu_recompute_fallbacks = 0u;
    require(
        !katana::progress_event_telemetry_complete(
            invalid_incremental_ledger) &&
            katana::format_progress_event_json(
                invalid_incremental_ledger)
                    .find("\"telemetry_complete\":false") !=
                std::string::npos,
        "Ein unausgeglichenes Root-Artefaktledger blieb telemetrisch "
        "vollstaendig.");
    auto partial_seed_ledger = formatted;
    partial_seed_ledger.counters.round_seed_facts_added = 1u;
    require(
        !katana::progress_event_telemetry_complete(partial_seed_ledger),
        "Eine partielle Seed-Rundengruppe blieb telemetrisch vollstaendig.");
    katana::ProgressEvent partial_physical_work;
    partial_physical_work.counters.program_delta_entries_visited = 1u;
    require(
        !katana::progress_event_telemetry_complete(
            partial_physical_work),
        "Eine partielle physische Arbeitsgruppe blieb telemetrisch "
        "vollstaendig.");
    katana::ProgressEvent invalid_seed_subset;
    invalid_seed_subset.counters.round_seed_facts_added = 2u;
    invalid_seed_subset.counters.round_seed_targets_changed = 1u;
    invalid_seed_subset.counters.round_decode_targets = 2u;
    invalid_seed_subset.counters.round_metadata_targets = 0u;
    invalid_seed_subset.counters.round_full_cpu_fallbacks = 0u;
    invalid_seed_subset.counters.growing_workset = true;
    require(
        !katana::progress_event_telemetry_complete(invalid_seed_subset),
        "Decode-Targets ausserhalb der geaenderten Seedmenge blieben gruen.");
    auto invalid_growing_workset = invalid_seed_subset;
    invalid_growing_workset.counters.round_seed_targets_changed = 0u;
    invalid_growing_workset.counters.round_decode_targets = 0u;
    require(
        !katana::progress_event_telemetry_complete(
            invalid_growing_workset),
        "Ein wachsender Workset ohne Seeddelta oder Fallback blieb gruen.");
    katana::ProgressEvent partial_epoch_ledger;
    partial_epoch_ledger.counters.analysis_epochs_published = 1u;
    require(
        !katana::progress_event_telemetry_complete(partial_epoch_ledger),
        "Ein allein publizierter Epoch-Zaehler blieb vollstaendig.");
    auto impossible_live_epoch = invalid_incremental_ledger;
    impossible_live_epoch.state = katana::ProgressState::Running;
    impossible_live_epoch.counters.resolution_root_artifacts_total = 3u;
    impossible_live_epoch.counters.analysis_epochs_published = 2u;
    require(
        !katana::progress_event_telemetry_complete(
            impossible_live_epoch),
        "Mehr abgeschlossene als gestartete Live-Epochen blieben gruen.");
    auto unsettled_terminal_epoch = invalid_incremental_ledger;
    unsettled_terminal_epoch.counters.resolution_root_artifacts_total = 3u;
    unsettled_terminal_epoch.counters.analysis_epochs_published = 0u;
    require(
        !katana::progress_event_telemetry_complete(
            unsettled_terminal_epoch),
        "Eine terminal unvollstaendige Epochenbilanz blieb gruen.");
    auto overflowing_root_ledger = invalid_incremental_ledger;
    overflowing_root_ledger.counters.resolution_root_artifacts_total =
        std::numeric_limits<std::uint64_t>::max();
    overflowing_root_ledger.counters.resolution_root_artifacts_reused =
        std::numeric_limits<std::uint64_t>::max();
    require(
        !katana::progress_event_telemetry_complete(
            overflowing_root_ledger),
        "Ein ueberlaufendes Root-Artefaktledger blieb gruen.");
    auto partial_retention_after_limit = invalid_incremental_ledger;
    partial_retention_after_limit.counters.resolution_root_artifacts_total =
        3u;
    partial_retention_after_limit.counters
        .resolution_root_artifacts_retained = 1u;
    partial_retention_after_limit.counters
        .resolution_retention_limit_reason = "byte-limit";
    require(
        !katana::progress_event_telemetry_complete(
            partial_retention_after_limit),
        "Ein Byte-Limit mit partiell behaltenem Root blieb gruen.");
    auto incomplete_root_retention = invalid_incremental_ledger;
    incomplete_root_retention.counters.resolution_root_artifacts_total = 3u;
    incomplete_root_retention.counters
        .resolution_root_artifacts_retained = 0u;
    incomplete_root_retention.counters
        .resolution_epoch_retained_bytes = 0u;
    incomplete_root_retention.counters
        .resolution_retention_limit_reason = "incomplete-root";
    require(
        katana::progress_event_telemetry_complete(
            incomplete_root_retention) &&
            katana::format_progress_event_json(
                incomplete_root_retention)
                    .find("\"resolution_retention_limit_reason\":"
                          "\"incomplete-root\"") !=
                std::string::npos,
        "Ein vollstaendig verworfener unvollstaendiger Root erreichte "
        "Progress-/JSON-Telemetrie nicht als typisierter Retentiongrund.");
    auto unknown_retention_reason = invalid_incremental_ledger;
    unknown_retention_reason.counters.resolution_root_artifacts_total = 3u;
    unknown_retention_reason.counters.resolution_retention_limit_reason =
        "mystery-limit";
    require(
        !katana::progress_event_telemetry_complete(
            unknown_retention_reason),
        "Ein untypisierter Retention-Limitgrund blieb gruen.");
    katana::ProgressEvent unknown_bypass_reason;
    unknown_bypass_reason.counters.persistent_analysis_bypass_reason =
        "mystery-fallback";
    require(
        !katana::progress_event_telemetry_complete(
            unknown_bypass_reason),
        "Ein untypisierter Persistent-Bypassgrund blieb gruen.");

    std::atomic_bool callback_saw_invalid_incremental = false;
    const katana::ProgressReporter invalid_incremental_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.label == "invalid-incremental-ledger" &&
                event.state != katana::ProgressState::Started &&
                !event.telemetry_complete)
                callback_saw_invalid_incremental.store(
                    true, std::memory_order_release);
        },
        std::chrono::milliseconds(0));
    auto invalid_incremental_scope =
        invalid_incremental_reporter.begin(
            katana::ProgressOperation::FunctionValueAnalysis,
            katana::ProgressUnit::Functions,
            1u,
            "invalid-incremental-ledger");
    invalid_incremental_scope.update(
        0u, invalid_incremental_ledger.counters);
    invalid_incremental_scope.complete(1u);
    require(
        invalid_incremental_reporter.seal_and_flush() &&
            callback_saw_invalid_incremental.load(
                std::memory_order_acquire),
        "Der Progress-Callback markierte ein ungueltiges "
        "Inkremental-Ledger faelschlich als vollstaendig.");
    auto invalid_hit_aggregate = formatted;
    invalid_hit_aggregate.counters.cache_hits = 9u;
    require(
        !katana::progress_cache_accounting_valid(
            invalid_hit_aggregate.counters) &&
            !katana::progress_event_telemetry_complete(
                invalid_hit_aggregate),
        "Ein falsches Cachehit-Aggregat blieb trotz exakter Splitzaehler "
        "telemetrisch vollstaendig.");
    auto invalid_multi_root_ledger = formatted;
    invalid_multi_root_ledger.counters.multi_root_context_requests = 6u;
    require(
        !katana::progress_cache_accounting_valid(
            invalid_multi_root_ledger.counters) &&
            !katana::progress_event_telemetry_complete(
                invalid_multi_root_ledger),
        "Ein widerspruechliches Multi-Root-Fanout-Ledger blieb "
        "telemetrisch vollstaendig.");
    auto invalid_multi_root_retention = formatted;
    invalid_multi_root_retention.counters.multi_root_retained_contexts = 3u;
    require(
        !katana::progress_cache_accounting_valid(
            invalid_multi_root_retention.counters),
        "Ein vom Unique-Ledger abweichender Multi-Root-Retentionzaehler "
        "blieb gruen.");
    auto valid_multi_root_eviction = formatted;
    valid_multi_root_eviction.counters.multi_root_retained_contexts = 1u;
    valid_multi_root_eviction.counters.multi_root_evictions = 1u;
    require(
        katana::progress_cache_accounting_valid(
            valid_multi_root_eviction.counters) &&
            katana::format_progress_event_json(
                valid_multi_root_eviction)
                    .find("\"multi_root_evictions\":1") !=
                std::string::npos,
        "Ein gueltiges Multi-Root-Eviction-Ledger wurde verworfen oder "
        "nicht formatiert.");
    auto invalid_multi_root_zero_retention = formatted;
    invalid_multi_root_zero_retention.counters.multi_root_context_requests = 3u;
    invalid_multi_root_zero_retention.counters.multi_root_unique_contexts = 0u;
    invalid_multi_root_zero_retention.counters.multi_root_retained_contexts = 0u;
    require(
        !katana::progress_cache_accounting_valid(
            invalid_multi_root_zero_retention.counters),
        "Ein leerer Multi-Root-Retentionzaehler mit Payload blieb gruen.");
    auto overflowing_multi_root_ledger = formatted;
    overflowing_multi_root_ledger.counters.multi_root_context_requests =
        std::numeric_limits<std::uint64_t>::max();
    overflowing_multi_root_ledger.counters.multi_root_unique_contexts =
        std::numeric_limits<std::uint64_t>::max();
    overflowing_multi_root_ledger.counters.multi_root_ready_reuses = 1u;
    overflowing_multi_root_ledger.counters.multi_root_in_flight_reuses = 0u;
    overflowing_multi_root_ledger.counters.multi_root_retained_contexts =
        std::numeric_limits<std::uint64_t>::max();
    require(
        !katana::progress_cache_accounting_valid(
            overflowing_multi_root_ledger.counters),
        "Ein ueberlaufender Multi-Root-Ledger blieb gruen.");
    auto incomplete_multi_root_ledger = formatted;
    incomplete_multi_root_ledger.counters
        .multi_root_provenance_links.reset();
    require(
        !katana::progress_cache_accounting_valid(
            incomplete_multi_root_ledger.counters),
        "Ein unvollstaendiges Multi-Root-Ledger blieb gruen.");
    katana::ProgressCounterSnapshot legacy_cache;
    legacy_cache.cache_hits = 42u;
    legacy_cache.cache_misses = 7u;
    require(katana::progress_cache_accounting_valid(legacy_cache),
            "Der neue Split-Cachevertrag bricht alte Cachezaehler.");
    auto missing_reason_ledger = formatted.counters;
    missing_reason_ledger.cache_miss_cold.reset();
    missing_reason_ledger.cache_miss_evicted.reset();
    require(!katana::progress_cache_accounting_valid(missing_reason_ledger),
            "Split-Cachemisses bleiben ohne vollstaendiges Miss-Reason-Ledger gruen.");
    katana::ProgressCounterSnapshot all_miss_reasons;
    all_miss_reasons.cache_misses = 12u;
    all_miss_reasons.cache_miss_cold = 1u;
    all_miss_reasons.cache_miss_evicted = 1u;
    all_miss_reasons.cache_miss_oversize_or_no_exact_replay = 1u;
    all_miss_reasons.cache_miss_function_shape_changed = 1u;
    all_miss_reasons.cache_miss_projected_ingress_changed = 1u;
    all_miss_reasons.cache_miss_summary_dependency_changed = 1u;
    all_miss_reasons.cache_miss_abi_contract_changed = 1u;
    all_miss_reasons.cache_miss_resolution_lens_changed = 1u;
    all_miss_reasons.cache_miss_inventory_sink_changed = 1u;
    all_miss_reasons.cache_miss_isolation_partition_changed = 1u;
    all_miss_reasons.cache_miss_contextual_summary_changed = 1u;
    all_miss_reasons.cache_miss_tail_ingress_changed = 1u;
    auto all_reasons_event = formatted;
    all_reasons_event.counters = all_miss_reasons;
    const auto all_reasons_json = katana::format_progress_event_json(all_reasons_event);
    require(katana::progress_cache_accounting_valid(all_miss_reasons) &&
                all_reasons_json.find("\"cache_miss_oversize_or_no_exact_replay\":1") !=
                    std::string::npos &&
                all_reasons_json.find("\"cache_miss_tail_ingress_changed\":1") != std::string::npos,
            "Das primaere Miss-Reason-Ledger ist unvollstaendig oder falsch summiert.");

    const katana::ProgressReporter constructor_fault_reporter(
        [](const katana::ProgressEvent&) {},
        std::chrono::milliseconds(0));
    std::string constructor_fault_label = "constructor-allocation-fault";
    progress_allocation_fault::arm(
        1u, std::numeric_limits<std::size_t>::max());
    auto constructor_fault_scope = constructor_fault_reporter.begin(
        katana::ProgressOperation::ArtifactWrite,
        katana::ProgressUnit::Files,
        1u,
        std::move(constructor_fault_label));
    progress_allocation_fault::disarm();
    require(progress_allocation_fault::consumed.load(std::memory_order_acquire) &&
                !constructor_fault_scope.enabled() &&
                constructor_fault_reporter.dropped_observations() == 1u &&
                !constructor_fault_reporter.seal_and_flush(),
            "Scope-Konstruktorverlust entkommt der Producer-/Seal-Epoche.");

    std::atomic_bool heartbeat_fault_started = false;
    constexpr auto heartbeat_fault_interval = std::chrono::milliseconds(20);
    const katana::ProgressReporter heartbeat_fault_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.state == katana::ProgressState::Started)
                heartbeat_fault_started.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        heartbeat_fault_interval);
    constexpr std::size_t heartbeat_fault_label_size = 131'071u;
    std::string heartbeat_fault_label(heartbeat_fault_label_size, 'h');
    auto heartbeat_fault_scope = heartbeat_fault_reporter.begin(
        katana::ProgressOperation::HostRuntimeBuild,
        katana::ProgressUnit::Steps,
        std::nullopt,
        std::move(heartbeat_fault_label));
    const auto heartbeat_fault_start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!heartbeat_fault_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < heartbeat_fault_start_deadline)
        std::this_thread::yield();
    require(heartbeat_fault_started.load(std::memory_order_acquire),
            "Heartbeat-Allokationstest erhielt kein Started-Ereignis.");
    progress_allocation_fault::arm(
        heartbeat_fault_label_size,
        heartbeat_fault_label_size + 4'096u);
    const auto heartbeat_fault_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!progress_allocation_fault::consumed.load(std::memory_order_acquire) ||
            heartbeat_fault_reporter.dropped_observations() == 0u) &&
           std::chrono::steady_clock::now() < heartbeat_fault_deadline)
        std::this_thread::yield();
    progress_allocation_fault::disarm();
    require(progress_allocation_fault::consumed.load(std::memory_order_acquire) &&
                heartbeat_fault_reporter.dropped_observations() == 1u,
            "Heartbeat-Copy-Allokationsverlust wurde nicht sticky erfasst.");
    heartbeat_fault_scope.complete();
    require(!heartbeat_fault_reporter.seal_and_flush(),
            "Heartbeat-Allokationsverlust blieb beim Seal vollstaendig.");

    std::atomic_bool slow_sink_entered = false;
    std::atomic_bool slow_sink_release = false;
    std::mutex slow_sink_mutex;
    std::condition_variable slow_sink_condition;
    constexpr auto fairness_heartbeat_interval = std::chrono::milliseconds(25);
    const katana::ProgressReporter slow_sink_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.state != katana::ProgressState::Started) return;
            slow_sink_entered.store(true, std::memory_order_release);
            slow_sink_condition.notify_all();
            std::unique_lock lock(slow_sink_mutex);
            slow_sink_condition.wait(lock, [&] {
                return slow_sink_release.load(std::memory_order_acquire);
            });
        },
        std::chrono::milliseconds(0),
        fairness_heartbeat_interval);
    auto slow_sink_scope = slow_sink_reporter.begin(
        katana::ProgressOperation::Compilation,
        katana::ProgressUnit::TranslationUnits,
        std::nullopt,
        "blocked-reporter");
    const auto slow_sink_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!slow_sink_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < slow_sink_deadline)
        std::this_thread::yield();
    require(slow_sink_entered.load(std::memory_order_acquire),
            "Fairnesstest konnte den langsamen Sink nicht blockieren.");

    std::atomic_bool fair_started_seen = false;
    std::atomic_bool fair_heartbeat_seen = false;
    std::atomic<std::uint64_t> fair_heartbeat_delay_ms = 0u;
    const auto fair_scope_started = std::chrono::steady_clock::now();
    const katana::ProgressReporter fair_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.state == katana::ProgressState::Started)
                fair_started_seen.store(true, std::memory_order_release);
            if (event.state == katana::ProgressState::Heartbeat) {
                const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - fair_scope_started);
                fair_heartbeat_delay_ms.store(
                    static_cast<std::uint64_t>(std::max(delay, std::chrono::milliseconds(0)).count()),
                    std::memory_order_release);
                fair_heartbeat_seen.store(true, std::memory_order_release);
            }
        },
        std::chrono::milliseconds(0),
        fairness_heartbeat_interval);
    auto fair_scope = fair_reporter.begin(
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressUnit::Functions,
        std::nullopt,
        "independent-reporter");
    const auto fair_heartbeat_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!fair_heartbeat_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < fair_heartbeat_deadline)
        std::this_thread::yield();
    require(fair_started_seen.load(std::memory_order_acquire) &&
                fair_heartbeat_seen.load(std::memory_order_acquire) &&
                fair_heartbeat_delay_ms.load(std::memory_order_acquire) <= 500u &&
                !slow_sink_release.load(std::memory_order_acquire),
            "Blockierter Sink bremst einen unabhaengigen Reporter weiterhin aus.");
    fair_scope.complete();
    require(fair_reporter.seal_and_flush(),
            "Unabhaengiger Fairness-Reporter laesst sich nicht versiegeln.");
    slow_sink_release.store(true, std::memory_order_release);
    slow_sink_condition.notify_all();
    slow_sink_scope.complete();
    require(slow_sink_reporter.seal_and_flush(),
            "Freigegebener langsamer Reporter verliert seinen Lifecycle.");

    // Exceed the global worker-slot bound sequentially. Expired slots must be
    // joined and reused instead of accumulating one thread per historical
    // reporter or rejecting a bounded live workload after enough churn.
    for (std::size_t index = 0u; index < 80u; ++index) {
        const katana::ProgressReporter transient_reporter(
            [](const katana::ProgressEvent&) {},
            std::chrono::milliseconds(0));
        auto transient_scope = transient_reporter.begin(
            katana::ProgressOperation::ArtifactWrite,
            katana::ProgressUnit::Files,
            1u,
            "worker-slot-churn");
        transient_scope.complete();
        require(transient_reporter.seal_and_flush(),
                "Abgelaufener Progress-Worker-Slot wird nicht begrenzt wiederverwendet.");
    }

    std::vector<katana::ProgressEvent> worker_events;
    std::atomic_bool worker_terminal_seen = false;
    const katana::ProgressReporter worker_reporter(
        [&](const katana::ProgressEvent& event) {
            worker_events.push_back(event);
            if (event.state == katana::ProgressState::Completed)
                worker_terminal_seen.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(0));
    auto worker_scope = worker_reporter.begin(katana::ProgressOperation::Compilation,
                                              katana::ProgressUnit::TranslationUnits,
                                              24u,
                                              "parallel-workers");
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0u; worker < 4u; ++worker) {
        workers.emplace_back([&] {
            for (std::size_t item = 0u; item < 6u; ++item)
                worker_scope.advance(1u);
        });
    }
    workers.clear();
    worker_scope.complete();
    const auto worker_terminal_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!worker_terminal_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < worker_terminal_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(worker_terminal_seen.load(std::memory_order_acquire),
            "Paralleler Worker-Scope liefert seinen Terminalabschluss nicht aus.");
    require(worker_events.back().state == katana::ProgressState::Completed &&
                worker_events.back().completed == 24u &&
                std::none_of(
                    worker_events.begin(),
                    worker_events.end(),
                    [](const auto& event) { return event.state == katana::ProgressState::Failed; }),
            "Gemeinsam genutzter Worker-Scope ist nicht serialisiert.");

    std::vector<katana::ProgressEvent> automatic_events;
    std::mutex automatic_events_mutex;
    std::atomic_bool heartbeat_seen = false;
    std::atomic_bool automatic_terminal_seen = false;
    const katana::ProgressReporter automatic_reporter(
        [&](const katana::ProgressEvent& event) {
            {
                const std::scoped_lock lock(automatic_events_mutex);
                automatic_events.push_back(event);
            }
            if (event.state == katana::ProgressState::Heartbeat)
                heartbeat_seen.store(true, std::memory_order_release);
            if (event.label == "terminal-cache" && event.state == katana::ProgressState::Cached)
                automatic_terminal_seen.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(100),
        std::chrono::milliseconds(20));
    auto silent = automatic_reporter.begin(katana::ProgressOperation::HostRuntimeBuild,
                                           katana::ProgressUnit::TranslationUnits,
                                           std::nullopt,
                                           "silent-build");
    for (std::size_t attempt = 0u; attempt < 50u && !heartbeat_seen.load(std::memory_order_acquire);
         ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    silent.complete();
    {
        const std::scoped_lock lock(automatic_events_mutex);
        require(
            heartbeat_seen.load(std::memory_order_acquire) &&
                std::any_of(automatic_events.begin(),
                            automatic_events.end(),
                            [](const auto& event) {
                                return event.state == katana::ProgressState::Heartbeat &&
                                       !event.total && event.scope_elapsed_milliseconds > 0u;
                            }),
            "Stiller dynamischer Vorgang erzeugt keinen zeitlich korrekten automatischen Heartbeat.");
    }
    auto terminal_cached = automatic_reporter.begin(katana::ProgressOperation::ArtifactWrite,
                                                    katana::ProgressUnit::Files,
                                                    1u,
                                                    "terminal-cache");
    terminal_cached.cached();
    const auto automatic_terminal_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!automatic_terminal_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < automatic_terminal_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(automatic_terminal_seen.load(std::memory_order_acquire),
            "Terminaler Cache-Scope liefert seinen Abschluss nicht aus.");
    std::size_t terminal_event_count = 0u;
    {
        const std::scoped_lock lock(automatic_events_mutex);
        terminal_event_count = automatic_events.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        const std::scoped_lock lock(automatic_events_mutex);
        require(automatic_events.size() == terminal_event_count,
                "Terminaler Cache-Scope erzeugt spaete Heartbeats.");
    }
    require(automatic_reporter.seal_and_flush(),
            "Automatische Heartbeats lassen keinen vollstaendigen Seal zu.");

    std::vector<katana::ProgressEvent> reentrant_events;
    std::atomic_bool reentrant_armed = false;
    std::atomic_bool reentrant_started = false;
    std::atomic_bool reentrant_completed = false;
    std::atomic_bool reentrant_update_completed = false;
    std::atomic_bool reentrant_terminal_seen = false;
    std::atomic<katana::ProgressScope*> reentrant_scope_pointer = nullptr;
    const katana::ProgressReporter reentrant_reporter(
        [&](const katana::ProgressEvent& event) {
            reentrant_events.push_back(event);
            if (event.state == katana::ProgressState::Completed)
                reentrant_terminal_seen.store(true, std::memory_order_release);
            auto* const reentrant_scope = reentrant_scope_pointer.load(std::memory_order_acquire);
            if (event.state != katana::ProgressState::Heartbeat || reentrant_scope == nullptr ||
                reentrant_armed.exchange(true, std::memory_order_acq_rel))
                return;
            reentrant_started.store(true, std::memory_order_release);
            // Let the worker enter update(). The old lock order held the
            // scope mutex while waiting for this callback mutex, after which
            // completed() below formed a deterministic ABBA cycle.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            static_cast<void>(reentrant_scope->completed());
            reentrant_completed.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(10));
    auto reentrant_scope =
        reentrant_reporter.begin(katana::ProgressOperation::FunctionValueAnalysis,
                                 katana::ProgressUnit::Functions,
                                 1u,
                                 "reentrant-heartbeat");
    reentrant_scope_pointer.store(&reentrant_scope, std::memory_order_release);
    std::thread reentrant_worker([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!reentrant_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (reentrant_started.load(std::memory_order_acquire)) reentrant_scope.update(1u);
        reentrant_update_completed.store(true, std::memory_order_release);
    });
    const auto reentrant_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!reentrant_completed.load(std::memory_order_acquire) ||
            !reentrant_update_completed.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < reentrant_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!reentrant_completed.load(std::memory_order_acquire) ||
        !reentrant_update_completed.load(std::memory_order_acquire)) {
        std::cerr << "TEST FEHLGESCHLAGEN: Reentranter Heartbeat deadlockt mit Scope-Update.\n"
                  << std::flush;
        std::_Exit(EXIT_FAILURE);
    }
    reentrant_worker.join();
    reentrant_scope.complete();
    const auto reentrant_terminal_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reentrant_terminal_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < reentrant_terminal_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(reentrant_terminal_seen.load(std::memory_order_acquire),
            "Reentranter Progresspfad liefert seinen Terminalabschluss nicht aus.");
    for (std::size_t index = 0u; index < reentrant_events.size(); ++index)
        require(reentrant_events[index].sequence == index + 1u,
                "Reentranter Progresspfad verliert seine serialisierte Sequenz.");
    require(reentrant_events.back().state == katana::ProgressState::Completed &&
                reentrant_events.back().completed == 1u,
            "Reentranter Progresspfad verliert seinen Terminalabschluss.");

    std::atomic<katana::ProgressScope*> fence_scope_pointer = nullptr;
    std::atomic_bool fence_producer_enabled = false;
    std::atomic_bool fence_producer_started = false;
    std::atomic_bool fence_drainer_returned = false;
    std::atomic_bool fence_flush_returned = false;
    std::atomic_bool fence_flush_result = false;
    std::atomic_bool fence_started_seen = false;
    std::atomic_bool fence_terminal_seen = false;
    std::atomic_uint64_t fence_next_completed = 1u;
    std::atomic_uint64_t fence_running_callbacks = 0u;
    const katana::ProgressReporter fence_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.label != "continuous-fence") return;
            if (event.state == katana::ProgressState::Started)
                fence_started_seen.store(true, std::memory_order_release);
            if (event.state == katana::ProgressState::Completed)
                fence_terminal_seen.store(true, std::memory_order_release);
            if (event.state != katana::ProgressState::Running ||
                !fence_producer_enabled.load(std::memory_order_acquire))
                return;
            fence_running_callbacks.fetch_add(1u, std::memory_order_release);
            fence_producer_started.store(true, std::memory_order_release);
            // Admit a successor before this callback returns. The serialized
            // drainer therefore remains busy for as long as the producer is
            // enabled, making an "entire queue became idle" flush
            // deterministically starve.
            const auto next_completed =
                fence_next_completed.fetch_add(1u, std::memory_order_relaxed) + 1u;
            fence_scope_pointer.load(std::memory_order_acquire)->update(next_completed);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(1000));
    auto fence_scope =
        fence_reporter.begin(katana::ProgressOperation::FunctionValueAnalysis,
                             katana::ProgressUnit::Functions,
                             std::nullopt,
                             "continuous-fence");
    fence_scope_pointer.store(&fence_scope, std::memory_order_release);
    fence_producer_enabled.store(true, std::memory_order_release);
    std::jthread fence_drainer([&] {
        fence_scope.update(1u);
        fence_drainer_returned.store(true, std::memory_order_release);
    });
    const auto fence_producer_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fence_producer_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < fence_producer_deadline)
        std::this_thread::yield();
    require(fence_producer_started.load(std::memory_order_acquire),
            "Kontinuierlicher Progress-Produzent ist nicht angelaufen.");
    std::jthread fence_flusher([&] {
        fence_flush_result.store(fence_reporter.flush(), std::memory_order_release);
        fence_flush_returned.store(true, std::memory_order_release);
    });
    const auto fence_flush_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fence_flush_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < fence_flush_deadline)
        std::this_thread::yield();
    if (!fence_flush_returned.load(std::memory_order_acquire)) {
        std::cerr << "TEST FEHLGESCHLAGEN: Progress-Flush verhungert an nach seinem Fence "
                     "zugelassenen Callbacks.\n"
                  << std::flush;
        std::_Exit(EXIT_FAILURE);
    }
    const auto callbacks_at_flush =
        fence_running_callbacks.load(std::memory_order_acquire);
    const auto post_fence_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fence_running_callbacks.load(std::memory_order_acquire) <=
               callbacks_at_flush &&
           std::chrono::steady_clock::now() < post_fence_deadline)
        std::this_thread::yield();
    require(fence_flush_result.load(std::memory_order_acquire) &&
                fence_drainer_returned.load(std::memory_order_acquire) &&
                callbacks_at_flush != 0u &&
                fence_running_callbacks.load(std::memory_order_acquire) >
                    callbacks_at_flush,
            "Progress-Flush besitzt keinen festen Admission-Fence bei "
            "kontinuierlichem Producer.");
    fence_producer_enabled.store(false, std::memory_order_release);
    fence_flusher.join();
    fence_drainer.join();
    fence_scope.complete();
    require(fence_reporter.seal_and_flush() &&
                fence_started_seen.load(std::memory_order_acquire) &&
                fence_terminal_seen.load(std::memory_order_acquire),
            "Seal schwaecht Started-/Terminal-Lifecycle-Auslieferung.");

    std::atomic_uint64_t continuous_deliveries = 0u;
    const katana::ProgressReporter continuous_seal_reporter(
        [&](const katana::ProgressEvent&) {
            continuous_deliveries.fetch_add(1u, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(1000));
    auto continuous_seal_scope = continuous_seal_reporter.begin(
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressUnit::Functions,
        std::nullopt,
        "continuous-seal");
    std::atomic_bool continuous_seal_stop = false;
    std::atomic_bool continuous_seal_started = false;
    std::jthread continuous_seal_producer([&] {
        std::uint64_t completed = 0u;
        while (!continuous_seal_stop.load(std::memory_order_acquire)) {
            continuous_seal_started.store(true, std::memory_order_release);
            continuous_seal_scope.update(++completed);
        }
    });
    const auto continuous_start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!continuous_seal_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < continuous_start_deadline)
        std::this_thread::yield();
    require(continuous_seal_started.load(std::memory_order_acquire),
            "Kontinuierlicher Seal-Producer startete nicht.");
    const auto continuous_seal_result =
        continuous_seal_reporter.seal_and_flush();
    const auto deliveries_at_seal =
        continuous_deliveries.load(std::memory_order_acquire);
    continuous_seal_stop.store(true, std::memory_order_release);
    continuous_seal_producer.join();
    continuous_seal_scope.update(
        continuous_seal_scope.completed() + 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(!continuous_seal_result &&
                continuous_deliveries.load(std::memory_order_acquire) ==
                    deliveries_at_seal,
            "Seal akzeptiert neue Admissions oder uebersieht einen aktiven Scope.");

    std::atomic_bool pending_callback_entered = false;
    std::atomic_bool pending_callback_release = false;
    std::vector<katana::ProgressState> pending_states;
    const katana::ProgressReporter pending_reporter(
        [&](const katana::ProgressEvent& event) {
            pending_states.push_back(event.state);
            if (event.state != katana::ProgressState::Started) return;
            pending_callback_entered.store(true, std::memory_order_release);
            while (!pending_callback_release.load(std::memory_order_acquire))
                std::this_thread::yield();
        },
        std::chrono::milliseconds(0));
    auto pending_scope = pending_reporter.begin(
        katana::ProgressOperation::Compilation,
        katana::ProgressUnit::TranslationUnits,
        1u,
        "pending-terminal");
    const auto pending_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pending_callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < pending_entry_deadline)
        std::this_thread::yield();
    require(pending_callback_entered.load(std::memory_order_acquire),
            "Pending-Terminal-Test erreichte den Callback nicht.");
    pending_scope.update(1u);
    pending_scope.complete();
    std::atomic_bool pending_seal_returned = false;
    std::atomic_bool pending_seal_result = false;
    std::jthread pending_sealer([&] {
        pending_seal_result.store(
            pending_reporter.seal_and_flush(),
            std::memory_order_release);
        pending_seal_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    require(!pending_seal_returned.load(std::memory_order_acquire),
            "Seal kehrte vor einem bereits laufenden Callback zurueck.");
    pending_callback_release.store(true, std::memory_order_release);
    pending_sealer.join();
    require(pending_seal_result.load(std::memory_order_acquire) &&
                pending_states ==
                    std::vector<katana::ProgressState>{
                        katana::ProgressState::Started,
                        katana::ProgressState::Running,
                        katana::ProgressState::Completed},
            "Seal verlor oder vertauschte pending Running/Terminal-Callbacks.");

    const katana::ProgressReporter terminal_loss_reporter(
        [](const katana::ProgressEvent& event) {
            if (event.state == katana::ProgressState::Completed)
                throw std::runtime_error("deterministic-terminal-loss");
        },
        std::chrono::milliseconds(0));
    auto terminal_loss_scope = terminal_loss_reporter.begin(
        katana::ProgressOperation::Packaging,
        katana::ProgressUnit::Steps,
        1u,
        "terminal-loss");
    terminal_loss_scope.complete();
    require(!terminal_loss_reporter.seal_and_flush() &&
                !terminal_loss_reporter.flush() &&
                terminal_loss_reporter.dropped_observations() == 1u,
            "Verworfener Terminalcallback wird von Seal oder Flush als Erfolg gemeldet.");

    const katana::ProgressReporter external_loss_reporter(
        [](const katana::ProgressEvent&) {},
        std::chrono::milliseconds(0));
    external_loss_reporter.record_observation_loss(2u);
    require(
        external_loss_reporter.dropped_observations() == 2u &&
            !external_loss_reporter.seal_and_flush(),
        "Ein Adapterverlust wurde nicht sticky in den Reporter-/Seal-Vertrag "
        "uebernommen.");

    std::optional<katana::ProgressReporter> callback_owned_reporter;
    std::optional<katana::ProgressScope> callback_owned_scope;
    std::atomic_bool callback_owner_release_started = false;
    std::atomic_bool callback_owner_release_completed = false;
    callback_owned_reporter.emplace(
        [&](const katana::ProgressEvent& event) {
            if (event.state != katana::ProgressState::Heartbeat ||
                callback_owner_release_started.exchange(true, std::memory_order_acq_rel))
                return;
            callback_owned_scope.reset();
            callback_owned_reporter.reset();
            callback_owner_release_completed.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(10));
    callback_owned_scope.emplace(
        callback_owned_reporter->begin(katana::ProgressOperation::RuntimeStartup,
                                       katana::ProgressUnit::Steps,
                                       std::nullopt,
                                       "callback-owned-lifetime"));
    const auto owner_release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callback_owner_release_completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < owner_release_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(
        callback_owner_release_completed.load(std::memory_order_acquire) &&
            !callback_owned_scope.has_value() && !callback_owned_reporter.has_value(),
        "Letzter Progress-Owner kann aus dem Heartbeat-Callback nicht sicher freigegeben werden.");

    std::optional<katana::ProgressReporter> callback_lock_reporter;
    std::atomic_bool callback_lock_test_armed = false;
    std::atomic_bool callback_lock_worker_completed = false;
    callback_lock_reporter.emplace(
        [&](const katana::ProgressEvent& event) {
            if (event.state != katana::ProgressState::Started ||
                event.label != "callback-outside-locks" ||
                callback_lock_test_armed.exchange(true, std::memory_order_acq_rel))
                return;
            std::thread nested_worker([&] {
                auto nested = callback_lock_reporter->begin(katana::ProgressOperation::Compilation,
                                                            katana::ProgressUnit::TranslationUnits,
                                                            1u,
                                                            "nested-callback-work");
                nested.complete();
                callback_lock_worker_completed.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!callback_lock_worker_completed.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!callback_lock_worker_completed.load(std::memory_order_acquire)) {
                std::cerr << "TEST FEHLGESCHLAGEN: Progress-Callback wird unter einem Core-Lock "
                             "ausgefuehrt.\n"
                          << std::flush;
                std::_Exit(EXIT_FAILURE);
            }
            nested_worker.join();
        },
        std::chrono::milliseconds(0));
    auto callback_lock_scope =
        callback_lock_reporter->begin(katana::ProgressOperation::HostRuntimeBuild,
                                      katana::ProgressUnit::Steps,
                                      1u,
                                      "callback-outside-locks");
    callback_lock_scope.complete();
    const auto callback_lock_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callback_lock_worker_completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < callback_lock_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(callback_lock_worker_completed.load(std::memory_order_acquire),
            "Progress-Callback kann parallele Core-Arbeit nicht lockfrei ausloesen.");

    std::optional<katana::ProgressReporter> callback_seal_reporter;
    std::atomic_bool callback_seal_returned = false;
    std::atomic_bool callback_seal_result = true;
    callback_seal_reporter.emplace(
        [&](const katana::ProgressEvent& event) {
            if (event.state != katana::ProgressState::Started ||
                event.label != "callback-seal")
                return;
            callback_seal_result.store(
                callback_seal_reporter->seal_and_flush(),
                std::memory_order_release);
            callback_seal_returned.store(true, std::memory_order_release);
        },
        std::chrono::milliseconds(0));
    auto callback_seal_scope = callback_seal_reporter->begin(
        katana::ProgressOperation::Packaging,
        katana::ProgressUnit::Steps,
        1u,
        "callback-seal");
    const auto callback_seal_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callback_seal_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < callback_seal_deadline)
        std::this_thread::yield();
    require(callback_seal_returned.load(std::memory_order_acquire) &&
                !callback_seal_result.load(std::memory_order_acquire),
            "Seal aus dem eigenen Callback wurde nicht ohne Mutation abgelehnt.");
    callback_seal_scope.complete();
    require(callback_seal_reporter->seal_and_flush(),
            "Abgelehnter Callback-Seal versiegelt den Reporter trotzdem dauerhaft.");

    constexpr std::size_t revision_race_count = 5'000u;
    std::unordered_set<std::uint64_t> revision_terminal_scopes;
    std::atomic_size_t revision_terminal_count = 0u;
    std::atomic_bool revision_sentinel_seen = false;
    bool stale_revision_delivered = false;
    const katana::ProgressReporter revision_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.label == "revision-drain-sentinel" &&
                event.state == katana::ProgressState::Completed) {
                revision_sentinel_seen.store(true, std::memory_order_release);
                return;
            }
            if (event.label != "revision-race") return;
            const auto terminal = event.state == katana::ProgressState::Completed ||
                                  event.state == katana::ProgressState::Cached ||
                                  event.state == katana::ProgressState::Skipped ||
                                  event.state == katana::ProgressState::Failed;
            if (terminal) {
                revision_terminal_scopes.insert(event.scope_id);
                revision_terminal_count.fetch_add(1u, std::memory_order_release);
            } else if (revision_terminal_scopes.contains(event.scope_id)) {
                stale_revision_delivered = true;
            }
        },
        std::chrono::milliseconds(0));
    std::atomic<katana::ProgressScope*> revision_scope = nullptr;
    std::barrier revision_start(3);
    std::barrier revision_done(3);
    std::jthread revision_updater([&] {
        for (std::size_t iteration = 0u; iteration < revision_race_count; ++iteration) {
            revision_start.arrive_and_wait();
            revision_scope.load(std::memory_order_acquire)->update(1u);
            revision_done.arrive_and_wait();
        }
    });
    std::jthread revision_completer([&] {
        for (std::size_t iteration = 0u; iteration < revision_race_count; ++iteration) {
            revision_start.arrive_and_wait();
            revision_scope.load(std::memory_order_acquire)->complete();
            revision_done.arrive_and_wait();
        }
    });
    for (std::size_t iteration = 0u; iteration < revision_race_count; ++iteration) {
        auto raced = revision_reporter.begin(katana::ProgressOperation::FunctionValueAnalysis,
                                             katana::ProgressUnit::Functions,
                                             1u,
                                             "revision-race");
        revision_scope.store(&raced, std::memory_order_release);
        revision_start.arrive_and_wait();
        revision_done.arrive_and_wait();
        revision_scope.store(nullptr, std::memory_order_release);
    }
    revision_updater.join();
    revision_completer.join();
    auto revision_sentinel = revision_reporter.begin(katana::ProgressOperation::ArtifactWrite,
                                                     katana::ProgressUnit::Steps,
                                                     1u,
                                                     "revision-drain-sentinel");
    revision_sentinel.complete();
    const auto revision_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!revision_sentinel_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < revision_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(
        revision_sentinel_seen.load(std::memory_order_acquire) &&
            revision_terminal_count.load(std::memory_order_acquire) == revision_race_count &&
            revision_terminal_scopes.size() == revision_race_count && !stale_revision_delivered,
        "Scope-eigene Revisionen lassen spaete Ereignisse hinter einem Terminalabschluss durch.");

    constexpr std::size_t saturated_scope_count = 4097u;
    std::atomic_bool saturation_callback_entered = false;
    std::atomic_bool saturation_callback_release = false;
    std::atomic_size_t saturated_started_count = 0u;
    std::atomic_size_t saturated_terminal_count = 0u;
    std::atomic_bool saturation_incomplete_seen = false;
    std::atomic<std::uint64_t> saturation_dropped_observations = 0u;
    const katana::ProgressReporter saturation_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.label == "queue-blocker" && event.state == katana::ProgressState::Started) {
                saturation_callback_entered.store(true, std::memory_order_release);
                while (!saturation_callback_release.load(std::memory_order_acquire))
                    std::this_thread::yield();
            }
            if (event.label == "saturated-terminal") {
                if (event.state == katana::ProgressState::Started)
                    saturated_started_count.fetch_add(
                        1u, std::memory_order_release);
                else if (event.state == katana::ProgressState::Completed)
                    saturated_terminal_count.fetch_add(
                        1u, std::memory_order_release);
                if (event.dropped_observations != 0u &&
                    !katana::progress_event_telemetry_complete(event)) {
                    saturation_dropped_observations.store(event.dropped_observations,
                                                          std::memory_order_release);
                    saturation_incomplete_seen.store(true, std::memory_order_release);
                }
            }
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(1000));
    std::jthread saturation_blocker([&] {
        auto blocker = saturation_reporter.begin(katana::ProgressOperation::HostRuntimeBuild,
                                                 katana::ProgressUnit::Steps,
                                                 1u,
                                                 "queue-blocker");
        blocker.complete();
    });
    const auto saturation_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!saturation_callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < saturation_entry_deadline)
        std::this_thread::yield();
    require(saturation_callback_entered.load(std::memory_order_acquire),
            "Saturationstest konnte den Progress-Callback nicht blockieren.");
    std::atomic_size_t saturated_created_count = 0u;
    std::atomic_bool saturated_producer_completed = false;
    std::jthread saturated_producer([&] {
        for (std::size_t index = 0u;
             index < saturated_scope_count;
             ++index) {
            auto scope = saturation_reporter.begin(
                katana::ProgressOperation::ArtifactWrite,
                katana::ProgressUnit::Files,
                1u,
                "saturated-terminal");
            saturated_created_count.fetch_add(
                1u, std::memory_order_release);
            scope.complete();
        }
        saturated_producer_completed.store(
            true, std::memory_order_release);
    });
    const auto saturation_completion_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!saturated_producer_completed.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <
               saturation_completion_deadline)
        std::this_thread::yield();
    require(
        saturated_created_count.load(
            std::memory_order_acquire) == saturated_scope_count &&
            saturated_producer_completed.load(
                std::memory_order_acquire),
        "Eine volle Progress-Queue blockiert weiterhin Lifecycle-Producer.");
    saturation_callback_release.store(true, std::memory_order_release);
    saturated_producer.join();
    saturation_blocker.join();
    const auto saturated_seal = saturation_reporter.seal_and_flush();
    require(
        !saturated_seal &&
            saturated_started_count.load(std::memory_order_acquire) <
                saturated_scope_count &&
            saturated_terminal_count.load(std::memory_order_acquire) <
                saturated_scope_count,
        "Gesaettigte Progress-Queue meldet verworfene Lifecycle-"
        "Admissions nicht sticky und fail-closed.");

    constexpr std::uint64_t short_lived_scope_count = 100'000u;
    std::uint64_t churn_event_count = 0u;
    std::uint64_t churn_last_sequence = 0u;
    bool churn_sequence_valid = true;
    const katana::ProgressReporter churn_reporter(
        [&](const katana::ProgressEvent& event) {
            ++churn_event_count;
            churn_sequence_valid =
                churn_sequence_valid && event.sequence == churn_last_sequence + 1u;
            churn_last_sequence = event.sequence;
        },
        std::chrono::milliseconds(0));
    for (std::uint64_t index = 0u; index < short_lived_scope_count; ++index) {
        auto short_lived = churn_reporter.begin(katana::ProgressOperation::ArtifactWrite,
                                                katana::ProgressUnit::Files,
                                                1u,
                                                "short-lived");
        short_lived.complete();
        // Reclamation, not host scheduler luck, is the contract under test.
        // Drain below the bounded queue limit so a slow single-core CI worker
        // cannot turn this lifecycle test into an accidental saturation test.
        if ((index + 1u) % 1'024u == 0u)
            require(churn_reporter.flush(),
                    "Kurzlebige Scopes lassen sich nicht verlustfrei drainieren.");
    }
    require(churn_reporter.seal_and_flush(),
            "Kurzlebige Scopes lassen sich nicht vollstaendig versiegeln.");
    // Each scope owns its delivery/revision state. This churn regression keeps
    // the reporter alive while all scopes die, so sanitizers and bounded-memory
    // runs exercise reclamation instead of hiding it in reporter destruction.
    require(churn_sequence_valid && churn_event_count == short_lived_scope_count * 2u,
            "Kurzlebige Scopes verlieren Ereignisse oder behalten alte Revisionszustaende.");

    std::cout << "Typisierte Progress-Core-Regressionen erfolgreich.\n";
    return EXIT_SUCCESS;
}
