#include "katana/runtime/host_runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename Exception, typename Callback> bool throws(Callback&& callback) {
    try {
        callback();
    } catch (const Exception&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(native_host_runtime_contract_version == 2u);
    static_assert(host_pacing_contract_version == 1u);
    std::uint64_t host_now = 1'000u;
    std::vector<std::uint64_t> deadlines;
    HostPacer pacer(
        {100u},
        [&] { return host_now; },
        [&](const std::uint64_t deadline) {
            deadlines.push_back(deadline);
            host_now = deadline;
        });
    pacer.resume(10u);
    pacer.pace(60u);
    require(deadlines == std::vector<std::uint64_t>{500'001'000u} && pacer.wait_calls() == 1u &&
                pacer.late_deadlines() == 0u,
            "Ganzzahliges Host-Pacing berechnet keine exakte Deadline.");
    pacer.pause(60u);
    host_now += 100u;
    pacer.resume(70u);
    pacer.pace(170u);
    require(deadlines.back() == 1'500'001'100u && pacer.last_guest_cycle() == 170u,
            "Pause/Resume verankert Hostzeit und Gastzyklus nicht neu.");
    host_now = deadlines.back() + 1u;
    pacer.pace(170u);
    require(pacer.late_deadlines() == 1u, "Verspaetete Hostdeadline wird nicht separat gezaehlt.");

    auto guest_now = std::uint64_t{10u};
    HostPacer guest_regression({100u}, [&] { return guest_now; });
    guest_regression.resume(20u);
    require(throws<HostPacingException>([&] { guest_regression.pace(19u); }) &&
                guest_regression.first_error()->error == HostPacingError::GuestCycleRegression,
            "Gastzyklusregression stoppt nicht mit typisierter Diagnose.");
    HostPacer host_regression({100u}, [&] { return guest_now; });
    host_regression.resume(0u);
    --guest_now;
    require(throws<HostPacingException>([&] { host_regression.pace(1u); }) &&
                host_regression.first_error()->error == HostPacingError::HostClockRegression,
            "Hostuhrregression stoppt nicht mit typisierter Diagnose.");
    std::uint64_t early_now = 0u;
    HostPacer early_wait({100u}, [&] { return early_now; }, [](const std::uint64_t) {});
    early_wait.resume(0u);
    require(throws<HostPacingException>([&] { early_wait.pace(100u); }) &&
                early_wait.first_error()->error == HostPacingError::WaitReturnedEarly,
            "Zu frueh zurueckkehrender Wait wird nicht abgewiesen.");
    std::uint64_t overflow_now =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1u;
    HostPacer overflow({1u}, [&] { return overflow_now; });
    overflow.resume(0u);
    require(throws<HostPacingException>([&] { overflow.pace(1u); }) &&
                overflow.first_error()->error == HostPacingError::DeadlineOverflow,
            "Deadline-Ueberlauf wird nicht typisiert abgewiesen.");
    require(pacer.serialize_status_json().find("\"wait_calls\":2") != std::string::npos &&
                guest_regression.first_error().has_value(),
            "Host-Pacing-Status verliert Metriken oder ersten Fehler.");

    EventScheduler scheduler;
    DreamcastMediaClock clock(scheduler, {600u, 60u, 60u, 2u});
    auto input = std::make_shared<InjectedHostInput>();
    RecordingHostAudioOutput audio;
    std::uint64_t session_host_now = 0u;
    HostPacer session_pacer(
        {600u},
        [&] { return session_host_now; },
        [&](const std::uint64_t deadline) { session_host_now = deadline; });
    std::uint64_t saves = 0u;
    HostRuntimeSession session(scheduler, clock, input, audio, &session_pacer, [&] { ++saves; });

    session.inject({1u, 0u, HostRuntimeEventKind::Resume, {}});
    require(session.state() == HostRuntimeState::Running && clock.running() &&
                scheduler.pending_event_count() == 2u && session_pacer.running(),
            "Explizites Resume startet den Medienvertrag nicht.");
    ControllerState controller{};
    controller.pressed_buttons = static_cast<std::uint16_t>(ControllerButton::A) |
                                 static_cast<std::uint16_t>(ControllerButton::DpadRight);
    session.inject({2u, 0u, HostRuntimeEventKind::Controller, controller});
    const auto sampled = input->sample(0u);
    require(sampled.pressed_buttons == controller.pressed_buttons &&
                input->injected_events() == 1u && input->sampled_frames() == 1u,
            "Explizit injizierte Controllereingabe erreicht Maple nicht deterministisch.");

    const std::array<std::int16_t, 8u> samples = {0, 1, -1, 2, -2, 3, -3, 4};
    audio.submit(samples, 44'100u);
    const auto first_hash = audio.deterministic_hash();
    RecordingHostAudioOutput replay_audio;
    replay_audio.submit(samples, 44'100u);
    require(first_hash == replay_audio.deterministic_hash() && audio.submitted_frames() == 4u,
            "Identische Audiopuffer besitzen keinen reproduzierbaren Hash.");

    session.inject({3u, 0u, HostRuntimeEventKind::FocusLost, {}});
    require(session.state() == HostRuntimeState::Paused && !clock.running() && audio.paused() &&
                scheduler.pending_event_count() == 0u && !session_pacer.running() &&
                input->sample(1u).pressed_buttons == 0u && input->injected_events() == 2u,
            "Fokusverlust pausiert Medien nicht oder laesst Controllerbuttons klemmen.");
    session.inject({4u, 0u, HostRuntimeEventKind::FocusGained, {}});
    require(session.state() == HostRuntimeState::Running && clock.running() && !audio.paused(),
            "Fokusgewinn setzt den pausierten Hostvertrag nicht fort.");
    session.inject({5u, 0u, HostRuntimeEventKind::Shutdown, {}});
    require(session.state() == HostRuntimeState::Shutdown && audio.shutdown_complete() &&
                scheduler.pending_event_count() == 0u && session_pacer.shutdown_complete() &&
                saves == 1u,
            "Sauberer Shutdown hinterlaesst Schedulerereignisse oder Hostaudio.");
    require(throws<std::logic_error>(
                [&] { session.inject({6u, 0u, HostRuntimeEventKind::Resume, {}}); }),
            "Hostereignis nach Shutdown wird akzeptiert.");
    session.shutdown();
    require(saves == 1u, "Wiederholter Shutdown speichert die Arbeitskopien mehrfach.");

    EventScheduler save_scheduler;
    DreamcastMediaClock save_clock(save_scheduler, {600u, 60u, 60u, 2u});
    auto save_input = std::make_shared<InjectedHostInput>();
    RecordingHostAudioOutput save_audio;
    HostRuntimeSession save_failure(
        save_scheduler, save_clock, save_input, save_audio, nullptr, [] {
            throw std::runtime_error("save-failed");
        });
    require(throws<std::runtime_error>(
                [&] { save_failure.inject({1u, 0u, HostRuntimeEventKind::Shutdown, {}}); }) &&
                save_failure.state() == HostRuntimeState::Shutdown &&
                save_failure.shutdown_error() == "persistent-storage-save-failed",
            "Persistenzfehler bleibt nach noexcept-Shutdown nicht reproduzierbar sichtbar.");

    EventScheduler failing_scheduler;
    DreamcastMediaClock failing_clock(failing_scheduler, {600u, 60u, 60u, 2u});
    auto failing_input = std::make_shared<InjectedHostInput>();
    RecordingHostAudioOutput failing_audio;
    HostRuntimeSession failing(failing_scheduler, failing_clock, failing_input, failing_audio);
    failing.inject({1u, 0u, HostRuntimeEventKind::Resume, {}});
    require(throws<std::invalid_argument>(
                [&] { failing.inject({1u, 0u, HostRuntimeEventKind::Pause, {}}); }) &&
                failing.state() == HostRuntimeState::Shutdown &&
                failing_scheduler.pending_event_count() == 0u,
            "Fehlerpfad faehrt Hostruntime und Scheduler nicht kontrolliert herunter.");

#ifdef _WIN32
    if (native_audio_available()) {
        auto native = create_native_audio_output();
        const std::array<std::int16_t, 4u> silence{};
        native->submit(silence, 44'100u);
        require(native->submitted_buffers() == 1u && native->deterministic_hash() != 0u,
                "WinMM-Hostaudio nimmt den synthetischen Puffer nicht an.");
        native->pause();
        native->resume();
        native->shutdown();
        require(native->shutdown_complete(), "WinMM-Hostaudio wird nicht sauber beendet.");
    }
#else
    require(!native_audio_available(), "Nicht implementiertes Hostaudio wird behauptet.");
#endif
    std::cout << "KR_NATIVE_HOST_LIFECYCLE_READY\n";
}
