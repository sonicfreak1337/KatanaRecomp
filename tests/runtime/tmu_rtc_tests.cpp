#include "katana/runtime/timers.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}
template <typename Exception, typename Function>
bool throws(Function&& function) {
    try { function(); } catch (const Exception&) { return true; }
    return false;
}
}

int main() {
    using namespace katana::runtime;
    EventScheduler scheduler;
    Sh4Tmu tmu(scheduler, TmuTiming{1u, 64u});
    tmu.write_constant(0u, 1u);
    tmu.write_counter(0u, 1u);
    tmu.write_control(0u, Sh4Tmu::underflow_interrupt_enable);
    tmu.write_start(1u);
    const auto before = scheduler.advance_to(7u, 1u);
    require(before.processed_events == 0u && tmu.counter(0u) == 0u,
        "TMU zaehlt Pck/4 nicht deterministisch herunter.");
    const auto underflow = scheduler.advance_to(8u, 1u);
    require(underflow.processed_events == 1u && tmu.counter(0u) == 1u &&
            (tmu.control(0u) & Sh4Tmu::underflow_flag) != 0u &&
            tmu.interrupt_pending(0u) && tmu.underflow_count(0u) == 1u,
        "TMU-Unterlauf setzt Flag, Auto-Reload oder Interruptzustand nicht.");
    tmu.acknowledge_interrupt(0u);
    tmu.write_control(0u, Sh4Tmu::underflow_interrupt_enable);
    require(!tmu.interrupt_pending(0u) &&
            (tmu.control(0u) & Sh4Tmu::underflow_flag) == 0u,
        "TMU-UNF kann nicht mit Write-zero-to-clear quittiert werden.");
    static_cast<void>(scheduler.advance_to(16u, 1u));
    require(tmu.underflow_count(0u) == 2u, "TMU-Auto-Reload plant keinen Folgeunterlauf.");
    tmu.write_start(0u);
    const auto stopped_counter = tmu.counter(0u);
    static_cast<void>(scheduler.advance_to(100u, 0u));
    require(tmu.counter(0u) == stopped_counter, "Gestoppter TMU-Kanal zaehlt weiter.");

    tmu.write_counter(1u, 0u);
    tmu.write_control(1u, 6u);
    tmu.write_start(2u);
    static_cast<void>(scheduler.advance_to(163u, 0u));
    require(tmu.underflow_count(1u) == 0u, "RTC-getakteter TMU-Kanal laeuft zu frueh ab.");
    static_cast<void>(scheduler.advance_to(164u, 1u));
    require(tmu.underflow_count(1u) == 1u, "RTC-getakteter TMU-Kanal laeuft nicht exakt ab.");
    require(throws<std::invalid_argument>([&] { tmu.write_control(2u, 5u); }) &&
            throws<std::invalid_argument>([&] { tmu.write_control(2u, 7u); }) &&
            throws<std::out_of_range>([&] { static_cast<void>(tmu.counter(3u)); }),
        "Ungueltige TMU-Konfiguration wird nicht abgewiesen.");
    tmu.reset();
    require(tmu.start() == 0u && tmu.counter(0u) == 0xFFFFFFFFu,
        "TMU-Reset stellt keinen SH-4-Grundzustand her.");

    EventScheduler rtc_scheduler;
    Sh4Rtc rtc(rtc_scheduler, 256u);
    rtc.set_date_time(RtcDateTime{2024u, 12u, 31u, 2u, 23u, 59u, 59u});
    rtc.set_periodic_rate(RtcPeriodicRate::Every1Over64Second);
    rtc.set_carry_interrupt_enabled(true);
    rtc.start();
    static_cast<void>(rtc_scheduler.advance_to(256u, 256u));
    require(rtc.date_time() == RtcDateTime{2025u, 1u, 1u, 3u, 0u, 0u, 0u} &&
            rtc.counter_64hz() == 0u && rtc.tick_count() == 256u &&
            rtc.periodic_event_count() == 64u && rtc.periodic_interrupt_pending() &&
            rtc.carry_interrupt_pending(),
        "RTC-Jahreswechsel, Teiler oder Interruptzaehler ist falsch.");
    require(throws<std::logic_error>([&] { rtc.set_date_time(RtcDateTime{}); }),
        "Laufende RTC erlaubt Kalenderwechsel.");
    rtc.acknowledge_periodic_interrupt();
    rtc.acknowledge_carry_interrupt();
    rtc.stop();
    const auto stopped_time = rtc.date_time();
    static_cast<void>(rtc_scheduler.advance_to(512u, 0u));
    require(rtc.date_time() == stopped_time, "Gestoppte RTC laeuft weiter.");
    rtc.set_date_time(RtcDateTime{2024u, 2u, 28u, 3u, 23u, 59u, 59u});
    rtc.start();
    static_cast<void>(rtc_scheduler.advance_to(768u, 256u));
    require(rtc.date_time() == RtcDateTime{2024u, 2u, 29u, 4u, 0u, 0u, 0u},
        "RTC-Schaltjahrbehandlung ist falsch.");
    rtc.stop();
    require(throws<std::invalid_argument>([&] {
                rtc.set_date_time(RtcDateTime{2023u, 2u, 29u, 3u, 0u, 0u, 0u});
            }) &&
            throws<std::invalid_argument>([&] { Sh4Rtc invalid(rtc_scheduler, 255u); }),
        "RTC akzeptiert ungueltige Kalender- oder Taktkonfiguration.");
    std::cout << "KR-3102 TMU und RTC erfolgreich.\n";
}
