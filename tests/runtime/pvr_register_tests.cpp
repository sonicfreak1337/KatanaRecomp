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
    bus.write_u32(0xA05F8000u + pvr_register::FramebufferReadSize, 0x00100140u);
    require(bus.read_u32(0x605F8000u + pvr_register::FramebufferReadSof1) == 0x00400000u,
            "PVR-Registerzustand wird nicht zwischen Aliasen geteilt.");
    bus.write_u32(0x005F8000u + pvr_register::FramebufferWriteSof1, 0x01ABCDEFu);
    bus.write_u32(0x005F8000u + pvr_register::FramebufferWriteSof2, 0x03FEDCBBu);
    require(pvr->read(pvr_register::FramebufferWriteSof1) == 0x01ABCDECu &&
                pvr->read(pvr_register::FramebufferWriteSof2) == 0x01FEDCB8u,
            "FB_W_SOF verliert das Render-to-Texture-Bit oder maskiert reservierte Bits falsch.");
    const auto scheduler_cycle_before_snapshot = scheduler.current_cycle();
    const auto pending_before_snapshot = scheduler.pending_event_count();
    const auto diagnostic_snapshot = pvr->snapshot();
    require(diagnostic_snapshot.framebuffer_read_sof1 == 0x00400000u &&
                diagnostic_snapshot.framebuffer_read_size == 0x00100140u &&
                diagnostic_snapshot.framebuffer_write_sof1 == 0x01ABCDECu &&
                diagnostic_snapshot.framebuffer_write_sof2 == 0x01FEDCB8u &&
                diagnostic_snapshot.render_requests == 0u &&
                diagnostic_snapshot.render_completions == 0u &&
                scheduler.current_cycle() == scheduler_cycle_before_snapshot &&
                scheduler.pending_event_count() == pending_before_snapshot,
            "Strukturierter PVR-Snapshot verliert Zustand oder bewegt den Scheduler.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    require(pvr->render_request_count() == 1u && pvr->render_completion_count() == 0u &&
                scheduler.next_event_cycle() == 5u,
            "STARTRENDER erzeugt keine terminierte Completion.");
    const auto active_cycle_before_snapshot = scheduler.current_cycle();
    const auto active_pending_before_snapshot = scheduler.pending_event_count();
    const auto active_next_before_snapshot = scheduler.next_event_cycle();
    const auto active_render_snapshot = pvr->snapshot();
    require(active_render_snapshot.render_requests == 1u &&
                active_render_snapshot.render_completions == 0u &&
                scheduler.current_cycle() == active_cycle_before_snapshot &&
                scheduler.pending_event_count() == active_pending_before_snapshot &&
                scheduler.next_event_cycle() == active_next_before_snapshot && completions == 0u,
            "PVR-Snapshot bewegt eine ausstehende Rendercompletion oder meldet sie vorzeitig.");
    static_cast<void>(scheduler.advance_to(4u, 0u));
    require(completions == 0u, "PVR-Rendercompletion wird vor ihrer Frist sichtbar.");
    static_cast<void>(scheduler.advance_to(5u, 1u));
    require(completions == 1u && pvr->render_completion_count() == 1u,
            "PVR-Rendercompletion fehlt oder wird mehrfach gemeldet.");
    const auto scan_load_before_soft_reset = pvr->read(pvr_register::SpgLoad);
    bus.write_u32(0x005F8000u + pvr_register::FramebufferReadSof1, 0x00400000u);
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    bus.write_u32(0x005F8000u + pvr_register::SoftReset, 1u);
    static_cast<void>(scheduler.advance_to(10u, 1u));
    require(pvr->reset_count() == 1u &&
                pvr->read(pvr_register::FramebufferReadSof1) == 0x00400000u &&
                pvr->read(pvr_register::SpgLoad) == scan_load_before_soft_reset &&
                completions == 2u,
            "TA-Softreset loescht Register/Scanout oder stoppt faelschlich den ISP-Core.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    bus.write_u32(0x005F8000u + pvr_register::SoftReset, 2u);
    static_cast<void>(scheduler.advance_to(15u, 0u));
    require(pvr->reset_count() == 2u && completions == 2u,
            "PVR-Core-Softreset laesst eine stale Rendercompletion weiterlaufen.");
    bus.write_u32(0x005F8000u + pvr_register::SoftReset, 0u);
    scheduler.reset();
    static_cast<void>(scheduler.advance_to(5u, 0u));
    require(completions == 2u && pvr->render_completion_count() == 2u,
            "Schedulerreset fuehrt einen stale PVR-Rendercallback aus.");
    bus.write_u32(0x005F8000u + pvr_register::StartRender, 1u);
    static_cast<void>(scheduler.advance_to(10u, 1u));
    require(completions == 3u && pvr->render_completion_count() == 3u,
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
    scan_scheduler.reset();
    static_cast<void>(scan_scheduler.advance_to(50u, 32u));
    require((scan_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 5u,
            "Schedulerreset stellt das SPG-Scanouttiming nicht wieder her.");

    EventScheduler cadence_scheduler;
    Memory cadence_bus(0u);
    std::uint64_t cadence_vblank_in = 0u;
    std::uint64_t cadence_vblank_out = 0u;
    const auto cadence_pvr = map_pvr_registers(
        cadence_bus,
        cadence_scheduler,
        {},
        PvrTiming{5u, 100u, 100u},
        [&](const bool entering) {
            if (entering)
                ++cadence_vblank_in;
            else
                ++cadence_vblank_out;
        });
    cadence_pvr->write(pvr_register::SpgLoad, (9u << 16u) | 9u);
    cadence_pvr->write(pvr_register::SpgHblankInterrupt, (1u << 12u) | 0x3FFu);
    cadence_pvr->write(pvr_register::SpgVblankInterrupt, (0x3FFu << 16u) | 0u);
    const auto require_frame_cycles = [&](const bool vclk_div,
                                          const bool interlaced,
                                          const std::uint64_t expected) {
        cadence_pvr->write(pvr_register::FramebufferReadControl,
                           vclk_div ? 1u << 23u : 0u);
        cadence_pvr->write(pvr_register::SpgControl, interlaced ? 1u << 4u : 0u);
        require(cadence_scheduler.next_event_cycle() == expected,
                "FB_R_CTRL.vclk_div oder SPG_CONTROL.interlace skaliert die "
                "SPG-Frameperiode falsch.");
    };
    require_frame_cycles(false, false, 200u);
    require_frame_cycles(false, true, 100u);
    require_frame_cycles(true, false, 100u);
    require_frame_cycles(true, true, 50u);

    cadence_pvr->write(pvr_register::SpgControl, 0u);
    cadence_pvr->write(pvr_register::FramebufferReadControl, 1u << 23u);
    cadence_pvr->write(pvr_register::SpgVblankInterrupt, (11u << 16u) | 10u);
    require(!cadence_scheduler.next_event_cycle().has_value(),
            "SPG_VBLANK_INT faltet ausserhalb von SPG_LOAD liegende Compare-Linien um.");
    static_cast<void>(cadence_scheduler.advance_to(200u, 32u));
    require(cadence_vblank_in == 0u && cadence_vblank_out == 0u,
            "Ungueltige SPG-VBlank-Compare-Linien haben einen Interrupt terminiert.");

    cadence_pvr->write(pvr_register::SpgVblankInterrupt, (11u << 16u) | 0u);
    require(cadence_scheduler.next_event_cycle() == 300u,
            "SPG-VBlank-Linie 0 liegt nicht an der naechsten Framegrenze.");
    static_cast<void>(cadence_scheduler.advance_to(299u, 32u));
    require(cadence_vblank_in == 0u,
            "SPG-VBlank-Linie 0 wurde innerhalb des aktuellen Frames ausgeloest.");
    static_cast<void>(cadence_scheduler.advance_to(300u, 32u));
    require(cadence_vblank_in == 1u && cadence_vblank_out == 0u,
            "SPG-VBlank-Linie 0 fehlt an der naechsten Framegrenze.");

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
