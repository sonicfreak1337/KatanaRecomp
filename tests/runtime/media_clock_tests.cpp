#include "katana/runtime/aica.hpp"
#include "katana/runtime/media_clock.hpp"
#include "katana/runtime/pvr.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Exception, typename Function>
bool throws(Function&& function) {
    try { function(); } catch (const Exception&) { return true; }
    return false;
}

} // namespace

int main() {
    using namespace katana::runtime;

    EventScheduler scheduler;
    RecordingPvrRenderBackend video_backend;
    RecordingAicaAudioBackend audio_backend;
    std::vector<std::string> order;
    DreamcastMediaClock clock(
        scheduler,
        MediaClockConfig{600u, 60u, 120u, 4u},
        [&](const VideoTick& tick) {
            order.push_back("V" + std::to_string(tick.guest_cycle));
            video_backend.render(PvrTaFrame{}, {});
        },
        [&](const AudioTick& tick) {
            order.push_back("A" + std::to_string(tick.guest_cycle));
            std::vector<std::int16_t> silence(tick.frame_count * 2u, 0);
            audio_backend.submit(silence, 120u);
        }
    );
    clock.start();
    require(
        clock.running() && clock.next_video_cycle() == 10u && clock.next_audio_cycle() == 20u,
        "Medienuhr plant erste Video-/Audiofristen nicht deterministisch."
    );
    const auto limited = scheduler.advance_to(60u, 4u);
    require(
        limited.status == SchedulerAdvanceStatus::EventBudgetExhausted &&
            limited.guest_cycle == 30u && clock.video_tick_count() == 3u &&
            clock.audio_tick_count() == 1u,
        "Medienuhr respektiert das gemeinsame Scheduler-Budget nicht."
    );
    const auto completed = scheduler.advance_to(60u, 5u);
    require(
        completed.status == SchedulerAdvanceStatus::ReachedTarget &&
            clock.video_tick_count() == 6u && clock.audio_tick_count() == 3u &&
            clock.emitted_audio_frames() == 12u &&
            video_backend.submitted_frames() == 6u &&
            audio_backend.submitted_buffers() == 3u &&
            audio_backend.submitted_frames() == 12u,
        "Frame-/Audio-Callbacks oder Zaehler verlieren Scheduler-Ticks."
    );
    require(
        order == std::vector<std::string>{
            "V10", "A20", "V20", "V30", "A40", "V40", "V50", "A60", "V60"
        },
        "Gleiche Frame-/Audiofristen besitzen keine stabile Ereignisreihenfolge."
    );

    clock.stop();
    static_cast<void>(scheduler.advance_to(100u, 0u));
    require(
        clock.video_tick_count() == 6u && clock.audio_tick_count() == 3u &&
            !clock.next_video_cycle().has_value() && !clock.next_audio_cycle().has_value(),
        "Gestoppte Medienuhr laesst geplante Ticks weiterlaufen."
    );
    clock.start();
    require(clock.next_video_cycle() == 110u && clock.next_audio_cycle() == 120u,
        "Neustart ankert Medienfristen nicht am aktuellen Gastzyklus.");
    static_cast<void>(scheduler.advance_to(120u, 3u));
    require(clock.video_tick_count() == 8u && clock.audio_tick_count() == 4u,
        "Medienuhr kann nach Stop nicht deterministisch fortgesetzt werden.");
    clock.reset();
    require(
        !clock.running() && clock.video_tick_count() == 0u &&
            clock.audio_tick_count() == 0u && clock.emitted_audio_frames() == 0u,
        "Medienuhr-Reset stellt keinen zaehlerfreien Grundzustand her."
    );

    EventScheduler fractional_scheduler;
    std::vector<std::uint64_t> fractional_video_cycles;
    DreamcastMediaClock fractional(
        fractional_scheduler,
        MediaClockConfig{10u, 3u, 1u, 100u},
        [&](const VideoTick& tick) { fractional_video_cycles.push_back(tick.guest_cycle); }
    );
    fractional.start();
    static_cast<void>(fractional_scheduler.advance_to(10u, 3u));
    require(
        fractional_video_cycles == std::vector<std::uint64_t>{3u, 6u, 10u},
        "Rationale Frame-Kadenz akkumuliert Rundungsdrift."
    );

    EventScheduler failing_scheduler;
    DreamcastMediaClock failing(
        failing_scheduler,
        MediaClockConfig{60u, 60u, 60u, 1u},
        [](const VideoTick&) { throw std::runtime_error("sichtbarer Video-Callbackfehler"); }
    );
    failing.start();
    require(
        throws<std::runtime_error>([&] {
            static_cast<void>(failing_scheduler.advance_to(1u, 2u));
        }) && !failing.running() && failing_scheduler.pending_event_count() == 0u,
        "Callbackfehler propagiert nicht oder laesst die Medienuhr halbaktiv."
    );

    require(
        throws<std::invalid_argument>([&] {
            DreamcastMediaClock invalid(scheduler, MediaClockConfig{0u, 60u, 44'100u, 735u});
        }) &&
            throws<std::invalid_argument>([&] {
                DreamcastMediaClock invalid(scheduler, MediaClockConfig{1u, 60u, 44'100u, 735u});
            }) &&
            throws<std::invalid_argument>([&] {
                DreamcastMediaClock invalid(scheduler, MediaClockConfig{60u, 60u, 60u, 0u});
            }),
        "Medienuhr akzeptiert Null- oder Subzykluskonfigurationen."
    );

    EventScheduler callback_stop_scheduler;
    DreamcastMediaClock* callback_stop_clock = nullptr;
    DreamcastMediaClock callback_stop(
        callback_stop_scheduler,
        MediaClockConfig{60u, 60u, 1u, 100u},
        [&](const VideoTick&) { callback_stop_clock->stop(); }
    );
    callback_stop_clock = &callback_stop;
    callback_stop.start();
    static_cast<void>(callback_stop_scheduler.advance_to(1u, 1u));
    require(
        !callback_stop.running() && callback_stop.video_tick_count() == 1u &&
            callback_stop_scheduler.pending_event_count() == 0u,
        "Video-Callback-stop laesst ein Folgeereignis aktiv."
    );
    static_cast<void>(callback_stop_scheduler.advance_to(100u, 0u));
    require(callback_stop.video_tick_count() == 1u,
        "Video-Callback-stop erzeugt einen spaeteren Geistertick.");

    EventScheduler callback_restart_scheduler;
    DreamcastMediaClock* callback_restart_clock = nullptr;
    std::size_t restart_callbacks = 0u;
    DreamcastMediaClock callback_restart(
        callback_restart_scheduler,
        MediaClockConfig{60u, 60u, 1u, 100u},
        [&](const VideoTick&) {
            ++restart_callbacks;
            if (restart_callbacks == 1u) {
                callback_restart_clock->stop();
                callback_restart_clock->start();
            }
        }
    );
    callback_restart_clock = &callback_restart;
    callback_restart.start();
    static_cast<void>(callback_restart_scheduler.advance_to(1u, 1u));
    require(
        callback_restart.running() && callback_restart_scheduler.pending_event_count() == 2u &&
            callback_restart.next_video_cycle() == 2u,
        "Video-Callback-Neustart plant doppelte oder unkontrollierbare Ereignisse."
    );
    static_cast<void>(callback_restart_scheduler.advance_to(2u, 1u));
    require(restart_callbacks == 2u && callback_restart.video_tick_count() == 2u,
        "Video-Callback-Neustart erzeugt mehr als einen Tick pro Frist.");
    callback_restart.stop();
    require(callback_restart_scheduler.pending_event_count() == 0u,
        "Stop nach Video-Callback-Neustart kann nicht alle Ereignisse abbrechen.");
    static_cast<void>(callback_restart_scheduler.advance_to(100u, 0u));
    require(restart_callbacks == 2u,
        "Nach Stop des neugestarteten Videopfads bleibt ein Geistertick."
    );

    EventScheduler audio_reset_scheduler;
    DreamcastMediaClock* audio_reset_clock = nullptr;
    std::size_t audio_reset_callbacks = 0u;
    DreamcastMediaClock audio_reset(
        audio_reset_scheduler,
        MediaClockConfig{60u, 1u, 60u, 1u},
        {},
        [&](const AudioTick&) {
            ++audio_reset_callbacks;
            audio_reset_clock->reset();
        }
    );
    audio_reset_clock = &audio_reset;
    audio_reset.start();
    static_cast<void>(audio_reset_scheduler.advance_to(1u, 1u));
    require(
        audio_reset_callbacks == 1u && !audio_reset.running() &&
            audio_reset.audio_tick_count() == 0u &&
            audio_reset.emitted_audio_frames() == 0u &&
            audio_reset_scheduler.pending_event_count() == 0u,
        "Audio-Callback-Reset laesst Zaehler oder Ereignisse aus dem alten Lauf zurueck."
    );
    audio_reset.stop();
    static_cast<void>(audio_reset_scheduler.advance_to(100u, 0u));
    require(audio_reset_callbacks == 1u,
        "Audio-Callback-Reset hinterlaesst nach Stop einen Geistertick."
    );

    std::cout << "KR-3105 Frame- und Audio-Taktung erfolgreich.\n";
    return 0;
}
