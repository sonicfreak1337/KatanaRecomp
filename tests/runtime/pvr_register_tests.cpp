#include "katana/runtime/pvr.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
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
    bus.write_u32(0x005F8000u + pvr_register::BorderColor, 0x00123456u);
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
                diagnostic_snapshot.registers[pvr_register::BorderColor / 4u] ==
                    0x00123456u &&
                diagnostic_snapshot.registers[pvr_register::Id / 4u] == pvr_id &&
                diagnostic_snapshot.registers[pvr_register::Revision / 4u] ==
                    pvr_revision &&
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
                active_render_snapshot.render_event_ids.size() == 1u &&
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

    EventScheduler result_scheduler;
    PvrRegisterFile result_pvr(result_scheduler, PvrTiming{3u});
    std::uint64_t result_observer_calls = 0u;
    result_pvr.set_render_result_observer([&] {
        ++result_observer_calls;
        return result_observer_calls == 1u ? PvrRenderResult::Failed
                                          : PvrRenderResult::Success;
    });
    const auto expect_render_failure =
        [](EventScheduler& target, const std::uint64_t cycle) {
            try {
                static_cast<void>(target.advance_to(cycle, 1u));
            } catch (const PvrRenderFailed& error) {
                return error.failure();
            }
            require(false, "PVR-Renderfehler beendet den Produktpfad nicht typisiert.");
            return PvrRenderFailure{};
        };
    result_pvr.write(pvr_register::StartRender, 1u);
    const auto rejected_render = expect_render_failure(result_scheduler, 3u);
    const auto failed_result_snapshot = result_pvr.snapshot();
    require(result_observer_calls == 1u && result_pvr.render_request_count() == 1u &&
                result_pvr.render_completion_count() == 0u &&
                result_pvr.render_failure_count() == 1u &&
                failed_result_snapshot.render_completions == 0u &&
                failed_result_snapshot.render_failures == 1u &&
                rejected_render.request == 1u && rejected_render.generation == 1u &&
                rejected_render.error == PvrRenderError::UnsupportedFeature &&
                rejected_render.ta_packet_class == "none" &&
                rejected_render.register_digest == 0u &&
                rejected_render.guest_cycle == 3u &&
                failed_result_snapshot.last_render_failure == rejected_render &&
                result_pvr.last_render_failure() == rejected_render,
            "Fehlgeschlagener PVR-Render verliert typisierten strukturierten Abschluss.");
    result_pvr.write(pvr_register::StartRender, 1u);
    static_cast<void>(result_scheduler.advance_to(6u, 1u));
    require(result_observer_calls == 2u && result_pvr.render_request_count() == 2u &&
                result_pvr.render_completion_count() == 1u &&
                result_pvr.render_failure_count() == 1u,
            "Erfolgreicher PVR-Renderabschluss wird nicht getrennt von Fehlern gezaehlt.");
    result_pvr.set_render_result_observer({});
    result_pvr.write(pvr_register::StartRender, 1u);
    const auto missing_render_path = expect_render_failure(result_scheduler, 9u);
    require(result_pvr.render_completion_count() == 1u &&
                result_pvr.render_failure_count() == 2u &&
                missing_render_path.request == 3u &&
                missing_render_path.generation == 3u &&
                missing_render_path.error == PvrRenderError::InternalLifecycle,
            "STARTRENDER ohne produktiven Renderpfad erfindet eine Completion "
            "oder bleibt untypisiert.");
    result_pvr.set_render_result_observer([]() -> PvrRenderResult {
        throw std::runtime_error("synthetic-render-observer-failure");
    });
    result_pvr.write(pvr_register::StartRender, 1u);
    const auto thrown_render_path = expect_render_failure(result_scheduler, 12u);
    require(result_pvr.render_completion_count() == 1u &&
                result_pvr.render_failure_count() == 3u &&
                thrown_render_path.request == 4u &&
                thrown_render_path.generation == 4u &&
                thrown_render_path.error == PvrRenderError::InternalLifecycle &&
                thrown_render_path.detail == "synthetic-render-observer-failure",
            "Renderobserver-Exception entkommt untypisiert oder wird als Completion gezaehlt.");

    EventScheduler frozen_scheduler;
    PvrRegisterFile frozen_pvr(frozen_scheduler, PvrTiming{5u});
    PvrTaFifo frozen_fifo;
    std::array<std::uint8_t, 32u> list_start{};
    list_start[3u] = 0x40u;
    const std::array<std::uint8_t, 32u> list_end{};
    const auto submit_closed_list = [&] {
        frozen_fifo.submit(list_start);
        frozen_fifo.submit(list_end);
    };
    struct ExecutedRenderJob {
        std::uint64_t request = 0u;
        std::uint64_t generation = 0u;
        std::uint64_t start_cycle = 0u;
        std::uint64_t captured_packets = 0u;
        std::uint32_t captured_border = 0u;
    };
    std::vector<ExecutedRenderJob> executed_jobs;
    std::uint64_t render_factory_calls = 0u;
    std::optional<PvrRegisterSnapshot> snapshot_during_commit;
    frozen_pvr.set_render_job_factory(
        [&](const PvrRegisterSnapshot& register_snapshot,
            const std::uint64_t request,
            const std::uint64_t generation,
            const std::uint64_t start_cycle) {
            auto staged_fifo = frozen_fifo;
            static_cast<void>(staged_fifo.finish_frame());
            const auto captured_packets = staged_fifo.metrics().packets;
            const auto captured_border = register_snapshot.read(pvr_register::BorderColor);
            const auto payload_digest =
                captured_packets ^ (static_cast<std::uint64_t>(captured_border) << 32u);
            ++render_factory_calls;
            return PvrRegisterFile::PreparedRenderJob{
                [&, request, generation, start_cycle, captured_packets, captured_border] {
                    executed_jobs.push_back(ExecutedRenderJob{
                        request, generation, start_cycle, captured_packets, captured_border});
                    return PvrRenderResult::Success;
                },
                [&frozen_fifo,
                 &frozen_pvr,
                 &snapshot_during_commit,
                 staged_fifo = std::move(staged_fifo)]() mutable {
                    snapshot_during_commit = frozen_pvr.snapshot();
                    frozen_fifo = std::move(staged_fifo);
                },
                payload_digest,
            };
        });
    submit_closed_list();
    frozen_pvr.write(pvr_register::BorderColor, 0x00112233u);
    frozen_pvr.write(pvr_register::StartRender, 1u);
    submit_closed_list();
    frozen_pvr.write(pvr_register::BorderColor, 0x00445566u);
    frozen_pvr.write(pvr_register::StartRender, 1u);
    const auto busy_render = frozen_pvr.snapshot();
    require(snapshot_during_commit &&
                snapshot_during_commit->render_event_ids.empty() &&
                snapshot_during_commit->active_render_request == 0u &&
                snapshot_during_commit->active_render_generation == 0u &&
                render_factory_calls == 1u && executed_jobs.empty() &&
                busy_render.render_requests == 2u && busy_render.render_failures == 1u &&
                busy_render.render_overruns == 1u &&
                busy_render.next_render_generation == 2u &&
                busy_render.active_render_request == 1u &&
                busy_render.active_render_generation == 1u &&
                busy_render.active_render_start_cycle == 0u &&
                busy_render.active_render_payload_digest ==
                    (2u ^ (static_cast<std::uint64_t>(0x00112233u) << 32u)) &&
                busy_render.last_render_start_error == PvrRenderStartError::Busy &&
                busy_render.render_event_ids.size() == 1u,
            "STARTRENDER veroeffentlicht einen halben Auftrag oder ersetzt ihn bei Busy.");
    static_cast<void>(frozen_scheduler.advance_to(5u, 1u));
    require(executed_jobs.size() == 1u && executed_jobs[0u].request == 1u &&
                executed_jobs[0u].generation == 1u &&
                executed_jobs[0u].start_cycle == 0u &&
                executed_jobs[0u].captured_packets == 2u &&
                executed_jobs[0u].captured_border == 0x00112233u,
            "TA-Pakete oder Register nach STARTRENDER mutieren den laufenden Renderauftrag.");
    frozen_pvr.write(pvr_register::StartRender, 1u);
    static_cast<void>(frozen_scheduler.advance_to(10u, 1u));
    require(render_factory_calls == 2u && executed_jobs.size() == 2u &&
                executed_jobs[1u].request == 3u && executed_jobs[1u].generation == 2u &&
                executed_jobs[1u].start_cycle == 5u &&
                executed_jobs[1u].captured_packets == 4u &&
                executed_jobs[1u].captured_border == 0x00445566u &&
                frozen_pvr.render_completion_count() == 2u &&
                frozen_pvr.render_failure_count() == 1u,
            "Nach STARTRENDER eintreffende TA-Pakete werden nicht dem naechsten Auftrag zugeordnet.");

    EventScheduler rollback_scheduler;
    PvrRegisterFile rollback_pvr(rollback_scheduler, PvrTiming{5u});
    rollback_pvr.set_render_job_factory(
        [](const PvrRegisterSnapshot&, const std::uint64_t, const std::uint64_t,
           const std::uint64_t) -> PvrRegisterFile::PreparedRenderJob {
            throw 7;
        });
    rollback_pvr.write(pvr_register::StartRender, 1u);
    const auto capture_rollback = rollback_pvr.snapshot();
    require(capture_rollback.render_event_ids.empty() &&
                capture_rollback.active_render_request == 0u &&
                capture_rollback.active_render_generation == 0u &&
                capture_rollback.active_render_start_cycle == 0u &&
                capture_rollback.active_render_payload_digest == 0u &&
                capture_rollback.next_render_generation == 2u &&
                capture_rollback.render_failures == 1u &&
                capture_rollback.last_render_start_error ==
                    PvrRenderStartError::CaptureFailed,
            "Fehlgeschlagene STARTRENDER-Capture hinterlaesst einen halben Auftrag.");

    EventScheduler registration_scheduler;
    PvrRegisterFile registration_pvr(
        registration_scheduler,
        PvrTiming{std::numeric_limits<std::uint64_t>::max()});
    static_cast<void>(registration_scheduler.advance_to(1u, 0u));
    PvrTaFifo registration_fifo;
    registration_fifo.submit(list_start);
    registration_fifo.submit(list_end);
    const auto fifo_before_registration = registration_fifo.snapshot();
    std::uint64_t registration_commit_calls = 0u;
    registration_pvr.set_render_job_factory(
        [&](const PvrRegisterSnapshot&,
            const std::uint64_t,
            const std::uint64_t,
            const std::uint64_t) {
            auto staged_fifo = registration_fifo;
            static_cast<void>(staged_fifo.finish_frame());
            return PvrRegisterFile::PreparedRenderJob{
                [] { return PvrRenderResult::Success; },
                [&registration_fifo,
                 &registration_commit_calls,
                 staged_fifo = std::move(staged_fifo)]() mutable noexcept {
                    registration_fifo = std::move(staged_fifo);
                    ++registration_commit_calls;
                },
            };
        });
    registration_pvr.write(pvr_register::StartRender, 1u);
    const auto registration_failure = registration_pvr.snapshot();
    const auto fifo_after_registration = registration_fifo.snapshot();
    require(registration_commit_calls == 0u &&
                registration_failure.render_requests == 1u &&
                registration_failure.render_failures == 1u &&
                registration_failure.render_event_ids.empty() &&
                registration_failure.next_render_generation == 2u &&
                registration_failure.active_render_request == 0u &&
                registration_failure.active_render_generation == 0u &&
                registration_failure.active_render_start_cycle == 0u &&
                registration_failure.active_render_payload_digest == 0u &&
                registration_failure.last_render_start_error ==
                    PvrRenderStartError::SchedulerFailure &&
                fifo_after_registration.metrics.frames_current_generation ==
                    fifo_before_registration.metrics.frames_current_generation &&
                fifo_after_registration.metrics.packets ==
                    fifo_before_registration.metrics.packets &&
                fifo_after_registration.accelerator.frame_has_list ==
                    fifo_before_registration.accelerator.frame_has_list &&
                fifo_after_registration.accelerator.list_open ==
                    fifo_before_registration.accelerator.list_open,
            "Schedulerfehler nach STARTRENDER-Prepare konsumiert den alten TA-Auftrag.");
    static_cast<void>(registration_fifo.finish_frame());
    require(registration_fifo.metrics().frames_current_generation ==
                fifo_before_registration.metrics.frames_current_generation + 1u &&
                registration_fifo.metrics().frames_lifetime ==
                    fifo_before_registration.metrics.frames_lifetime + 1u,
            "Nach Schedulerrollback ist der vorbereitete TA-Auftrag nicht mehr renderbar.");

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

    EventScheduler wrapped_vblank_scheduler;
    Memory wrapped_vblank_bus(0u);
    std::uint64_t wrapped_vblank_in = 0u;
    std::uint64_t wrapped_vblank_out = 0u;
    const auto wrapped_vblank_pvr = map_pvr_registers(
        wrapped_vblank_bus,
        wrapped_vblank_scheduler,
        {},
        PvrTiming{5u, 100u, 100u},
        [&](const bool entering) {
            if (entering)
                ++wrapped_vblank_in;
            else
                ++wrapped_vblank_out;
        });
    wrapped_vblank_pvr->write(pvr_register::SpgLoad, (9u << 16u) | 9u);
    wrapped_vblank_pvr->write(pvr_register::FramebufferReadControl, 1u << 23u);
    wrapped_vblank_pvr->write(pvr_register::SpgControl, 0u);
    wrapped_vblank_pvr->write(pvr_register::SpgHblankInterrupt,
                              (1u << 12u) | 0x3FFu);
    wrapped_vblank_pvr->write(pvr_register::SpgVblankInterrupt,
                              (2u << 16u) | 8u);
    require(wrapped_vblank_pvr->in_vblank(),
            "Gewrappter VBlank-Bereich startet am Rasterpunkt null nicht aktiv.");
    static_cast<void>(wrapped_vblank_scheduler.advance_to(20u, 32u));
    require(!wrapped_vblank_pvr->in_vblank() && wrapped_vblank_in == 0u &&
                wrapped_vblank_out == 1u,
            "Erstes VBlank-Out eines gewrappten Bereichs besitzt falschen Anfangszustand.");
    static_cast<void>(wrapped_vblank_scheduler.advance_to(50u, 32u));
    wrapped_vblank_pvr->write(pvr_register::VideoControl, 0u);
    require(!wrapped_vblank_pvr->in_vblank(),
            "Mid-frame-Reschedule setzt gewrappten VBlank faelschlich auf Rasterpunkt null.");
    static_cast<void>(wrapped_vblank_scheduler.advance_to(79u, 32u));
    require(!wrapped_vblank_pvr->in_vblank() && wrapped_vblank_in == 0u &&
                wrapped_vblank_out == 1u,
            "Mid-frame-Reschedule verschiebt die naechste VBlank-In-Grenze.");
    static_cast<void>(wrapped_vblank_scheduler.advance_to(80u, 32u));
    require(wrapped_vblank_pvr->in_vblank() && wrapped_vblank_in == 1u &&
                wrapped_vblank_out == 1u,
            "Gewrappter VBlank-Bereich tritt an seiner spaeten In-Grenze nicht erneut ein.");

    EventScheduler full_reset_scheduler;
    Memory full_reset_bus(0u);
    const auto full_reset_pvr = map_pvr_registers(
        full_reset_bus, full_reset_scheduler, {}, PvrTiming{5u, 100u, 100u});
    full_reset_pvr->write(pvr_register::VideoControl, 0u);
    full_reset_pvr->write(
        pvr_register::FramebufferReadControl, 1u << 23u);
    full_reset_pvr->write(pvr_register::SpgControl, 0u);
    full_reset_pvr->write(pvr_register::SpgHblankInterrupt,
                          (1u << 12u) | 0x3FFu);
    full_reset_pvr->write(pvr_register::SpgVblankInterrupt,
                          (2u << 16u) | 8u);
    full_reset_pvr->write(pvr_register::SpgLoad, (9u << 16u) | 9u);
    static_cast<void>(full_reset_scheduler.advance_to(50u, 32u));
    const auto before_runtime_reschedule = full_reset_pvr->snapshot();
    full_reset_pvr->write(pvr_register::VideoControl, 1u);
    const auto after_runtime_reschedule = full_reset_pvr->snapshot();
    require(before_runtime_reschedule.scan_epoch_cycle ==
                    after_runtime_reschedule.scan_epoch_cycle &&
                (full_reset_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 5u,
            "Laufende PVR-Registeraenderung verliert die bestehende Rasterphase.");

    full_reset_pvr->reset();
    const auto after_full_reset = full_reset_pvr->snapshot();
    require(after_full_reset.scan_frame_cycles != 0u &&
                after_full_reset.scan_epoch_cycle ==
                    full_reset_scheduler.current_cycle() &&
                after_full_reset.in_vblank &&
                (full_reset_pvr->read(pvr_register::SpgStatus) & 0x3FFu) == 0u,
            "PVR-Vollreset uebernimmt Rasterepoche oder VBlank-Zustand des alten Modus.");

    const auto reset_vertical =
        static_cast<std::uint64_t>(
            (after_full_reset.read(pvr_register::SpgLoad) >> 16u) & 0x3FFu) +
        1u;
    const auto reset_vblank_out_line = static_cast<std::uint64_t>(
        (after_full_reset.read(pvr_register::SpgVblankInterrupt) >> 16u) &
        0x3FFu);
    const auto reset_vblank_out_cycle =
        after_full_reset.scan_epoch_cycle +
        std::max<std::uint64_t>(
            1u,
            after_full_reset.scan_frame_cycles * reset_vblank_out_line /
                reset_vertical);
    static_cast<void>(
        full_reset_scheduler.advance_to(reset_vblank_out_cycle - 1u, 32u));
    require(full_reset_pvr->in_vblank() &&
                full_reset_pvr->vblank_in_count() ==
                    after_full_reset.vblank_in &&
                full_reset_pvr->vblank_out_count() ==
                    after_full_reset.vblank_out,
            "PVR-Vollreset verlaesst den gewrappten Default-VBlank vor der "
            "Out-Grenze.");
    static_cast<void>(
        full_reset_scheduler.advance_to(reset_vblank_out_cycle, 32u));
    require(!full_reset_pvr->in_vblank() &&
                full_reset_pvr->vblank_in_count() ==
                    after_full_reset.vblank_in &&
                full_reset_pvr->vblank_out_count() ==
                    after_full_reset.vblank_out + 1u,
            "Erstes VBlank-Ereignis nach PVR-Vollreset ist nicht VBlankOut.");

    full_reset_pvr->set_ta_reset_observer(
        [] { throw std::runtime_error("synthetic-ta-reset-observer"); });
    require(throws<std::runtime_error>([&] { full_reset_pvr->reset(); }),
            "Werfender TA-Resetobserver wurde an der oeffentlichen Grenze verschluckt.");
    const auto after_observer_failure = full_reset_pvr->snapshot();
    require(after_observer_failure.scan_frame_cycles != 0u &&
                after_observer_failure.scan_epoch_cycle ==
                    full_reset_scheduler.current_cycle() &&
                after_observer_failure.in_vblank &&
                full_reset_scheduler.next_event_cycle().has_value(),
            "TA-Resetobserverfehler strandet den bereits reseteten PVR ohne "
            "Rasterereignisse.");

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

    DreamcastPvrStateSnapshot policy_snapshot;
    policy_snapshot.registers.scan_epoch_cycle = 99u;
    policy_snapshot.registers.next_render_generation = 43u;
    policy_snapshot.registers.render_requests = 9u;
    policy_snapshot.registers.render_completions = 7u;
    policy_snapshot.registers.render_failures = 2u;
    policy_snapshot.registers.vblank_in = 4u;
    policy_snapshot.registers.last_render_start_error =
        PvrRenderStartError::CaptureFailed;
    policy_snapshot.ta_fifo.frame_packets = 5u;
    policy_snapshot.ta_fifo.pending_intensity_header = true;
    policy_snapshot.ta_fifo.metrics.packets = 17u;
    policy_snapshot.ta_fifo.metrics.frames_current_generation = 3u;
    policy_snapshot.ta_fifo.metrics.frames_lifetime = 3u;
    policy_snapshot.ta_fifo.metrics.rejected_packets = 1u;
    policy_snapshot.ta_fifo.first_input_error =
        PvrTaInputError{PvrTaInputErrorReason::InvalidPacket, 17u, "captured"};
    policy_snapshot.yuv.input = {1u, 2u, 3u};
    policy_snapshot.yuv.frame_macroblock = 4u;
    policy_snapshot.yuv.converted_macroblocks = 21u;
    policy_snapshot.renderer.metrics.frames = 6u;
    policy_snapshot.renderer.metrics.proven_guest_frames = 2u;
    policy_snapshot.renderer.next_render_generation = 43u;
    policy_snapshot.renderer.last_render_generation = 42u;
    policy_snapshot.renderer.pending_render_evidence.emplace_back();
    policy_snapshot.renderer.pending_render_evidence_bytes = 12u;
    policy_snapshot.renderer.next_evidence_scan_generation = 42u;
    policy_snapshot.renderer.next_direct_write_generation = 10u;
    policy_snapshot.renderer.pending_direct_write_generation = 9u;
    policy_snapshot.renderer.pending_direct_first_write_generation = 8u;
    policy_snapshot.renderer.pending_direct_last_write_generation = 9u;
    policy_snapshot.renderer.direct_dirty_words = {1u};
    policy_snapshot.renderer.direct_dirty_byte_count = 1u;
    policy_snapshot.renderer.direct_vram_shadow = {0xFFu};
    policy_snapshot.renderer.direct_vram_shadow_valid = true;
    policy_snapshot.renderer.queued_guest_frame_proof.emplace();
    policy_snapshot.renderer.first_error =
        PvrRenderFirstError{PvrRenderError::MemoryRange, 9u, "captured"};

    normalize_dreamcast_pvr_observations_for_restore(
        policy_snapshot,
        ObservationRestorePolicy::PreserveCapturedDiagnostics);
    require(policy_snapshot.registers.render_requests == 9u &&
                policy_snapshot.ta_fifo.metrics.packets == 17u &&
                policy_snapshot.yuv.converted_macroblocks == 21u &&
                policy_snapshot.renderer.metrics.frames == 6u &&
                policy_snapshot.renderer.queued_guest_frame_proof,
            "PVR-Diagnoserestore veraendert den Capture-Snapshot.");

    const std::vector<std::uint8_t> final_vram(8u << 20u, 0x5Au);
    normalize_dreamcast_pvr_observations_for_restore(
        policy_snapshot,
        ObservationRestorePolicy::FreshProductEpoch,
        final_vram);
    require(policy_snapshot.registers.render_requests == 0u &&
                policy_snapshot.registers.render_completions == 0u &&
                policy_snapshot.registers.render_failures == 0u &&
                policy_snapshot.registers.vblank_in == 0u &&
                !policy_snapshot.registers.last_render_start_error &&
                policy_snapshot.ta_fifo.metrics.packets == 0u &&
                policy_snapshot.ta_fifo.metrics.frames_current_generation == 0u &&
                policy_snapshot.ta_fifo.metrics.frames_lifetime == 0u &&
                policy_snapshot.ta_fifo.metrics.rejected_packets == 0u &&
                !policy_snapshot.ta_fifo.first_input_error &&
                policy_snapshot.yuv.converted_macroblocks == 0u &&
                policy_snapshot.renderer.metrics.frames == 0u &&
                policy_snapshot.renderer.metrics.proven_guest_frames == 0u &&
                policy_snapshot.renderer.pending_render_evidence.empty() &&
                policy_snapshot.renderer.next_evidence_scan_generation == 0u &&
                policy_snapshot.renderer.next_direct_write_generation == 1u &&
                policy_snapshot.renderer.pending_direct_write_generation == 0u &&
                policy_snapshot.renderer.pending_direct_first_write_generation == 0u &&
                policy_snapshot.renderer.pending_direct_last_write_generation == 0u &&
                policy_snapshot.renderer.direct_dirty_words.empty() &&
                policy_snapshot.renderer.direct_dirty_byte_count == 0u &&
                policy_snapshot.renderer.direct_vram_shadow == final_vram &&
                !policy_snapshot.renderer.queued_guest_frame_proof &&
                !policy_snapshot.renderer.first_error &&
                policy_snapshot.registers.scan_epoch_cycle == 99u &&
                policy_snapshot.registers.next_render_generation == 43u &&
                policy_snapshot.ta_fifo.frame_packets == 5u &&
                policy_snapshot.ta_fifo.pending_intensity_header &&
                policy_snapshot.yuv.input == std::vector<std::uint8_t>({1u, 2u, 3u}) &&
                policy_snapshot.yuv.frame_macroblock == 4u &&
                policy_snapshot.renderer.next_render_generation == 43u &&
                policy_snapshot.renderer.last_render_generation == 42u,
            "Frischer PVR-Produktepoch entfernt Geraetefortsetzung oder behaelt Evidence.");

    std::cout << "KR-2801 PVR-Registerminimum erfolgreich.\n";
}
