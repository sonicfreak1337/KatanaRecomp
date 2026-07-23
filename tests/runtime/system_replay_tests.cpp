#include "katana/runtime/scheduler_safepoint.hpp"
#include "katana/runtime/system_replay.hpp"

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
    observer({MemoryAccessOperation::Write, 0xA05F8000u, MemoryAccessWidth::Word, 0x1234u, "pvr"});
    require(mmio.events().size() == 1u &&
                mmio.events()[0].kind == SystemReplayEventKind::MmioWrite &&
                mmio.events()[0].guest_cycle == 20u,
            "MMIO-Beobachter erzeugt kein geordnetes Systemereignis.");

    SystemReplayLog reset_mmio;
    EventScheduler reset_scheduler;
    auto reset_observer = system_replay_mmio_observer(
        reset_mmio,
        [&] { return reset_scheduler.current_cycle(); },
        "reset-mmio",
        [&] { return reset_scheduler.reset_generation(); });
    static_cast<void>(reset_scheduler.advance_to(100u, 1u));
    reset_observer({MemoryAccessOperation::Write, 0xA05F8000u, MemoryAccessWidth::Word, 1u, "pvr"});
    reset_scheduler.reset();
    static_cast<void>(reset_scheduler.advance_to(5u, 1u));
    reset_observer({MemoryAccessOperation::Write, 0xA05F8004u, MemoryAccessWidth::Word, 2u, "pvr"});
    reset_mmio.seal(0x1234u);
    const auto reset_copy = reset_mmio.serialize_json();
    require(reset_mmio.events().size() == 2u && reset_mmio.events()[1].time_epoch == 1u &&
                reset_mmio.dropped_events() == 0u && reset_copy == reset_mmio.serialize_json(),
            "MMIO-Replay verliert nach Scheduler-Reset Epoche oder Bytegleichheit.");

    const SafepointReport safepoint{
        SafepointKind::AfterDelaySlot, ExecutionOrigin::Fallback, 30u, 29u, 1u, 2u, true, true};
    const auto safepoint_event = make_safepoint_replay_event(safepoint);
    require(safepoint_event.kind == SystemReplayEventKind::CpuSafepoint &&
                safepoint_event.code == "after-delay-slot-fallback" &&
                safepoint_event.guest_cycle == 29u && safepoint_event.value == 3u,
            "Safepoint-Replay verliert Origin, Delay-Slot-Grenze oder Budget-/Interruptflags.");

    std::cout << "KR-3609 deterministische Systemereignis-Replays erfolgreich.\n";
    return EXIT_SUCCESS;
}
