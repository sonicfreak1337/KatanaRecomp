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
    bus.write_u32(0xA05F8000u + pvr_register::FramebufferReadSof1, 0x00400000u);
    require(bus.read_u32(0x605F8000u + pvr_register::FramebufferReadSof1) == 0x00400000u,
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
    require(throws<std::runtime_error>([&] {
                bus.write_u32(0x005F8000u + pvr_register::SpgStatus, 0u);
            }),
            "Read-only-SPG_STATUS ist beschreibbar.");
    require(throws<std::runtime_error>([&] { static_cast<void>(bus.read_u16(0x005F8000u)); }),
            "PVR akzeptiert still einen nicht unterstuetzten 16-Bit-Zugriff.");
    require(!bus.contains(0xE05F8000u), "P4 wurde faelschlich als direkter PVR-Alias abgebildet.");

    std::uint64_t ta_resets = 0u;
    std::uint64_t ta_continuations = 0u;
    pvr->set_ta_reset_observer([&] { ++ta_resets; });
    pvr->set_ta_continue_observer([&] { ++ta_continuations; });
    pvr->write(pvr_register::TaObjectListBase, 0x04801237u);
    pvr->write(pvr_register::TaIspBase, 0x04802003u);
    pvr->write(pvr_register::TaNextOpbInit, 0x04803457u);
    pvr->write(pvr_register::TaInit, 0x80000000u);
    require(pvr->read(pvr_register::TaObjectListBase) == 0x00001234u &&
                pvr->read(pvr_register::TaIspBase) == 0x00002000u &&
                pvr->read(pvr_register::TaNextOpb) == 0x00003454u &&
                pvr->read(pvr_register::TaIspCurrent) == 0x00002000u && ta_resets == 1u,
            "TA_LIST_INIT verwendet Basis, NEXT_OPB_INIT, Masken oder Resetcallback falsch.");
    pvr->write(pvr_register::TaObjectListBase, 0x00005678u);
    pvr->write(pvr_register::TaListContinue, 0u);
    require(pvr->read(pvr_register::TaNextOpb) == 0x00005678u && ta_continuations == 1u,
            "TA_LIST_CONT uebernimmt OL_BASE oder Fortsetzungscallback nicht.");
    require(throws<std::runtime_error>(
                [&] { pvr->write(pvr_register::TaNextOpb, 0x1000u); }) &&
                throws<std::runtime_error>(
                    [&] { pvr->write(pvr_register::TaIspCurrent, 0x1000u); }),
            "Read-only-TA-Positionsregister sind beschreibbar.");

    EventScheduler scan_scheduler;
    Memory scan_bus(0u);
    const auto scan_pvr = map_pvr_registers(
        scan_bus, scan_scheduler, {}, PvrTiming{5u, 100u, 100u});
    scan_bus.write_u32(0x005F8000u + pvr_register::VideoControl, 0u);
    scan_bus.write_u32(0x005F8000u + pvr_register::SpgControl, 0x10u);
    scan_bus.write_u32(0x005F8000u + pvr_register::SpgHblank, (9u << 16u) | 8u);
    scan_bus.write_u32(0x005F8000u + pvr_register::SpgVblank, (6u << 16u) | 2u);
    scan_bus.write_u32(0x005F8000u + pvr_register::SpgLoad, (9u << 16u) | 9u);
    require((scan_pvr->read(pvr_register::SpgStatus) & ((1u << 13u) | 0x3FFu)) == 0u,
            "SPG_STATUS meldet Scanline oder Vertical Blank am Frameanfang falsch.");
    static_cast<void>(scan_scheduler.advance_to(50u, 32u));
    require((scan_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 5u &&
                (scan_pvr->read(pvr_register::SpgStatus) & (1u << 11u)) != 0u,
            "SPG_STATUS folgt der Gastzeit nicht mit einer dynamischen Scanline.");
    static_cast<void>(scan_scheduler.advance_to(60u, 32u));
    require((scan_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 6u &&
                (scan_pvr->read(pvr_register::SpgStatus) & (1u << 11u)) == 0u,
            "SPG_STATUS setzt das dokumentierte Blank-Bit nicht an der VBlank-Grenze.");
    static_cast<void>(scan_scheduler.advance_to(100u, 32u));
    require((scan_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 0u &&
                (scan_pvr->read(pvr_register::SpgStatus) & (1u << 10u)) != 0u,
            "SPG_STATUS setzt Feldnummer oder Scanline am Framewechsel falsch.");

    const auto require_profile = [&](const DreamcastVideoMode mode,
                                     const std::uint32_t load,
                                     const std::uint32_t hblank,
                                     const std::uint32_t vblank,
                                     const std::uint32_t width,
                                     const std::uint32_t control) {
        configure_dreamcast_video(*scan_pvr, mode);
        require(scan_pvr->read(pvr_register::SpgLoad) == load &&
                    scan_pvr->read(pvr_register::SpgHblank) == hblank &&
                    scan_pvr->read(pvr_register::SpgVblank) == vblank &&
                    scan_pvr->read(pvr_register::SpgWidth) == width &&
                    scan_pvr->read(pvr_register::SpgControl) == control,
                "Dreamcast-Videoprofil verliert einen dokumentierten SPG-Wert.");
    };
    require_profile(DreamcastVideoMode::NtscNonInterlaced,
                    0x01060359u, 0x007E0345u, 0x00120102u, 0x03F1933Fu, 0x00000140u);
    require_profile(DreamcastVideoMode::NtscInterlaced,
                    0x020C0359u, 0x007E0345u, 0x00240204u, 0x07D6C63Fu, 0x00000150u);
    require_profile(DreamcastVideoMode::PalNonInterlaced,
                    0x0138035Fu, 0x008D034Bu, 0x002C026Cu, 0x07F1F53Fu, 0x00000180u);
    require_profile(DreamcastVideoMode::PalInterlaced,
                    0x0270035Fu, 0x008D034Bu, 0x002C026Cu, 0x07D6A53Fu, 0x00000190u);
    require_profile(DreamcastVideoMode::Vga,
                    0x020C0359u, 0x007E0345u, 0x00280208u, 0x03F1933Fu, 0x00000100u);
    require(throws<std::invalid_argument>([&] {
                configure_dreamcast_video(*scan_pvr, static_cast<DreamcastVideoMode>(0xFFu));
            }),
            "Unbekannter Dreamcast-Videomodus wird still akzeptiert.");

    std::cout << "KR-2801 PVR-Registerminimum erfolgreich.\n";
}
