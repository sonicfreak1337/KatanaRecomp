#include "katana/runtime/host_runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
    EventScheduler scheduler;
    DreamcastMediaClock clock(scheduler, {600u, 60u, 60u, 2u});
    auto input = std::make_shared<InjectedHostInput>();
    RecordingHostAudioOutput audio;
    HostRuntimeSession session(scheduler, clock, input, audio);

    session.inject({1u, 0u, HostRuntimeEventKind::Resume, {}});
    require(session.state() == HostRuntimeState::Running && clock.running() &&
                scheduler.pending_event_count() == 2u,
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
                scheduler.pending_event_count() == 0u,
            "Fokusverlust pausiert Hostaudio oder Medienereignisse nicht.");
    session.inject({4u, 0u, HostRuntimeEventKind::FocusGained, {}});
    require(session.state() == HostRuntimeState::Running && clock.running() && !audio.paused(),
            "Fokusgewinn setzt den pausierten Hostvertrag nicht fort.");
    session.inject({5u, 0u, HostRuntimeEventKind::Shutdown, {}});
    require(session.state() == HostRuntimeState::Shutdown && audio.shutdown_complete() &&
                scheduler.pending_event_count() == 0u,
            "Sauberer Shutdown hinterlaesst Schedulerereignisse oder Hostaudio.");
    require(throws<std::logic_error>(
                [&] { session.inject({6u, 0u, HostRuntimeEventKind::Resume, {}}); }),
            "Hostereignis nach Shutdown wird akzeptiert.");

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
