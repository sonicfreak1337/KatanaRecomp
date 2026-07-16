#include "katana/runtime/scheduler_safepoint.hpp"
#include "katana/runtime/system_replay.hpp"

#include <cstdlib>
#include <iostream>
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
                              "\"event_hash\":\"0x",
                              "\"final_guest_state_hash\":\"0x"}) {
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

    SystemReplayLog mmio;
    auto observer = system_replay_mmio_observer(mmio, [] { return 20u; }, "pvr-register");
    observer({MemoryAccessOperation::Write, 0xA05F8000u, MemoryAccessWidth::Word, 0x1234u, "pvr"});
    require(mmio.events().size() == 1u &&
                mmio.events()[0].kind == SystemReplayEventKind::MmioWrite &&
                mmio.events()[0].guest_cycle == 20u,
            "MMIO-Beobachter erzeugt kein geordnetes Systemereignis.");

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
