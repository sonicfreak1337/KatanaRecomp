#include "katana/runtime/pvr.hpp"

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
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    Memory bus(0u);
    EventScheduler scheduler;
    std::uint64_t completions = 0u;
    const auto pvr = map_pvr_registers(bus, scheduler, [&] { ++completions; }, PvrTiming{5u});
    require(bus.read_u32(0x005F8000u) == pvr_id && bus.read_u32(0x805F8004u) == pvr_revision,
            "PVR-ID oder Revision ist ueber Aliase nicht lesbar.");
    bus.write_u32(0xA05F8000u + pvr_register::FramebufferReadSof1, 0x04000000u);
    require(bus.read_u32(0x605F8000u + pvr_register::FramebufferReadSof1) == 0x04000000u,
            "PVR-Registerzustand wird nicht zwischen Aliasen geteilt.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    require(pvr->render_request_count() == 1u && pvr->render_completion_count() == 0u &&
                scheduler.next_event_cycle() == 5u,
            "STARTRENDER erzeugt keine terminierte Completion.");
    static_cast<void>(scheduler.advance_to(4u, 0u));
    require(completions == 0u, "PVR-Rendercompletion wird vor ihrer Frist sichtbar.");
    static_cast<void>(scheduler.advance_to(5u, 1u));
    require(completions == 1u && pvr->render_completion_count() == 1u,
            "PVR-Rendercompletion fehlt oder wird mehrfach gemeldet.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    bus.write_u32(0x005F8000u + pvr_register::SoftReset, 1u);
    static_cast<void>(scheduler.advance_to(10u, 0u));
    require(pvr->reset_count() == 1u && pvr->read(pvr_register::FramebufferReadSof1) == 0u &&
                completions == 1u && !scheduler.next_event_cycle().has_value(),
            "PVR-Softreset loescht den Registerzustand nicht.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    scheduler.reset();
    static_cast<void>(scheduler.advance_to(5u, 0u));
    require(completions == 1u && pvr->render_completion_count() == 1u,
            "Schedulerreset fuehrt einen stale PVR-Rendercallback aus.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    static_cast<void>(scheduler.advance_to(10u, 1u));
    require(completions == 2u && pvr->render_completion_count() == 2u,
            "PVR plant nach Schedulerreset keine frische Completion.");
    require(throws<std::runtime_error>([&] { bus.write_u32(0x005F8000u, 0u); }),
            "Read-only-PVR-ID ist beschreibbar.");
    require(throws<std::runtime_error>([&] { static_cast<void>(bus.read_u16(0x005F8000u)); }),
            "PVR akzeptiert still einen nicht unterstuetzten 16-Bit-Zugriff.");
    require(!bus.contains(0xE05F8000u), "P4 wurde faelschlich als direkter PVR-Alias abgebildet.");

    std::cout << "KR-2801 PVR-Registerminimum erfolgreich.\n";
}
