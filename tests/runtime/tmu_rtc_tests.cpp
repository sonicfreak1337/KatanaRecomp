#include "katana/runtime/timers.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename Exception, typename Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;

    EventScheduler scheduler;
    auto slow_rtc_clock = std::make_shared<Sh4RtcClockDomain>(1'048'576u);
    Sh4Tmu tmu(scheduler, TmuTiming{1u, slow_rtc_clock});
    tmu.write_constant(0u, 1u);
    tmu.write_counter(0u, 1u);
    tmu.write_control(0u, Sh4Tmu::underflow_interrupt_enable);
    tmu.write_start(1u);
    const auto initial_tmu_snapshot = tmu.snapshot();
    require(initial_tmu_snapshot.channels[0u].running &&
                initial_tmu_snapshot.channels[0u].stored_counter == 1u &&
                initial_tmu_snapshot.channels[0u].effective_counter == 1u &&
                initial_tmu_snapshot.channels[0u].anchor_cycle == 0u &&
                initial_tmu_snapshot.channels[0u].event.has_value() &&
                initial_tmu_snapshot.channels[0u].event_deadline == 8u,
            "TMU-Snapshot verliert laufenden Zaehler, Anker oder Ereignisfrist.");
    const auto before = scheduler.advance_to(7u, 1u);
    const auto scheduler_before_running_snapshot = scheduler.snapshot();
    const auto running_tmu_snapshot = tmu.snapshot();
    require(before.processed_events == 0u &&
                running_tmu_snapshot.channels[0u].stored_counter == 1u &&
                running_tmu_snapshot.channels[0u].effective_counter == 0u &&
                running_tmu_snapshot.channels[0u].event_deadline == 8u &&
                scheduler.snapshot() == scheduler_before_running_snapshot &&
                tmu.counter(0u) == 0u,
            "TMU zaehlt Pck/4 nicht deterministisch herunter.");
    const auto underflow = scheduler.advance_to(8u, 1u);
    require(underflow.processed_events == 1u && tmu.counter(0u) == 1u &&
                (tmu.control(0u) & Sh4Tmu::underflow_flag) != 0u && tmu.interrupt_pending(0u) &&
                tmu.underflow_count(0u) == 1u,
            "TMU-Unterlauf setzt Flag, Auto-Reload oder Interruptzustand nicht.");
    tmu.acknowledge_interrupt(0u);
    require(tmu.interrupt_pending(0u),
            "TMU-Acknowledge erzeugt trotz gesetztem UNF und UNIE einen unmoeglichen Zustand.");
    tmu.write_control(0u, Sh4Tmu::underflow_interrupt_enable);
    require(!tmu.interrupt_pending(0u) && (tmu.control(0u) & Sh4Tmu::underflow_flag) == 0u,
            "TMU-UNF kann nicht mit Write-zero-to-clear quittiert werden.");
    static_cast<void>(scheduler.advance_to(16u, 1u));
    require(tmu.underflow_count(0u) == 2u, "TMU-Auto-Reload plant keinen Folgeunterlauf.");
    tmu.write_start(0u);
    const auto stopped_counter = tmu.counter(0u);
    static_cast<void>(scheduler.advance_to(100u, 0u));
    const auto stopped_tmu_snapshot = tmu.snapshot();
    require(tmu.counter(0u) == stopped_counter &&
                !stopped_tmu_snapshot.channels[0u].running &&
                stopped_tmu_snapshot.channels[0u].stored_counter == stopped_counter &&
                stopped_tmu_snapshot.channels[0u].effective_counter == stopped_counter &&
                !stopped_tmu_snapshot.channels[0u].event.has_value() &&
                !stopped_tmu_snapshot.channels[0u].event_deadline.has_value(),
            "Gestoppter TMU-Kanal zaehlt weiter oder verliert seinen Snapshotzustand.");

    tmu.write_counter(1u, 0u);
    tmu.write_control(1u, 6u);
    tmu.write_start(2u);
    static_cast<void>(scheduler.advance_to(127u, 0u));
    require(tmu.underflow_count(1u) == 0u, "RTC-getakteter TMU-Kanal laeuft zu frueh ab.");
    static_cast<void>(scheduler.advance_to(128u, 1u));
    require(tmu.underflow_count(1u) == 1u,
            "RTC-getakteter TMU-Kanal laeuft nicht auf der naechsten 16,384-kHz-Flanke ab.");
    require(throws<std::invalid_argument>([&] { tmu.write_control(2u, 5u); }) &&
                throws<std::invalid_argument>([&] { tmu.write_control(2u, 7u); }) &&
                throws<std::out_of_range>([&] { static_cast<void>(tmu.counter(3u)); }),
            "Ungueltige TMU-Konfiguration wird nicht abgewiesen.");

    EventScheduler default_clock_scheduler;
    Sh4Tmu default_clock_tmu(default_clock_scheduler);
    default_clock_tmu.write_counter(0u, 0u);
    default_clock_tmu.write_control(0u, 6u);
    default_clock_tmu.write_start(1u);
    require(default_clock_scheduler.next_event_cycle() == 12'208u,
            "TMU-TPSC=110 verwendet nicht den rationalen 16,384-kHz-RTCCLK.");
    Sh4RtcClockDomain dreamcast_clock;
    require(dreamcast_clock.deadline_after(0u, 16'384u) == 200'000'000u,
            "Rationale RTCCLK-Phase driftet ueber eine Sekunde.");

    EventScheduler coupled_scheduler;
    auto coupled_clock = std::make_shared<Sh4RtcClockDomain>(1'048'576u);
    Sh4Tmu coupled_tmu(coupled_scheduler, TmuTiming{1u, coupled_clock});
    Sh4Rtc coupled_rtc(coupled_scheduler, coupled_clock);
    coupled_tmu.write_counter(0u, 0u);
    coupled_tmu.write_control(0u, 6u);
    coupled_tmu.write_start(1u);
    static_cast<void>(coupled_scheduler.advance_to(20u, 0u));
    coupled_rtc.reset_divider();
    require(coupled_scheduler.next_event_cycle() == 84u,
            "RTC-Teilerreset rephasiert den RTC-getakteten TMU-Kanal nicht.");

    EventScheduler rtc_scheduler;
    auto rtc_clock = std::make_shared<Sh4RtcClockDomain>(256u);
    Sh4Rtc rtc(rtc_scheduler, rtc_clock);
    rtc.set_date_time(RtcDateTime{2024u, 12u, 31u, 2u, 23u, 59u, 59u});
    rtc.set_periodic_rate(RtcPeriodicRate::Every1Over64Second);
    rtc.set_carry_interrupt_enabled(true);
    rtc.start();
    static_cast<void>(rtc_scheduler.advance_to(254u, 254u));
    require(rtc.counter_64hz() == 127u && rtc.date_time().second == 59u,
            "RTC-R64CNT bildet nicht alle sieben 64-Hz-bis-1-Hz-Bits ab.");
    static_cast<void>(rtc_scheduler.advance_to(256u, 2u));
    require(rtc.date_time() == RtcDateTime{2025u, 1u, 1u, 3u, 0u, 0u, 0u} &&
                rtc.counter_64hz() == 0u && rtc.tick_count() == 256u &&
                rtc.periodic_event_count() == 64u && rtc.periodic_interrupt_pending() &&
                rtc.carry_flag() && rtc.carry_interrupt_pending(),
            "RTC-Jahreswechsel, Sieben-Bit-Teiler oder Interruptzaehler ist falsch.");
    require(throws<std::logic_error>([&] { rtc.set_date_time(RtcDateTime{}); }),
            "Laufende RTC erlaubt Kalenderwechsel.");

    rtc.acknowledge_periodic_interrupt();
    rtc.acknowledge_carry_interrupt();
    rtc.stop();
    const auto stopped_time = rtc.date_time();
    static_cast<void>(rtc_scheduler.advance_to(512u, 256u));
    require(rtc.date_time() == stopped_time && rtc.tick_count() == 512u &&
                rtc.counter_64hz() == 0u && rtc.carry_flag(),
            "START=0 stoppt faelschlich RTCEN oder laesst den Kalender weiterlaufen.");
    rtc.set_carry_interrupt_enabled(false);
    require(rtc.carry_flag() && !rtc.carry_interrupt_pending(),
            "CIE=0 loescht faelschlich das unabhaengige Carry-Flag.");
    rtc.set_carry_interrupt_enabled(true);
    require(rtc.carry_interrupt_pending(),
            "CIE=1 aktiviert ein bereits gesetztes Carry-Flag nicht.");
    rtc.acknowledge_carry_interrupt();
    require(!rtc.carry_flag() && !rtc.carry_interrupt_pending(),
            "RTC-Carry-Acknowledge loescht das Flag nicht.");
    rtc.set_rtc_enabled(false);
    const auto disabled_ticks = rtc.tick_count();
    static_cast<void>(rtc_scheduler.advance_to(768u, 0u));
    require(rtc.tick_count() == disabled_ticks, "RTCEN=0 laesst den RTC-Teiler weiterlaufen.");

    rtc.set_date_time(RtcDateTime{2024u, 2u, 28u, 3u, 23u, 59u, 59u});
    rtc.start();
    static_cast<void>(rtc_scheduler.advance_to(1024u, 256u));
    require(rtc.date_time() == RtcDateTime{2024u, 2u, 29u, 4u, 0u, 0u, 0u},
            "RTC-Schaltjahrbehandlung ist falsch.");
    rtc.stop();
    rtc.set_rtc_enabled(false);
    require(throws<std::invalid_argument>(
                [&] { rtc.set_date_time(RtcDateTime{2023u, 2u, 29u, 3u, 0u, 0u, 0u}); }) &&
                throws<std::invalid_argument>([&] { Sh4Rtc invalid(rtc_scheduler, 0u); }),
            "RTC akzeptiert ungueltige Kalender- oder Taktkonfiguration.");

    EventScheduler phase_scheduler;
    Sh4Rtc phase_rtc(phase_scheduler, 256u);
    phase_rtc.set_periodic_rate(RtcPeriodicRate::Every1Over64Second);
    phase_rtc.start();
    static_cast<void>(phase_scheduler.advance_to(3u, 3u));
    phase_rtc.reset_divider();
    static_cast<void>(phase_scheduler.advance_to(6u, 3u));
    require(!phase_rtc.periodic_interrupt_pending(),
            "RTC-Teilerreset behaelt eine alte periodische Phase.");
    static_cast<void>(phase_scheduler.advance_to(7u, 1u));
    require(phase_rtc.periodic_interrupt_pending(),
            "RTC-Teilerreset startet die periodische Phase nicht neu.");

    EventScheduler reset_scheduler;
    auto reset_clock = std::make_shared<Sh4RtcClockDomain>(256u);
    Sh4Tmu reset_tmu(reset_scheduler, TmuTiming{1u, reset_clock});
    Sh4Rtc reset_rtc(reset_scheduler, reset_clock);
    reset_tmu.write_counter(0u, 1u);
    reset_tmu.write_constant(0u, 1u);
    reset_tmu.write_start(1u);
    reset_rtc.set_date_time(RtcDateTime{2024u, 1u, 1u, 1u, 0u, 0u, 59u});
    reset_rtc.start();
    static_cast<void>(reset_scheduler.advance_to(4u, 4u));
    reset_scheduler.reset();
    bool foreign_event_ran = false;
    const auto foreign = reset_scheduler.schedule_at(
        300u, [&](const auto, const auto) { foreign_event_ran = true; });
    static_cast<void>(foreign);
    static_cast<void>(reset_scheduler.advance_to(256u, 400u));
    require(reset_rtc.date_time().second == 0u && reset_tmu.underflow_count(0u) != 0u,
            "Scheduler-Reset friert laufende RTC/TMU-Ereignisse ein.");
    reset_tmu.reset();
    reset_rtc.set_rtc_enabled(false);
    require(reset_scheduler.pending_event_count() == 1u,
            "Timer-Reset loescht nach Scheduler-Reset ein fremdes Ereignis.");
    static_cast<void>(reset_scheduler.advance_to(300u, 1u));
    require(foreign_event_ran,
            "Scheduler-Reset hinterlaesst eine kollidierende Timer-Ereignis-ID.");

    EventScheduler reverse_scheduler;
    auto reverse_clock = std::make_shared<Sh4RtcClockDomain>(1'048'576u);
    Sh4Rtc reverse_rtc(reverse_scheduler, reverse_clock);
    Sh4Tmu reverse_tmu(reverse_scheduler, TmuTiming{1u, reverse_clock});
    reverse_tmu.write_counter(0u, 0u);
    reverse_tmu.write_constant(0u, 0u);
    reverse_tmu.write_control(0u, 6u);
    reverse_tmu.write_start(1u);
    reverse_scheduler.reset();
    require(reverse_scheduler.pending_event_count() == 1u,
            "RTC-vor-TMU-Konstruktionsreihenfolge erzeugt beim Reset doppelte Ereignisse.");
    static_cast<void>(reverse_scheduler.advance_to(64u, 2u));
    require(reverse_tmu.underflow_count(0u) == 1u && reverse_scheduler.pending_event_count() == 1u,
            "Doppeltes geisterhaftes TMU-Ereignis ueberlebt den Scheduler-Reset.");

    std::cout << "KR-3102 TMU und RTC erfolgreich.\n";
}
