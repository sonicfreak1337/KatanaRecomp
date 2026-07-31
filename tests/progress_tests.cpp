#include "katana/progress.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

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
    analysis.update(3861u, first);
    auto heartbeat = first;
    heartbeat.queued_work = 80u;
    heartbeat.started = 1361u;
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
                heartbeat_event->counters.started == 1361u,
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
                progress_state_name(katana::ProgressState::Heartbeat) == "heartbeat",
            "Dynamische Progressmetadaten sind nicht stabil benannt.");

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
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
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
    std::atomic_bool heartbeat_seen = false;
    std::atomic_bool automatic_terminal_seen = false;
    const katana::ProgressReporter automatic_reporter(
        [&](const katana::ProgressEvent& event) {
            automatic_events.push_back(event);
            if (event.state == katana::ProgressState::Heartbeat)
                heartbeat_seen.store(true, std::memory_order_release);
            if (event.label == "terminal-cache" &&
                event.state == katana::ProgressState::Cached)
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
    require(heartbeat_seen.load(std::memory_order_acquire) &&
                std::any_of(automatic_events.begin(),
                            automatic_events.end(),
                            [](const auto& event) {
                                return event.state == katana::ProgressState::Heartbeat &&
                                       !event.total;
                            }),
            "Stiller dynamischer Vorgang erzeugt keinen automatischen Heartbeat.");
    auto terminal_cached = automatic_reporter.begin(katana::ProgressOperation::ArtifactWrite,
                                                    katana::ProgressUnit::Files,
                                                    1u,
                                                    "terminal-cache");
    terminal_cached.cached();
    const auto automatic_terminal_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!automatic_terminal_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < automatic_terminal_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(automatic_terminal_seen.load(std::memory_order_acquire),
            "Terminaler Cache-Scope liefert seinen Abschluss nicht aus.");
    const auto terminal_event_count = automatic_events.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(automatic_events.size() == terminal_event_count,
            "Terminaler Cache-Scope erzeugt spaete Heartbeats.");

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
            auto* const reentrant_scope =
                reentrant_scope_pointer.load(std::memory_order_acquire);
            if (event.state != katana::ProgressState::Heartbeat ||
                reentrant_scope == nullptr ||
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
    auto reentrant_scope = reentrant_reporter.begin(
        katana::ProgressOperation::FunctionValueAnalysis,
        katana::ProgressUnit::Functions,
        1u,
        "reentrant-heartbeat");
    reentrant_scope_pointer.store(
        &reentrant_scope, std::memory_order_release);
    std::thread reentrant_worker([&] {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!reentrant_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (reentrant_started.load(std::memory_order_acquire))
            reentrant_scope.update(1u);
        reentrant_update_completed.store(
            true, std::memory_order_release);
    });
    const auto reentrant_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while ((!reentrant_completed.load(std::memory_order_acquire) ||
            !reentrant_update_completed.load(
                std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < reentrant_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!reentrant_completed.load(std::memory_order_acquire) ||
        !reentrant_update_completed.load(std::memory_order_acquire)) {
        std::cerr
            << "TEST FEHLGESCHLAGEN: Reentranter Heartbeat deadlockt mit Scope-Update.\n"
            << std::flush;
        std::_Exit(EXIT_FAILURE);
    }
    reentrant_worker.join();
    reentrant_scope.complete();
    const auto reentrant_terminal_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!reentrant_terminal_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < reentrant_terminal_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(
        reentrant_terminal_seen.load(std::memory_order_acquire),
        "Reentranter Progresspfad liefert seinen Terminalabschluss nicht aus.");
    for (std::size_t index = 0u;
         index < reentrant_events.size();
         ++index)
        require(
            reentrant_events[index].sequence == index + 1u,
            "Reentranter Progresspfad verliert seine serialisierte Sequenz.");
    require(
        reentrant_events.back().state ==
                katana::ProgressState::Completed &&
            reentrant_events.back().completed == 1u,
        "Reentranter Progresspfad verliert seinen Terminalabschluss.");

    std::optional<katana::ProgressReporter> callback_owned_reporter;
    std::optional<katana::ProgressScope> callback_owned_scope;
    std::atomic_bool callback_owner_release_started = false;
    std::atomic_bool callback_owner_release_completed = false;
    callback_owned_reporter.emplace(
        [&](const katana::ProgressEvent& event) {
            if (event.state != katana::ProgressState::Heartbeat ||
                callback_owner_release_started.exchange(
                    true, std::memory_order_acq_rel))
                return;
            callback_owned_scope.reset();
            callback_owned_reporter.reset();
            callback_owner_release_completed.store(
                true, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(10));
    callback_owned_scope.emplace(callback_owned_reporter->begin(
        katana::ProgressOperation::RuntimeStartup,
        katana::ProgressUnit::Steps,
        std::nullopt,
        "callback-owned-lifetime"));
    const auto owner_release_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!callback_owner_release_completed.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <
               owner_release_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(
        callback_owner_release_completed.load(
            std::memory_order_acquire) &&
            !callback_owned_scope.has_value() &&
            !callback_owned_reporter.has_value(),
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
                auto nested = callback_lock_reporter->begin(
                    katana::ProgressOperation::Compilation,
                    katana::ProgressUnit::TranslationUnits,
                    1u,
                    "nested-callback-work");
                nested.complete();
                callback_lock_worker_completed.store(true, std::memory_order_release);
            });
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(2);
            while (!callback_lock_worker_completed.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!callback_lock_worker_completed.load(std::memory_order_acquire)) {
                std::cerr
                    << "TEST FEHLGESCHLAGEN: Progress-Callback wird unter einem Core-Lock "
                       "ausgefuehrt.\n"
                    << std::flush;
                std::_Exit(EXIT_FAILURE);
            }
            nested_worker.join();
        },
        std::chrono::milliseconds(0));
    auto callback_lock_scope = callback_lock_reporter->begin(
        katana::ProgressOperation::HostRuntimeBuild,
        katana::ProgressUnit::Steps,
        1u,
        "callback-outside-locks");
    callback_lock_scope.complete();
    require(callback_lock_worker_completed.load(std::memory_order_acquire),
            "Progress-Callback kann parallele Core-Arbeit nicht lockfrei ausloesen.");

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
            const auto terminal =
                event.state == katana::ProgressState::Completed ||
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
        for (std::size_t iteration = 0u;
             iteration < revision_race_count;
             ++iteration) {
            revision_start.arrive_and_wait();
            revision_scope.load(std::memory_order_acquire)->update(1u);
            revision_done.arrive_and_wait();
        }
    });
    std::jthread revision_completer([&] {
        for (std::size_t iteration = 0u;
             iteration < revision_race_count;
             ++iteration) {
            revision_start.arrive_and_wait();
            revision_scope.load(std::memory_order_acquire)->complete();
            revision_done.arrive_and_wait();
        }
    });
    for (std::size_t iteration = 0u;
         iteration < revision_race_count;
         ++iteration) {
        auto raced = revision_reporter.begin(
            katana::ProgressOperation::FunctionValueAnalysis,
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
    auto revision_sentinel = revision_reporter.begin(
        katana::ProgressOperation::ArtifactWrite,
        katana::ProgressUnit::Steps,
        1u,
        "revision-drain-sentinel");
    revision_sentinel.complete();
    const auto revision_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!revision_sentinel_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < revision_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(
        revision_sentinel_seen.load(std::memory_order_acquire) &&
            revision_terminal_count.load(std::memory_order_acquire) ==
                revision_race_count &&
            revision_terminal_scopes.size() == revision_race_count &&
            !stale_revision_delivered,
        "Scope-eigene Revisionen lassen spaete Ereignisse hinter einem Terminalabschluss durch.");

    constexpr std::size_t saturated_scope_count = 4097u;
    std::atomic_bool saturation_callback_entered = false;
    std::atomic_bool saturation_callback_release = false;
    std::atomic_size_t saturated_terminal_count = 0u;
    const katana::ProgressReporter saturation_reporter(
        [&](const katana::ProgressEvent& event) {
            if (event.label == "queue-blocker" &&
                event.state == katana::ProgressState::Started) {
                saturation_callback_entered.store(
                    true, std::memory_order_release);
                while (!saturation_callback_release.load(
                    std::memory_order_acquire))
                    std::this_thread::yield();
            }
            if (event.label == "saturated-terminal" &&
                event.state == katana::ProgressState::Completed)
                saturated_terminal_count.fetch_add(
                    1u, std::memory_order_release);
        },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(1000));
    std::jthread saturation_blocker([&] {
        auto blocker = saturation_reporter.begin(
            katana::ProgressOperation::HostRuntimeBuild,
            katana::ProgressUnit::Steps,
            1u,
            "queue-blocker");
        blocker.complete();
    });
    const auto saturation_entry_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!saturation_callback_entered.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <
               saturation_entry_deadline)
        std::this_thread::yield();
    require(
        saturation_callback_entered.load(
            std::memory_order_acquire),
        "Saturationstest konnte den Progress-Callback nicht blockieren.");
    std::vector<katana::ProgressScope> saturated_scopes;
    saturated_scopes.reserve(saturated_scope_count);
    for (std::size_t index = 0u;
         index < saturated_scope_count;
         ++index)
        saturated_scopes.push_back(saturation_reporter.begin(
            katana::ProgressOperation::ArtifactWrite,
            katana::ProgressUnit::Files,
            1u,
            "saturated-terminal"));
    for (std::size_t index = 0u;
         index + 1u < saturated_scopes.size();
         ++index)
        saturated_scopes[index].complete();
    std::atomic_bool saturated_last_completed = false;
    std::jthread saturated_last_completer([&] {
        saturated_scopes.back().complete();
        saturated_last_completed.store(
            true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    require(
        !saturated_last_completed.load(
            std::memory_order_acquire),
        "Terminal-Backpressure wurde trotz vollständig gesättigter "
        "Terminalqueue nicht erreicht.");
    saturation_callback_release.store(
        true, std::memory_order_release);
    saturated_last_completer.join();
    saturation_blocker.join();
    require(
        saturated_terminal_count.load(
            std::memory_order_acquire) ==
            saturated_scope_count,
        "Gesättigte Progress-Queue verliert Terminalabschlüsse hinter "
        "niedriger priorisierten Ereignissen.");

    constexpr std::uint64_t short_lived_scope_count = 100'000u;
    std::uint64_t churn_event_count = 0u;
    std::uint64_t churn_last_sequence = 0u;
    bool churn_sequence_valid = true;
    const katana::ProgressReporter churn_reporter(
        [&](const katana::ProgressEvent& event) {
            ++churn_event_count;
            churn_sequence_valid =
                churn_sequence_valid &&
                event.sequence == churn_last_sequence + 1u;
            churn_last_sequence = event.sequence;
        },
        std::chrono::milliseconds(0));
    for (std::uint64_t index = 0u;
         index < short_lived_scope_count;
         ++index) {
        auto short_lived = churn_reporter.begin(
            katana::ProgressOperation::ArtifactWrite,
            katana::ProgressUnit::Files,
            1u,
            "short-lived");
        short_lived.complete();
    }
    // Each scope owns its delivery/revision state. This churn regression keeps
    // the reporter alive while all scopes die, so sanitizers and bounded-memory
    // runs exercise reclamation instead of hiding it in reporter destruction.
    require(churn_sequence_valid &&
                churn_event_count == short_lived_scope_count * 2u,
            "Kurzlebige Scopes verlieren Ereignisse oder behalten alte Revisionszustaende.");

    std::cout << "Typisierte Progress-Core-Regressionen erfolgreich.\n";
    return EXIT_SUCCESS;
}
