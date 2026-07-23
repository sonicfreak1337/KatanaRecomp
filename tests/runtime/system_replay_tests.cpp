#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/scheduler_safepoint.hpp"
#include "katana/runtime/system_replay.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
std::string require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception& error) {
        return error.what();
    }
    require(false, message);
    return {};
}

class GroupedNumberPunctuation final : public std::numpunct<char> {
  protected:
    [[nodiscard]] char do_thousands_sep() const override {
        return '\'';
    }
    [[nodiscard]] std::string do_grouping() const override {
        return "\1";
    }
};

class GlobalLocaleGuard final {
  public:
    explicit GlobalLocaleGuard(const std::locale& replacement)
        : previous_(std::locale::global(replacement)) {}
    ~GlobalLocaleGuard() {
        std::locale::global(previous_);
    }

    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

  private:
    std::locale previous_;
};

katana::runtime::SystemReplayEvent event(const katana::runtime::SystemReplayEventKind kind,
                                         const std::uint64_t cycle,
                                         std::string code,
                                         const std::uint64_t epoch = 0u) {
    return {0u,
            cycle,
            kind,
            std::move(code),
            0xA0000000u,
            static_cast<std::uint32_t>(cycle),
            cycle * 2u,
            cycle * 3u,
            false,
            epoch};
}

katana::runtime::SystemReplayLog frame_log(const std::uint64_t guest_hash) {
    using namespace katana::runtime;
    SystemReplayLog log;
    log.record(event(SystemReplayEventKind::CpuSafepoint, 1u, "cpu-block"));
    log.record(event(SystemReplayEventKind::MmioRead, 2u, "mmio-read"));
    log.record(event(SystemReplayEventKind::MmioWrite, 3u, "mmio-write"));
    log.record(event(SystemReplayEventKind::Dma, 4u, "dma-complete"));
    log.record(event(SystemReplayEventKind::Interrupt, 5u, "interrupt-accepted"));
    log.record(event(SystemReplayEventKind::Timer, 6u, "timer-underflow"));
    log.record(event(SystemReplayEventKind::SchedulerCallback, 7u, "scheduler-callback"));
    log.record(event(SystemReplayEventKind::Video, 8u, "video-tick"));
    log.record(event(SystemReplayEventKind::Audio, 9u, "audio-tick"));
    log.inject(event(SystemReplayEventKind::ExternalInput, 10u, "controller-input"));
    log.inject(event(SystemReplayEventKind::HostEvent, 11u, "host-signal"));
    log.record(event(SystemReplayEventKind::CpuSafepoint, 0u, "reset-entry", 1u));
    log.seal(guest_hash);
    return log;
}

void replay_all(katana::runtime::DeterministicSystemReplay& replay,
                const katana::runtime::SystemReplayLog& log) {
    for (const auto& current : log.events())
        replay.observe(current);
}

} // namespace

int main() {
    using namespace katana::runtime;

    CpuState cpu;
    cpu.pc = 0x8C010000u;
    cpu.r[0] = 0x12345678u;
    const auto guest_hash = hash_replay_guest_state(cpu, 100u, 0x55AAu);
    const auto log = frame_log(guest_hash);
    const auto duplicate = frame_log(guest_hash);
    require(log.event_hash() == duplicate.event_hash() &&
                log.serialize_json() == duplicate.serialize_json() && log.events().size() == 12u,
            "Gleiche synthetische Framesequenz erzeugt keinen bytegleichen Ereignisvertrag.");
    require(log.coverage_complete() && log.required_coverage() == 0u &&
                log.enabled_coverage() == 0u && log.observed_coverage() != 0u,
            "Allgemeiner Replayvertrag trennt aktivierte und beobachtete Coverage nicht.");
    const auto json = log.serialize_json();
    for (const auto marker : {"cpu-safepoint",
                              "mmio-read",
                              "mmio-write",
                              "dma",
                              "interrupt",
                              "timer",
                              "scheduler-callback",
                              "video",
                              "audio",
                              "external-input",
                              "host-event",
                              "\"time_epoch\":1",
                              "\"injected\":true",
                              "\"event_hash\":null",
                              "\"profile\":\"general\"",
                              "\"enabled_coverage\":0",
                              "\"coverage_complete\":true",
                              "\"coverage_event_counts\":[",
                              "\"final_guest_state_hash\":null",
                              "\"detail\":null",
                              "\"auxiliary\":null"}) {
        require(json.find(marker) != std::string::npos,
                std::string("Systemreplay verliert Ereignis- oder Hashfeld: ") + marker);
    }

    DeterministicSystemReplay replay(log);
    replay_all(replay, log);
    replay.finish(guest_hash);
    require(replay.complete() && replay.position() == log.events().size(),
            "Vollstaendige synthetische Framesequenz wird nicht reproduziert.");

    DeterministicSystemReplay content_mismatch(log);
    auto changed = log.events().front();
    changed.code = "cpu-changed";
    const auto content_error = require_failure<SystemReplayMismatch>(
        [&] { content_mismatch.observe(changed); }, "Geaenderter Ereignisinhalt wurde akzeptiert.");
    require(content_error.find("Ereignis 0") != std::string::npos,
            "Inhaltsabweichung nennt nicht die erste Position.");

    DeterministicSystemReplay order_mismatch(log);
    const auto order_error =
        require_failure<SystemReplayMismatch>([&] { order_mismatch.observe(log.events()[1]); },
                                              "Vertauschte Ereignisreihenfolge wurde akzeptiert.");
    require(order_error.find("Ereignis 0") != std::string::npos,
            "Reihenfolgeabweichung nennt nicht die erste Position.");

    DeterministicSystemReplay missing(log);
    missing.observe(log.events().front());
    const auto missing_error = require_failure<SystemReplayMismatch>(
        [&] { missing.finish(guest_hash); }, "Fehlende Ereignisse wurden akzeptiert.");
    require(missing_error.find("Ereignis 1") != std::string::npos,
            "Fehlendes Ereignis nennt nicht die erste Luecke.");

    DeterministicSystemReplay extra(log);
    replay_all(extra, log);
    auto additional = event(SystemReplayEventKind::Timer, 12u, "extra-timer", 1u);
    const auto extra_error = require_failure<SystemReplayMismatch>(
        [&] { extra.observe(additional); }, "Zusaetzliches Ereignis wurde akzeptiert.");
    require(extra_error.find("Ereignis 12") != std::string::npos,
            "Zusaetzliches Ereignis nennt nicht die erste Position hinter der Spur.");

    DeterministicSystemReplay state_mismatch(log);
    replay_all(state_mismatch, log);
    require_failure<SystemReplayMismatch>([&] { state_mismatch.finish(guest_hash ^ 1u); },
                                          "Abweichender Gastzustandshash wurde akzeptiert.");
    auto changed_cpu = cpu;
    changed_cpu.r[0] ^= 1u;
    require(hash_replay_guest_state(changed_cpu, 100u, 0x55AAu) != guest_hash,
            "Gastzustandshash erkennt Registeraenderung nicht.");

    SystemReplayLog dropped;
    require(!dropped.try_record(event(SystemReplayEventKind::ExternalInput, 1u, "not-injected")) &&
                dropped.dropped_events() == 1u,
            "Fehlende explizite externe Injektion wird nicht als Drop sichtbar.");
    require_failure<std::runtime_error>([&] { dropped.seal(1u); },
                                        "Unvollstaendige Aufzeichnung wurde versiegelt.");
    const auto dropped_replay_error = require_failure<std::invalid_argument>(
        [&] { static_cast<void>(DeterministicSystemReplay(dropped)); },
        "Unvollstaendige Aufzeichnung wurde fuer Replay kopiert.");
    require(dropped_replay_error.find("Unvollstaendige") != std::string::npos,
            "Replay prueft den Drop nicht vor Versiegelung, Kopie und Hash.");
    SystemReplayLog unsealed;
    unsealed.record(event(SystemReplayEventKind::Timer, 1u, "unsealed"));
    const auto unsealed_replay_error = require_failure<std::invalid_argument>(
        [&] { static_cast<void>(DeterministicSystemReplay(unsealed)); },
        "Unversiegelte Aufzeichnung wurde fuer Replay kopiert.");
    require(unsealed_replay_error.find("Unversiegelte") != std::string::npos,
            "Replay prueft den Seal-Zustand nicht vor Kopie und Hash.");

    require_failure<std::invalid_argument>(
        [] { static_cast<void>(SystemReplayLog({0u, false})); },
        "Systemreplay akzeptiert Kapazitaet null.");
    require_failure<std::invalid_argument>(
        [] {
            static_cast<void>(
                SystemReplayLog({SystemReplayConfig::maximum_capacity + 1u, false}));
        },
        "Systemreplay akzeptiert eine nicht portable Kapazitaet.");
    SystemReplayLog overlong_code;
    require(!overlong_code.try_record(
                event(SystemReplayEventKind::Timer,
                      1u,
                      std::string(SystemReplayConfig::maximum_code_length + 1u, 'a'))) &&
                overlong_code.events().empty() && overlong_code.dropped_events() == 1u,
            "Systemreplay akzeptiert einen ueberlangen portablen Ereigniscode.");

    SystemReplayLog normalized_code({1u, false});
    auto oversized_code =
        event(SystemReplayEventKind::Timer, 1u, "short-code-with-large-reserve");
    oversized_code.code.reserve(1024u * 1024u);
    const auto oversized_capacity = oversized_code.code.capacity();
    normalized_code.record(std::move(oversized_code));
    require(oversized_capacity > SystemReplayConfig::maximum_code_length &&
                normalized_code.events()[0].code == "short-code-with-large-reserve" &&
                normalized_code.events()[0].code.capacity() <=
                    SystemReplayConfig::maximum_code_length,
            "Systemreplay behaelt eine uebergrosse Aufruferkapazitaet im Ereigniscode.");

    SystemReplayLog bounded({2u, false});
    auto bounded_first = event(SystemReplayEventKind::MmioWrite, 1u, "private-title-id");
    bounded_first.address = 0xDEADC0DEu;
    bounded_first.value = 0xDEADBEEFu;
    bounded.record(bounded_first);
    bounded.record(event(SystemReplayEventKind::Timer, 2u, "bounded-second"));
    require(!bounded.try_record(event(SystemReplayEventKind::Video, 3u, "bounded-third")) &&
                bounded.events().size() == 2u && bounded.dropped_events() == 1u &&
                !bounded.exact_event_stream_available() &&
                bounded.config().capacity == 2u && !bounded.config().serialize_values,
            "Systemreplay ueberschreitet seine Kapazitaet oder verliert Drop-/Konfigevidenz.");
    const auto bounded_json = bounded.serialize_json();
    require(bounded_json.find("\"capacity\":2") != std::string::npos &&
                bounded_json.find("\"serialize_values\":false") != std::string::npos &&
                bounded_json.find("\"values_redacted\":true") != std::string::npos &&
                bounded_json.find("\"codes_redacted\":true") != std::string::npos &&
                bounded_json.find("\"addresses_redacted\":true") != std::string::npos &&
                bounded_json.find("\"numeric_payloads_redacted\":true") != std::string::npos &&
                bounded_json.find("\"hashes_redacted\":true") != std::string::npos &&
                bounded_json.find("\"event_hash\":null") != std::string::npos &&
                bounded_json.find("\"code\":null") != std::string::npos &&
                bounded_json.find("\"address\":null") != std::string::npos &&
                bounded_json.find("\"detail\":null") != std::string::npos &&
                bounded_json.find("\"auxiliary\":null") != std::string::npos &&
                bounded_json.find("private-title-id") == std::string::npos &&
                bounded_json.find("DEADC0DE") == std::string::npos &&
                bounded_json.find("DEADBEEF") == std::string::npos,
            "Begrenzter Systemreplay weist Kapazitaet/Redaktion nicht aus oder gibt Werte preis.");
    require_failure<std::runtime_error>([&] { bounded.seal(1u); },
                                        "Systemreplay mit Kapazitaetsdrop wurde versiegelt.");

    SystemReplayLog bounded_duplicate({2u, false});
    bounded_duplicate.record(bounded_first);
    bounded_duplicate.record(event(SystemReplayEventKind::Timer, 2u, "bounded-second"));
    bounded_duplicate.note_dropped_event();
    require(bounded.serialize_json() == bounded_duplicate.serialize_json() &&
                bounded.event_hash() == bounded_duplicate.event_hash(),
            "Gleiche begrenzte Systemreplays erzeugen keine bytegleiche Evidenz.");

    SystemReplayLog direct_overflow({1u, false});
    direct_overflow.record(event(SystemReplayEventKind::Timer, 1u, "direct-first"));
    require_failure<std::length_error>(
        [&] {
            direct_overflow.record(
                event(SystemReplayEventKind::Timer, 2u, "direct-overflow"));
        },
        "Direkter Systemreplay-Overflow wurde nicht sichtbar gestoppt.");
    require(direct_overflow.events().size() == 1u &&
                direct_overflow.dropped_events() == 1u,
            "Direkter Systemreplay-Overflow markiert den Drop nicht genau einmal.");
    require_failure<std::runtime_error>([&] { direct_overflow.seal(1u); },
                                        "Direkter Kapazitaetsdrop blieb versiegelbar.");

    SystemReplayLog value_opt_in({2u, true});
    value_opt_in.record(bounded_first);
    value_opt_in.seal(0x1234u);
    const auto opt_in_json = value_opt_in.serialize_json();
    require(opt_in_json.find("\"serialize_values\":true") != std::string::npos &&
                opt_in_json.find("\"values_redacted\":false") != std::string::npos &&
                opt_in_json.find("\"codes_redacted\":false") != std::string::npos &&
                opt_in_json.find("\"addresses_redacted\":false") != std::string::npos &&
                opt_in_json.find("\"numeric_payloads_redacted\":false") !=
                    std::string::npos &&
                opt_in_json.find("\"hashes_redacted\":false") != std::string::npos &&
                opt_in_json.find("\"event_hash\":\"0x") != std::string::npos &&
                opt_in_json.find("\"final_guest_state_hash\":\"0x0000000000001234\"") !=
                    std::string::npos &&
                opt_in_json.find("\"code\":\"private-title-id\"") != std::string::npos &&
                opt_in_json.find("\"address\":\"0xDEADC0DE\"") != std::string::npos &&
                opt_in_json.find("\"value\":\"0xDEADBEEF\"") != std::string::npos &&
                opt_in_json.find("\"detail\":2") != std::string::npos &&
                opt_in_json.find("\"auxiliary\":3") != std::string::npos,
            "Explizites lokales Replay-Wert-Opt-in wird nicht ausgewiesen oder angewendet.");
    const auto original_global_locale = std::locale();
    std::string locale_independent_json;
    {
        const GlobalLocaleGuard locale_guard(
            std::locale(std::locale::classic(), new GroupedNumberPunctuation));
        locale_independent_json = value_opt_in.serialize_json();
    }
    require(locale_independent_json == opt_in_json &&
                std::locale() == original_global_locale,
            "Systemreplay-JSON haengt von der globalen Locale ab oder stellt sie nicht wieder her.");

    SystemReplayLog sealed_drop_guard;
    sealed_drop_guard.record(event(SystemReplayEventKind::Timer, 1u, "sealed"));
    sealed_drop_guard.seal(0x5678u);
    const auto sealed_before_drop = sealed_drop_guard.serialize_json();
    sealed_drop_guard.note_dropped_event();
    require(sealed_drop_guard.dropped_events() == 0u &&
                sealed_drop_guard.serialize_json() == sealed_before_drop,
            "Dropmeldung mutiert einen bereits versiegelten Systemreplay.");

    SystemReplayLog mmio;
    auto observer = system_replay_mmio_observer(mmio, [] { return 20u; }, "pvr-register");
    observer(
        {MemoryAccessOperation::Write, 0xA05F8000u, MemoryAccessWidth::Word, 0x1234u, "pvr"});
    require(mmio.events().size() == 1u &&
                mmio.events()[0].kind == SystemReplayEventKind::MmioWrite &&
                mmio.events()[0].guest_cycle == 20u &&
                mmio.observed_coverage() ==
                    static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Mmio),
            "MMIO-Beobachter erzeugt kein geordnetes Systemereignis.");

    SystemReplayLog reset_mmio;
    EventScheduler reset_scheduler;
    auto reset_observer = system_replay_mmio_observer(
        reset_mmio,
        [&] { return reset_scheduler.current_cycle(); },
        "reset-mmio",
        [&] { return reset_scheduler.reset_generation(); });
    static_cast<void>(reset_scheduler.advance_to(100u, 1u));
    reset_observer(
        {MemoryAccessOperation::Write, 0xA05F8000u, MemoryAccessWidth::Word, 1u, "pvr"});
    reset_scheduler.reset();
    static_cast<void>(reset_scheduler.advance_to(5u, 1u));
    reset_observer(
        {MemoryAccessOperation::Write, 0xA05F8004u, MemoryAccessWidth::Word, 2u, "pvr"});
    reset_mmio.seal(0x1234u);
    const auto reset_copy = reset_mmio.serialize_json();
    require(reset_mmio.events().size() == 2u && reset_mmio.events()[1].time_epoch == 1u &&
                reset_mmio.dropped_events() == 0u && reset_copy == reset_mmio.serialize_json(),
            "MMIO-Replay verliert nach Scheduler-Reset Epoche oder Bytegleichheit.");

    const SafepointReport safepoint{
        SafepointKind::AfterDelaySlot,
        ExecutionOrigin::Fallback,
        30u,
        29u,
        1u,
        2u,
        true,
        true,
        true};
    const auto safepoint_event = make_safepoint_replay_event(safepoint);
    require(safepoint_event.kind == SystemReplayEventKind::CpuSafepoint &&
                safepoint_event.code == "after-delay-slot-fallback" &&
                safepoint_event.guest_cycle == 29u && safepoint_event.value == 7u,
            "Safepoint-Replay verliert Origin, Delay-Slot-Grenze oder Budget-/Interruptflags.");

    const auto deterministic_required =
        system_replay_required_coverage(SystemReplayProfile::DeterministicV1);
    SystemReplayLog enabled_without_events(
        {16u, false, SystemReplayProfile::DeterministicV1});
    require(!enabled_without_events.coverage_complete() &&
                enabled_without_events.observed_coverage() == 0u,
            "Deterministic-v1 behauptet Coverage ohne aktivierte Hooks.");
    for (const auto hook : {SystemReplayCoverage::CpuSafepoint,
                            SystemReplayCoverage::SchedulerCallback,
                            SystemReplayCoverage::AcceptedInterrupt,
                            SystemReplayCoverage::Video,
                            SystemReplayCoverage::Audio,
                             SystemReplayCoverage::Input,
                             SystemReplayCoverage::Mmio,
                             SystemReplayCoverage::Dma,
                             SystemReplayCoverage::BlockDispatch,
                             SystemReplayCoverage::GuestException,
                             SystemReplayCoverage::ControlledFallback,
                             SystemReplayCoverage::GuestCheckpoint}) {
        enabled_without_events.enable_coverage(
            static_cast<SystemReplayCoverageMask>(hook));
    }
    require(enabled_without_events.coverage_complete() &&
                enabled_without_events.enabled_coverage() == deterministic_required &&
                enabled_without_events.observed_coverage() == 0u &&
                std::all_of(enabled_without_events.event_counts().begin(),
                            enabled_without_events.event_counts().end(),
                            [](const auto count) { return count == 0u; }),
            "Aktivierte Hooks werden faelschlich als aufgetretene Ereignisse gewertet.");
    const auto empty_digest = enabled_without_events.ordering_digest();
    enabled_without_events.seal(guest_hash);
    require(empty_digest == enabled_without_events.ordering_digest(),
            "Seal mutiert den kanonischen Reihenfolge-Digest eines leeren Laufs.");

    SystemReplayLog incomplete_coverage(
        {16u, false, SystemReplayProfile::DeterministicV1});
    const auto dma_coverage =
        static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::Dma);
    incomplete_coverage.enable_coverage(deterministic_required & ~dma_coverage);
    require(!incomplete_coverage.coverage_complete() &&
                (incomplete_coverage.required_coverage() &
                 ~incomplete_coverage.enabled_coverage()) == dma_coverage,
            "Fehlender DMA-Hook bleibt im Pflichtprofil nicht sichtbar.");

    const auto make_order_log = [&](const bool reverse) {
        SystemReplayLog order({8u, false, SystemReplayProfile::DeterministicV1});
        order.enable_coverage(deterministic_required);
        const auto cpu_event = event(SystemReplayEventKind::CpuSafepoint, 1u, "cpu-order");
        const auto scheduler_event =
            event(SystemReplayEventKind::SchedulerCallback, 1u, "scheduler-order");
        reverse ? order.record(scheduler_event) : order.record(cpu_event);
        reverse ? order.record(cpu_event) : order.record(scheduler_event);
        order.seal(guest_hash);
        return order;
    };
    const auto forward_order = make_order_log(false);
    const auto reverse_order = make_order_log(true);
    require(forward_order.coverage_complete() && reverse_order.coverage_complete() &&
                forward_order.final_guest_state_hash() ==
                    reverse_order.final_guest_state_hash() &&
                forward_order.observed_coverage() == reverse_order.observed_coverage() &&
                forward_order.ordering_digest() != reverse_order.ordering_digest(),
            "Vertauschte Ereignisreihenfolge mit gleichem Endzustand aendert den Digest nicht.");

    SystemReplayLog undeclared_hook(
        {8u, false, SystemReplayProfile::DeterministicV1});
    require(!undeclared_hook.try_record(
                event(SystemReplayEventKind::CpuSafepoint, 1u, "undeclared-hook")) &&
                undeclared_hook.dropped_events() == 1u,
            "Deterministic-v1 akzeptiert Ereignisse aus nicht aktivierten Hooks.");
    require_failure<std::invalid_argument>(
        [] {
            static_cast<void>(SystemReplayLog(
                {8u, false, static_cast<SystemReplayProfile>(0xFFu)}));
        },
        "Unbekanntes Systemreplay-Profil wurde akzeptiert.");
    require_failure<std::invalid_argument>(
        [] {
            static_cast<void>(SystemReplayLog(
                {8u,
                 false,
                 SystemReplayProfile::General,
                 static_cast<SystemReplayStorageMode>(0xFFu)}));
        },
        "Unbekannter Systemreplay-Speichermodus wurde akzeptiert.");
    SystemReplayLog unknown_event_kind;
    auto unknown_event =
        event(static_cast<SystemReplayEventKind>(0xFFu), 1u, "unknown-event-kind");
    require(!unknown_event_kind.try_record(std::move(unknown_event)) &&
                unknown_event_kind.dropped_events() == 1u,
            "Unbekannte Systemreplay-Ereignisklasse wurde akzeptiert.");

    const auto observation_coverage =
        static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::BlockDispatch) |
        static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestException) |
        static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::ControlledFallback) |
        static_cast<SystemReplayCoverageMask>(SystemReplayCoverage::GuestCheckpoint);
    SystemReplayLog observation_log(
        {32u, false, SystemReplayProfile::DeterministicV1});
    observation_log.enable_coverage(deterministic_required & ~observation_coverage);
    EventScheduler observation_scheduler;
    SystemReplayObservationSession observations(&observation_log, &observation_scheduler);
    require(observation_log.coverage_complete(),
            "Zentrale Observation-Session aktiviert ihre vier Pflichtklassen nicht.");
    require(observations.observe_guest_checkpoint(SystemReplayCheckpointKind::RuntimeStarted) &&
                !observations.observe_guest_checkpoint(
                    SystemReplayCheckpointKind::RuntimeStarted),
            "Runtime-Checkpoint wird nicht strikt monoton und dedupliziert erfasst.");
    observations.observe_block_dispatch_hit(RuntimeDispatchClass::GuardedFallback, true);
    IndirectDispatchMetrics observation_metrics;
    observation_metrics.record_miss(RuntimeDispatchClass::RuntimeOnly,
                                    DispatchDiagnosticError::PermissionDenied,
                                    0x8C010000u,
                                    0x8C020000u);
    observations.observe_block_dispatch_miss(observation_metrics);
    observations.observe_controlled_fallback();
    observations.observe_guest_exception(ExceptionCause::TlbMissRead);
    observations.observe_guest_exception(ExceptionCause::None);
    require(observations.observe_guest_checkpoint(
                SystemReplayCheckpointKind::GuestProgramEntered) &&
                !observations.observe_guest_checkpoint(
                    SystemReplayCheckpointKind::RuntimeStarted),
            "Observation-Session akzeptiert einen zuruecklaufenden Checkpoint.");
    require(observation_log.events().size() == 6u &&
                observation_log.events()[1].kind ==
                    SystemReplayEventKind::BlockDispatchHit &&
                observation_log.events()[2].kind ==
                    SystemReplayEventKind::BlockDispatchMiss &&
                observation_log.events()[2].detail ==
                    static_cast<std::uint64_t>(DispatchDiagnosticError::PermissionDenied) &&
                observation_log.events()[2].auxiliary ==
                    static_cast<std::uint64_t>(RuntimeDispatchClass::RuntimeOnly) &&
                observation_log.events()[3].kind ==
                    SystemReplayEventKind::ControlledFallback &&
                observation_log.events()[4].kind ==
                    SystemReplayEventKind::GuestException &&
                observation_log.events()[5].kind ==
                    SystemReplayEventKind::GuestCheckpoint,
            "Observation-Session verliert Dispatch-, Fallback-, Exception- oder Checkpointklasse.");
    require(std::all_of(observation_log.events().begin(),
                        observation_log.events().end(),
                        [](const auto& recorded) {
                            return !recorded.address.has_value() && !recorded.value.has_value();
                        }),
            "Zentrale Observation-Events speichern rohe Gastadressen oder Werte.");
    require(observations.serialize_checkpoint_json() ==
                "{\"schema\":\"katana.runtime-probe-checkpoint\",\"report_version\":1,"
                "\"status\":\"observed\",\"sequence\":2,"
                "\"checkpoint\":\"guest-program-entered\"}",
            "Redigierter Checkpoint-Zeilenvertrag ist nicht exakt oder nicht monoton.");
    SystemReplayLog materialized_sampling_log({8u, false});
    EventScheduler materialized_sampling_scheduler;
    SystemReplayObservationSession materialized_sampling_observations(
        &materialized_sampling_log, &materialized_sampling_scheduler);
    materialized_sampling_observations.observe_block_dispatch_hit(
        RuntimeDispatchClass::GuardedFallback);
    materialized_sampling_observations.observe_block_dispatch_hit(
        RuntimeDispatchClass::GuardedFallback);
    materialized_sampling_observations.observe_block_dispatch_hit(
        RuntimeDispatchClass::RuntimeOnly, true);
    materialized_sampling_observations.observe_block_dispatch_hit(
        RuntimeDispatchClass::GuardedFallback);
    require(materialized_sampling_log.events().size() == 4u &&
                materialized_sampling_log.events()[2].code ==
                    "block-dispatch-hit-materialized" &&
                materialized_sampling_log.events()[2].auxiliary == 3u &&
                materialized_sampling_log.events()[3].code == "block-dispatch-hit" &&
                materialized_sampling_log.events()[3].auxiliary == 4u,
            "Materialisierter Dispatch ausserhalb des Potenz-Samples wird verschluckt oder "
            "veraendert die Dispatch-Countersemantik.");
    const auto redacted_observations = observation_log.serialize_json();
    require(redacted_observations.find("0x8C010000") == std::string::npos &&
                redacted_observations.find("0x8C020000") == std::string::npos &&
                redacted_observations.find("\"detail\":null") != std::string::npos &&
                redacted_observations.find("\"auxiliary\":null") != std::string::npos,
            "Redigierter Observation-Replay gibt Dispatchadressen oder Payloads preis.");

    const auto make_high_volume_observations = [] {
        SystemReplayLog high_volume({64u, false});
        EventScheduler high_volume_scheduler;
        SystemReplayObservationSession high_volume_observations(
            &high_volume, &high_volume_scheduler);
        for (std::uint64_t index = 0u; index < 100'000u; ++index) {
            high_volume_observations.observe_block_dispatch_hit(
                RuntimeDispatchClass::GuardedFallback);
            high_volume_observations.observe_controlled_fallback();
            high_volume_observations.observe_guest_exception(ExceptionCause::Trap);
        }
        high_volume.seal(0u);
        return high_volume;
    };
    const auto high_volume = make_high_volume_observations();
    const auto high_volume_duplicate = make_high_volume_observations();
    require(high_volume.events().size() == 51u &&
                high_volume.dropped_events() == 0u &&
                high_volume.event_hash() == high_volume_duplicate.event_hash() &&
                high_volume.events() == high_volume_duplicate.events(),
            "100.000 Dispatch-/Fallback-/Exceptionbeobachtungen saettigen den begrenzten "
            "Replay oder werden nicht deterministisch potenzbasiert verdichtet.");

    constexpr std::uint64_t long_replay_event_count = 100'000u;
    const auto long_replay_event = [](const std::uint64_t index) {
        return event(SystemReplayEventKind::CpuSafepoint,
                     index,
                     "long-product-safepoint",
                     index * 3u + 1u);
    };
    const auto make_long_digest = [&] {
        SystemReplayLog digest(
            {8u,
             false,
             SystemReplayProfile::DeterministicV1,
             SystemReplayStorageMode::DigestStream});
        digest.enable_coverage(digest.required_coverage());
        for (std::uint64_t index = 0u; index < long_replay_event_count; ++index)
            digest.record(long_replay_event(index));
        digest.seal(0xD16E57u);
        return digest;
    };
    const auto long_digest = make_long_digest();
    const auto long_digest_duplicate = make_long_digest();
    require(long_digest.event_count() == long_replay_event_count &&
                long_digest.events().size() == 8u &&
                long_digest.summarized_event_count() ==
                    long_replay_event_count - 8u &&
                !long_digest.exact_event_stream_available() &&
                long_digest.dropped_events() == 0u && long_digest.sealed() &&
                long_digest.ordering_digest() ==
                    long_digest_duplicate.ordering_digest() &&
                long_digest.event_counts() ==
                    long_digest_duplicate.event_counts(),
            "DigestStream verliert lange Produktreplays, waechst ueber die Retention "
            "hinaus oder ist nicht deterministisch.");
    const auto long_digest_json = long_digest.serialize_json();
    require(long_digest_json.find("\"storage_mode\":\"digest-stream\"") !=
                    std::string::npos &&
                long_digest_json.find("\"event_count\":100000") !=
                    std::string::npos &&
                long_digest_json.find("\"retained_event_count\":8") !=
                    std::string::npos &&
                long_digest_json.find("\"summarized_event_count\":99992") !=
                    std::string::npos &&
                long_digest_json.find("\"exact_event_stream\":false") !=
                    std::string::npos &&
                long_digest_json.find("\"dropped_events\":0") !=
                    std::string::npos,
            "DigestStream weist Gesamtzahl, Retention und Zusammenfassung nicht "
            "explizit aus.");
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(DeterministicSystemReplay(long_digest)); },
        "Exakter Replay akzeptiert faelschlich einen DigestStream.");
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(DeterministicSystemReplayDigest(frame_log(1u))); },
        "Digest-Replay akzeptiert faelschlich eine exakte Aufzeichnung.");

    DeterministicSystemReplayDigest digest_replay(long_digest);
    for (std::uint64_t index = 0u; index < long_replay_event_count; ++index)
        digest_replay.observe(long_replay_event(index));
    digest_replay.finish(0xD16E57u);
    require(digest_replay.complete() &&
                digest_replay.position() == long_replay_event_count,
            "Langer DigestStream wird nicht vollstaendig reproduziert.");

    DeterministicSystemReplayDigest witness_mismatch(long_digest);
    witness_mismatch.observe(long_replay_event(0u));
    auto changed_witness = long_replay_event(1u);
    ++changed_witness.detail;
    require_failure<SystemReplayMismatch>(
        [&] { witness_mismatch.observe(std::move(changed_witness)); },
        "Digest-Witness meldet eine fruehe Inhaltsabweichung nicht sofort.");

    DeterministicSystemReplayDigest summarized_mismatch(long_digest);
    for (std::uint64_t index = 0u; index < long_replay_event_count; ++index) {
        auto observed = long_replay_event(index);
        if (index == 90'000u) ++observed.detail;
        summarized_mismatch.observe(std::move(observed));
    }
    require_failure<SystemReplayMismatch>(
        [&] { summarized_mismatch.finish(0xD16E57u); },
        "DigestStream akzeptiert eine Abweichung ausserhalb des Witness-Praefix.");

    DeterministicSystemReplayDigest missing_digest_event(long_digest);
    for (std::uint64_t index = 0u; index + 1u < long_replay_event_count; ++index)
        missing_digest_event.observe(long_replay_event(index));
    require_failure<SystemReplayMismatch>(
        [&] { missing_digest_event.finish(0xD16E57u); },
        "DigestStream akzeptiert ein fehlendes Ereignis.");

    DeterministicSystemReplayDigest extra_digest_event(long_digest);
    for (std::uint64_t index = 0u; index < long_replay_event_count; ++index)
        extra_digest_event.observe(long_replay_event(index));
    require_failure<SystemReplayMismatch>(
        [&] { extra_digest_event.observe(long_replay_event(long_replay_event_count)); },
        "DigestStream akzeptiert ein zusaetzliches Ereignis.");

    DeterministicSystemReplayDigest wrong_digest_state(long_digest);
    for (std::uint64_t index = 0u; index < long_replay_event_count; ++index)
        wrong_digest_state.observe(long_replay_event(index));
    require_failure<SystemReplayMismatch>(
        [&] { wrong_digest_state.finish(0xBAD57A7Eu); },
        "DigestStream akzeptiert einen falschen Gastendzustand.");

    SystemReplayLog invalid_digest(
        {8u,
         false,
         SystemReplayProfile::General,
         SystemReplayStorageMode::DigestStream});
    require(!invalid_digest.try_record(
                event(static_cast<SystemReplayEventKind>(0xFFu), 1u, "invalid")) &&
                invalid_digest.dropped_events() == 1u &&
                !invalid_digest.exact_event_stream_available(),
            "DigestStream weist einen echten Aufnahmefehler nicht als Drop aus.");
    require_failure<std::runtime_error>(
        [&] { invalid_digest.seal(0u); },
        "DigestStream mit echtem Aufnahmefehler bleibt versiegelbar.");

    require(std::string(system_replay_scheduler_event_code(SchedulerEventKind::DiscRead)) ==
                    "gdrom-disc-read" &&
                std::string(
                    system_replay_scheduler_event_code(SchedulerEventKind::GdRomPacket)) ==
                    "gdrom-packet" &&
                std::string(
                    system_replay_scheduler_event_code(SchedulerEventKind::PvrRender)) ==
                    "pvr-render" &&
                std::string(
                    system_replay_scheduler_event_code(SchedulerEventKind::PvrVblankIn)) ==
                    "pvr-vblank-in" &&
                std::string(
                    system_replay_scheduler_event_code(SchedulerEventKind::PvrVblankOut)) ==
                    "pvr-vblank-out" &&
                std::string(
                    system_replay_scheduler_event_code(SchedulerEventKind::PvrHblank)) ==
                    "pvr-hblank" &&
                std::string(system_replay_scheduler_event_code(SchedulerEventKind::AicaTick)) ==
                    "aica-tick",
            "GD-ROM-, PVR- oder AICA-Schedulerklassen besitzen kein stabiles Replaymapping.");
    SystemReplayLog mapped_scheduler_log;
    EventScheduler mapped_scheduler(&mapped_scheduler_log);
    for (const auto kind : {SchedulerEventKind::DiscRead,
                            SchedulerEventKind::GdRomPacket,
                            SchedulerEventKind::PvrRender,
                            SchedulerEventKind::AicaTick}) {
        static_cast<void>(
            mapped_scheduler.schedule_at(1u, [](const auto, const auto) {}, kind));
    }
    static_cast<void>(mapped_scheduler.advance_to(1u, 8u));
    const std::array expected_scheduler_codes{
        std::string("gdrom-disc-read"),
        std::string("gdrom-packet"),
        std::string("pvr-render"),
        std::string("aica-tick")};
    require(mapped_scheduler_log.events().size() == expected_scheduler_codes.size() &&
                std::equal(mapped_scheduler_log.events().begin(),
                           mapped_scheduler_log.events().end(),
                           expected_scheduler_codes.begin(),
                           [](const auto& recorded, const auto& code) {
                               return recorded.kind ==
                                          SystemReplayEventKind::SchedulerCallback &&
                                      recorded.code == code;
                           }),
            "Scheduler schreibt GD-ROM-, PVR- und AICA-Klassen nicht in den Replaystrom.");

    std::cout << "KR-3609 deterministische Systemereignis-Replays erfolgreich.\n";
    return EXIT_SUCCESS;
}
