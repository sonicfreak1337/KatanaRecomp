#include "katana/runtime/pvr.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace katana::runtime {
namespace {

[[nodiscard]] bool valid_pvr_list_type(
    const PvrListType value) noexcept {
    switch (value) {
    case PvrListType::Opaque:
    case PvrListType::OpaqueModifier:
    case PvrListType::Translucent:
    case PvrListType::TranslucentModifier:
    case PvrListType::PunchThrough:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_pvr_input_error_reason(
    const PvrTaInputErrorReason value) noexcept {
    switch (value) {
    case PvrTaInputErrorReason::InvalidPacket:
    case PvrTaInputErrorReason::UnsupportedPacket:
    case PvrTaInputErrorReason::InvalidListOrder:
    case PvrTaInputErrorReason::IncompletePacket:
    case PvrTaInputErrorReason::BufferOverflow:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_pvr_render_error(
    const PvrRenderError value) noexcept {
    switch (value) {
    case PvrRenderError::InvalidTaState:
    case PvrRenderError::InvalidConfiguration:
    case PvrRenderError::MemoryRange:
    case PvrRenderError::UnsupportedFeature:
    case PvrRenderError::InternalLifecycle:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_pvr_render_start_error(
    const PvrRenderStartError value) noexcept {
    switch (value) {
    case PvrRenderStartError::Busy:
    case PvrRenderStartError::CaptureFailed:
    case PvrRenderStartError::SchedulerFailure:
    case PvrRenderStartError::GenerationExhausted:
        return true;
    }
    return false;
}

void validate_pvr_vertex(const PvrVertex& vertex) {
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
        !std::isfinite(vertex.z) || !std::isfinite(vertex.u) ||
        !std::isfinite(vertex.v) || !std::isfinite(vertex.volume_u) ||
        !std::isfinite(vertex.volume_v))
        throw std::invalid_argument(
            "PVR-Handoff besitzt einen nicht endlichen Vertex.");
}

void validate_pvr_material(const PvrMaterial& material,
                           const std::size_t depth = 0u) {
    if (depth > 8u)
        throw std::invalid_argument(
            "PVR-Handoff-Materialgraph ist zyklisch oder zu tief.");
    if (material.depth_compare > 7u || material.culling > 3u ||
        material.texture_shading > 3u || material.texture_filter > 3u ||
        material.fog_mode > 3u || material.source_blend > 7u ||
        material.destination_blend > 7u ||
        material.user_clip_mode > 3u)
        throw std::invalid_argument(
            "PVR-Handoff-Material besitzt ungueltige Bitfelder.");
    if (material.volume_material)
        validate_pvr_material(*material.volume_material, depth + 1u);
}

[[nodiscard]] PvrMaterial clone_pvr_material(
    const PvrMaterial& material,
    const std::size_t depth = 0u) {
    validate_pvr_material(material, depth);
    auto result = material;
    result.volume_material.reset();
    if (material.volume_material)
        result.volume_material = std::make_shared<PvrMaterial>(
            clone_pvr_material(*material.volume_material, depth + 1u));
    return result;
}

void validate_pvr_primitive(const PvrPrimitive& primitive) {
    if (!valid_pvr_list_type(primitive.list))
        throw std::invalid_argument(
            "PVR-Handoff-Primitive besitzt einen ungueltigen Listentyp.");
    validate_pvr_material(primitive.material);
    for (const auto& vertex : primitive.vertices)
        validate_pvr_vertex(vertex);
}

void validate_pvr_modifier_volume(const PvrModifierVolume& volume) {
    if (volume.list != PvrListType::OpaqueModifier &&
        volume.list != PvrListType::TranslucentModifier)
        throw std::invalid_argument(
            "PVR-Handoff-Modifier-Volume besitzt einen ungueltigen Listentyp.");
    if (volume.depth_mode > 3u || volume.culling > 3u ||
        volume.user_clip_mode > 3u)
        throw std::invalid_argument(
            "PVR-Handoff-Modifier-Volume besitzt ungueltige Bitfelder.");
    for (const auto& triangle : volume.triangles)
        for (const auto& vertex : triangle)
            validate_pvr_vertex(vertex);
}

void detach_pvr_material_graphs(TileAcceleratorSnapshot& state) {
    state.current_material = clone_pvr_material(state.current_material);
    for (auto& primitive : state.primitives)
        primitive.material = clone_pvr_material(primitive.material);
}

void detach_pvr_material_graphs(PvrTaFifoSnapshot& state) {
    detach_pvr_material_graphs(state.accelerator);
    state.active_material = clone_pvr_material(state.active_material);
}

} // namespace

PvrRenderJobError::PvrRenderJobError(const PvrRenderError error,
                                     std::string ta_packet_class,
                                     std::string detail)
    : std::runtime_error(std::move(detail)), error_(error),
      ta_packet_class_(std::move(ta_packet_class)) {}

PvrRenderError PvrRenderJobError::error() const noexcept {
    return error_;
}

const std::string& PvrRenderJobError::ta_packet_class() const noexcept {
    return ta_packet_class_;
}

PvrRenderFailed::PvrRenderFailed(PvrRenderFailure failure)
    : std::runtime_error("pvr-render-failed: " + failure.detail),
      failure_(std::move(failure)) {}

const PvrRenderFailure& PvrRenderFailed::failure() const noexcept {
    return failure_;
}

PvrRegisterFile::PvrRegisterFile(EventScheduler& scheduler,
                                 const PvrTiming timing,
                                 std::function<void()> render_observer,
                                 std::function<void(bool)> vblank_observer)
    : scheduler_(scheduler), timing_(timing), scheduler_lifetime_(scheduler.lifetime_token()),
      vblank_observer_(std::move(vblank_observer)) {
    if (timing_.guest_clock_hz == 0u || timing_.pixel_clock_hz == 0u)
        throw std::invalid_argument("PVR-SPG braucht positive Gast- und Pixeltakte.");
    set_render_observer(std::move(render_observer));
    initialize_register_defaults();
    reschedule_scanout();
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

PvrRegisterFile::~PvrRegisterFile() {
    if (scheduler_lifetime_.expired()) return;
    for (const auto& [event, job] : render_jobs_) {
        static_cast<void>(job);
        static_cast<void>(scheduler_.cancel(event));
    }
    cancel_scan_events();
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::size_t PvrRegisterFile::index(const std::uint32_t offset) {
    if (offset >= pvr_register_size || (offset & 3u) != 0u) {
        throw std::out_of_range("Ungueltiger oder nicht ausgerichteter PVR-Registeroffset.");
    }
    return offset / 4u;
}

std::uint32_t PvrRegisterFile::read(const std::uint32_t offset) const {
    if (offset == pvr_register::Id) {
        return pvr_id;
    }
    if (offset == pvr_register::Revision) {
        return pvr_revision;
    }
    if (offset == pvr_register::SpgStatus) {
        const auto load = registers_[index(pvr_register::SpgLoad)];
        const auto horizontal = static_cast<std::uint64_t>(load & 0x3FFu) + 1u;
        const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
        if (scan_frame_cycles_ == 0u || horizontal <= 1u || vertical <= 1u) return 0u;

        const auto elapsed = scheduler_.current_cycle() >= scan_epoch_cycle_
                                 ? scheduler_.current_cycle() - scan_epoch_cycle_
                                 : 0u;
        const auto frame_cycle = elapsed % scan_frame_cycles_;
        const auto total_pixels = horizontal * vertical;
        const auto frame_pixel = std::min<std::uint64_t>(
            total_pixels - 1u, frame_cycle * total_pixels / scan_frame_cycles_);
        const auto scanline = frame_pixel / horizontal;
        const auto horizontal_position = frame_pixel % horizontal;
        const auto inside_wrapped = [](const std::uint64_t position,
                                       const std::uint64_t start,
                                       const std::uint64_t end) {
            return start <= end ? position >= start && position < end
                                : position >= start || position < end;
        };
        const auto vblank = registers_[index(pvr_register::SpgVblank)];
        const auto blank_start = static_cast<std::uint64_t>(vblank & 0x3FFu) % vertical;
        const auto blank_end = static_cast<std::uint64_t>((vblank >> 16u) & 0x3FFu) % vertical;
        const auto hblank = registers_[index(pvr_register::SpgHblank)];
        const auto horizontal_blank_start =
            static_cast<std::uint64_t>(hblank & 0x3FFu) % horizontal;
        const auto horizontal_blank_end =
            static_cast<std::uint64_t>((hblank >> 16u) & 0x3FFu) % horizontal;
        const auto vertical_blank = inside_wrapped(scanline, blank_start, blank_end);
        const auto horizontal_blank = inside_wrapped(
            horizontal_position, horizontal_blank_start, horizontal_blank_end);
        const auto control = registers_[index(pvr_register::SpgControl)];
        const auto field = (control & 0x10u) != 0u
                               ? static_cast<std::uint32_t>((elapsed / scan_frame_cycles_) & 1u)
                               : static_cast<std::uint32_t>((control >> 5u) & 1u);
        const auto sync_width = registers_[index(pvr_register::SpgWidth)];
        const auto horizontal_sync = horizontal_position <= (sync_width & 0x7Fu);
        const auto video_forced_blank =
            (registers_[index(pvr_register::VideoControl)] & 0x8u) != 0u;
        return static_cast<std::uint32_t>(scanline) | (field << 10u) |
               ((vertical_blank || horizontal_blank || video_forced_blank) ? 1u << 11u : 0u) |
               (horizontal_sync ? 1u << 12u : 0u) |
               (vertical_blank ? 1u << 13u : 0u);
    }
    if (offset == pvr_register::FramebufferCurrentReadStart) {
        return registers_[index(field() != 0u ? pvr_register::FramebufferReadSof2
                                             : pvr_register::FramebufferReadSof1)] &
               0x00FFFFFCu;
    }
    return registers_[index(offset)];
}

PvrRegisterSnapshot PvrRegisterFile::snapshot() const {
    PvrRegisterSnapshot result;
    result.registers = registers_;
    result.registers[pvr_register::Id / 4u] = read(pvr_register::Id);
    result.registers[pvr_register::Revision / 4u] = read(pvr_register::Revision);
    result.registers[pvr_register::SpgStatus / 4u] = read(pvr_register::SpgStatus);
    result.registers[pvr_register::FramebufferCurrentReadStart / 4u] =
        read(pvr_register::FramebufferCurrentReadStart);
    result.framebuffer_read_control =
        result.registers[pvr_register::FramebufferReadControl / 4u];
    result.framebuffer_read_size =
        result.registers[pvr_register::FramebufferReadSize / 4u];
    result.framebuffer_read_sof1 =
        result.registers[pvr_register::FramebufferReadSof1 / 4u];
    result.framebuffer_read_sof2 =
        result.registers[pvr_register::FramebufferReadSof2 / 4u];
    result.framebuffer_write_control =
        result.registers[pvr_register::FramebufferWriteControl / 4u];
    result.framebuffer_write_sof1 =
        result.registers[pvr_register::FramebufferWriteSof1 / 4u];
    result.framebuffer_write_sof2 =
        result.registers[pvr_register::FramebufferWriteSof2 / 4u];
    result.video_control = result.registers[pvr_register::VideoControl / 4u];
    result.render_requests = render_requests_;
    result.render_completions = render_completions_;
    result.render_failures = render_failures_;
    result.render_overruns = render_overruns_;
    result.vblank_in = vblank_in_count_;
    result.vblank_out = vblank_out_count_;
    result.hblank = hblank_count_;
    result.resets = resets_;
    result.render_event_ids.reserve(render_jobs_.size());
    for (const auto& [event, job] : render_jobs_)
        if (job.published) result.render_event_ids.push_back(event);
    result.vblank_in_event = vblank_in_event_;
    result.vblank_out_event = vblank_out_event_;
    result.hblank_event = hblank_event_;
    result.vblank_in_event_rehydration_pending =
        vblank_in_event_rehydration_pending_;
    result.vblank_out_event_rehydration_pending =
        vblank_out_event_rehydration_pending_;
    result.hblank_event_rehydration_pending =
        hblank_event_rehydration_pending_;
    result.hblank_event_line = hblank_event_line_;
    result.scan_frame_cycles = scan_frame_cycles_;
    result.scan_epoch_cycle = scan_epoch_cycle_;
    result.timing = timing_;
    result.in_vblank = in_vblank_;
    result.field = field();
    result.next_render_generation = next_render_generation_;
    if (render_jobs_.size() == 1u && render_jobs_.begin()->second.published) {
        const auto& active_job = render_jobs_.begin()->second;
        result.active_render_request = active_job.request;
        result.active_render_generation = active_job.generation;
        result.active_render_start_cycle = active_job.start_cycle;
        result.active_render_payload_digest = active_job.payload_digest;
    }
    result.last_render_start_error = last_render_start_error_;
    result.last_render_failure = last_render_failure_;
    return result;
}

void PvrRegisterFile::validate_state_restore(
    const PvrRegisterSnapshot& state) const {
    if (state.timing.render_latency != timing_.render_latency ||
        state.timing.guest_clock_hz != timing_.guest_clock_hz ||
        state.timing.pixel_clock_hz != timing_.pixel_clock_hz ||
        state.timing.guest_clock_hz == 0u ||
        state.timing.pixel_clock_hz == 0u)
        throw std::invalid_argument(
            "PVR-Register-Handoff passt nicht zum Runtime-Taktvertrag.");
    if (state.read(pvr_register::Id) != pvr_id ||
        state.read(pvr_register::Revision) != pvr_revision)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt eine ungueltige Hardwarekennung.");
    if (state.framebuffer_read_control !=
            state.registers[pvr_register::FramebufferReadControl / 4u] ||
        state.framebuffer_read_size !=
            state.registers[pvr_register::FramebufferReadSize / 4u] ||
        state.framebuffer_read_sof1 !=
            state.registers[pvr_register::FramebufferReadSof1 / 4u] ||
        state.framebuffer_read_sof2 !=
            state.registers[pvr_register::FramebufferReadSof2 / 4u] ||
        state.framebuffer_write_control !=
            state.registers[pvr_register::FramebufferWriteControl / 4u] ||
        state.framebuffer_write_sof1 !=
            state.registers[pvr_register::FramebufferWriteSof1 / 4u] ||
        state.framebuffer_write_sof2 !=
            state.registers[pvr_register::FramebufferWriteSof2 / 4u] ||
        state.video_control !=
            state.registers[pvr_register::VideoControl / 4u])
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt inkonsistente Registeransichten.");
    if (!state.render_event_ids.empty() ||
        state.active_render_request != 0u ||
        state.active_render_generation != 0u ||
        state.active_render_start_cycle != 0u ||
        state.active_render_payload_digest != 0u)
        throw std::invalid_argument(
            "PVR-Register-Handoff enthaelt einen nicht rekonstruierbaren "
            "aktiven Renderjob.");
    if (state.render_requests !=
        state.render_completions + state.render_failures)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt inkonsistente Renderzaehler.");
    if (state.render_overruns > state.render_failures ||
        state.field > 1u)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt ungueltige Zaehler oder Feldbits.");
    if (state.last_render_start_error &&
        !valid_pvr_render_start_error(*state.last_render_start_error))
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt einen ungueltigen Startfehler.");
    if (state.last_render_failure &&
        (!valid_pvr_render_error(state.last_render_failure->error) ||
         state.last_render_failure->request == 0u ||
         state.last_render_failure->request > state.render_requests ||
         state.last_render_failure->generation == 0u))
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt einen ungueltigen Renderfehler.");
    if (state.vblank_in_event &&
        state.vblank_in_event_rehydration_pending)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt VBlank-In doppelt gebunden.");
    if (state.vblank_out_event &&
        state.vblank_out_event_rehydration_pending)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt VBlank-Out doppelt gebunden.");
    if (state.hblank_event &&
        state.hblank_event_rehydration_pending)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt HBlank doppelt gebunden.");
    const auto has_vblank_in =
        state.vblank_in_event.has_value() ||
        state.vblank_in_event_rehydration_pending;
    const auto has_vblank_out =
        state.vblank_out_event.has_value() ||
        state.vblank_out_event_rehydration_pending;
    const auto has_hblank =
        state.hblank_event.has_value() ||
        state.hblank_event_rehydration_pending;
    if (state.scan_frame_cycles == 0u &&
        (has_vblank_in || has_vblank_out || has_hblank))
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt Scanereignisse ohne Frameperiode.");
    if (has_hblank != state.hblank_event_line.has_value())
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt keinen eindeutigen HBlank-Kanal.");
    const auto vertical =
        ((state.registers[pvr_register::SpgLoad / 4u] >> 16u) & 0x3FFu) +
        1u;
    if (state.hblank_event_line &&
        *state.hblank_event_line >= vertical)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt eine ungueltige HBlank-Zeile.");
    if (state.next_render_generation != 0u &&
        state.next_render_generation <= state.render_completions)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt eine zuruecklaufende Generation.");
}

void PvrRegisterFile::restore_state_passive(PvrRegisterSnapshot state) {
    validate_state_restore(state);
    const auto needs_vblank_in =
        state.vblank_in_event.has_value() ||
        state.vblank_in_event_rehydration_pending;
    const auto needs_vblank_out =
        state.vblank_out_event.has_value() ||
        state.vblank_out_event_rehydration_pending;
    const auto needs_hblank =
        state.hblank_event.has_value() ||
        state.hblank_event_rehydration_pending;

    for (const auto& [event, job] : render_jobs_) {
        static_cast<void>(job);
        static_cast<void>(scheduler_.cancel(event));
    }
    render_jobs_.clear();
    cancel_scan_events();
    registers_ = std::move(state.registers);
    render_requests_ = state.render_requests;
    render_completions_ = state.render_completions;
    render_failures_ = state.render_failures;
    render_overruns_ = state.render_overruns;
    next_render_generation_ = state.next_render_generation;
    last_render_start_error_ = state.last_render_start_error;
    last_render_failure_ = std::move(state.last_render_failure);
    resets_ = state.resets;
    vblank_in_count_ = state.vblank_in;
    vblank_out_count_ = state.vblank_out;
    hblank_count_ = state.hblank;
    scan_frame_cycles_ = state.scan_frame_cycles;
    scan_epoch_cycle_ = state.scan_epoch_cycle;
    in_vblank_ = state.in_vblank;
    field_ = state.field;
    vblank_in_event_rehydration_pending_ = needs_vblank_in;
    vblank_out_event_rehydration_pending_ = needs_vblank_out;
    hblank_event_rehydration_pending_ = needs_hblank;
    hblank_event_line_ = state.hblank_event_line;
}

SchedulerEventId PvrRegisterFile::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (token != dreamcast_pvr_scan_event_token_v1)
        throw std::invalid_argument(
            "PVR-Register-Handoff besitzt einen unbekannten Eventtoken.");
    if (guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "PVR-Scanereignis darf nicht in der Vergangenheit liegen.");
    if (channel == dreamcast_pvr_vblank_in_event_channel) {
        if (!vblank_in_event_rehydration_pending_ || vblank_in_event_)
            throw std::logic_error(
                "PVR-Register-Handoff erwartet kein VBlank-In-Ereignis.");
        const auto event = scheduler_.schedule_at(
            guest_cycle,
            [this](const auto id, const auto) {
                handle_scan_event(id, true);
            },
            SchedulerEventKind::PvrVblankIn);
        vblank_in_event_ = event;
        vblank_in_event_rehydration_pending_ = false;
        return event;
    }
    if (channel == dreamcast_pvr_vblank_out_event_channel) {
        if (!vblank_out_event_rehydration_pending_ || vblank_out_event_)
            throw std::logic_error(
                "PVR-Register-Handoff erwartet kein VBlank-Out-Ereignis.");
        const auto event = scheduler_.schedule_at(
            guest_cycle,
            [this](const auto id, const auto) {
                handle_scan_event(id, false);
            },
            SchedulerEventKind::PvrVblankOut);
        vblank_out_event_ = event;
        vblank_out_event_rehydration_pending_ = false;
        return event;
    }
    if (channel == dreamcast_pvr_hblank_event_channel) {
        if (!hblank_event_rehydration_pending_ || hblank_event_ ||
            !hblank_event_line_)
            throw std::logic_error(
                "PVR-Register-Handoff erwartet kein HBlank-Ereignis.");
        const auto line = *hblank_event_line_;
        const auto event = scheduler_.schedule_at(
            guest_cycle,
            [this, line](const auto id, const auto) {
                handle_hblank_event(id, line);
            },
            SchedulerEventKind::PvrHblank);
        hblank_event_ = event;
        hblank_event_rehydration_pending_ = false;
        return event;
    }
    throw std::invalid_argument(
        "PVR-Register-Handoff besitzt einen unbekannten Eventkanal.");
}

bool PvrRegisterFile::event_rehydration_pending(
    const std::uint32_t channel) const noexcept {
    if (channel == dreamcast_pvr_vblank_in_event_channel)
        return vblank_in_event_rehydration_pending_;
    if (channel == dreamcast_pvr_vblank_out_event_channel)
        return vblank_out_event_rehydration_pending_;
    if (channel == dreamcast_pvr_hblank_event_channel)
        return hblank_event_rehydration_pending_;
    return false;
}

void PvrRegisterFile::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == pvr_register::Id || offset == pvr_register::Revision ||
        offset == pvr_register::SpgStatus || offset == pvr_register::TaNextOpb ||
        offset == pvr_register::TaIspCurrent ||
        offset == pvr_register::FramebufferCurrentReadStart) {
        throw std::runtime_error("Read-only-PVR-Register ist nicht beschreibbar.");
    }
    if (offset == pvr_register::SoftReset) {
        const auto requested = value & 0x7u;
        registers_[index(pvr_register::SoftReset)] = requested;
        if (requested == 0u) return;
        ++resets_;

        // SOFTRESET drives three independent reset inputs. It is not a power-on reset of the
        // register bank or the scan generator.
        if ((requested & 0x2u) != 0u) {
            for (const auto& [event, job] : render_jobs_) {
                static_cast<void>(job);
                static_cast<void>(scheduler_.cancel(event));
            }
            render_jobs_.clear();
        }
        if ((requested & 0x1u) != 0u) {
            registers_[index(pvr_register::TaNextOpb)] = 0u;
            registers_[index(pvr_register::TaIspCurrent)] = 0u;
            if (ta_reset_observer_) ta_reset_observer_();
        }
        return;
    }
    if (offset == pvr_register::StartRender) {
        const auto request = ++render_requests_;
        if (!render_jobs_.empty()) {
            ++render_overruns_;
            ++render_failures_;
            last_render_start_error_ = PvrRenderStartError::Busy;
            if (render_overrun_observer_) {
                try {
                    render_overrun_observer_();
                } catch (...) {
                }
            }
            return;
        }
        if (next_render_generation_ == 0u) {
            ++render_failures_;
            last_render_start_error_ = PvrRenderStartError::GenerationExhausted;
            return;
        }
        const auto generation = next_render_generation_;
        next_render_generation_ =
            generation == std::numeric_limits<std::uint64_t>::max()
                ? 0u
                : generation + 1u;
        const auto start_cycle = scheduler_.current_cycle();
        PreparedRenderJob prepared_job{render_result_observer_, {}, 0u};
        if (render_job_factory_) {
            try {
                prepared_job =
                    render_job_factory_(snapshot(), request, generation, start_cycle);
            } catch (...) {
                ++render_failures_;
                last_render_start_error_ = PvrRenderStartError::CaptureFailed;
                return;
            }
        }
        std::optional<SchedulerEventId> scheduled_event;
        try {
            const auto event = scheduler_.schedule_after(
                timing_.render_latency,
                [this](const auto event_id, const auto) { complete_render(event_id); },
                SchedulerEventKind::PvrRender);
            scheduled_event = event;
            const auto [job_position, job_inserted] = render_jobs_.emplace(
                event,
                FrozenRenderJob{
                    request,
                    generation,
                    start_cycle,
                    prepared_job.payload_digest,
                    std::move(prepared_job.execute),
                    false});
            if (!job_inserted)
                throw std::logic_error("PVR-Renderauftrag-ID ist bereits aktiv.");
            static_cast<void>(job_position);
        } catch (...) {
            if (scheduled_event) {
                static_cast<void>(scheduler_.cancel(*scheduled_event));
                render_jobs_.erase(*scheduled_event);
            }
            ++render_failures_;
            last_render_start_error_ = PvrRenderStartError::SchedulerFailure;
            return;
        }
        try {
            if (prepared_job.commit) prepared_job.commit();
            const auto job = render_jobs_.find(*scheduled_event);
            if (job == render_jobs_.end())
                throw std::logic_error("Vorbereiteter PVR-Renderauftrag ging verloren.");
            job->second.published = true;
        } catch (...) {
            if (scheduled_event) {
                static_cast<void>(scheduler_.cancel(*scheduled_event));
                render_jobs_.erase(*scheduled_event);
            }
            ++render_failures_;
            last_render_start_error_ = PvrRenderStartError::CaptureFailed;
            return;
        }
        last_render_start_error_.reset();
        return;
    }
    if (offset == pvr_register::TaInit) {
        if ((value & 0x80000000u) != 0u) {
            registers_[index(pvr_register::TaNextOpb)] =
                registers_[index(pvr_register::TaNextOpbInit)];
            registers_[index(pvr_register::TaIspCurrent)] =
                registers_[index(pvr_register::TaIspBase)];
            if (ta_reset_observer_) ta_reset_observer_();
        }
        return;
    }
    if (offset == pvr_register::TaListContinue) {
        registers_[index(pvr_register::TaNextOpb)] =
            registers_[index(pvr_register::TaObjectListBase)];
        if (ta_continue_observer_) ta_continue_observer_();
        return;
    }
    if (offset == pvr_register::TaObjectListBase || offset == pvr_register::TaIspBase ||
        offset == pvr_register::TaObjectListLimit || offset == pvr_register::TaIspLimit ||
        offset == pvr_register::TaNextOpbInit) {
        registers_[index(offset)] = value & 0x007FFFFCu;
        return;
    }
    if (offset == pvr_register::FramebufferWriteSof1 ||
        offset == pvr_register::FramebufferWriteSof2) {
        // Bit 24 distinguishes a render-to-texture target from a display framebuffer.
        // Preserve it in the guest-visible register and reduce to the physical VRAM
        // address only at the renderer's memory access boundary.
        registers_[index(offset)] = value & 0x01FFFFFCu;
    } else if (offset == pvr_register::ParameterBase || offset == pvr_register::RegionBase ||
               offset == pvr_register::FramebufferReadSof1 ||
               offset == pvr_register::FramebufferReadSof2) {
        registers_[index(offset)] = value & 0x00FFFFFCu;
    } else if (offset == pvr_register::SpgTriggerPosition ||
               offset == pvr_register::SpgVblankInterrupt ||
               offset == pvr_register::SpgHblank || offset == pvr_register::SpgLoad ||
               offset == pvr_register::SpgVblank) {
        registers_[index(offset)] = value & 0x03FF03FFu;
    } else if (offset == pvr_register::SpgHblankInterrupt) {
        if (((value >> 12u) & 3u) == 3u)
            throw std::invalid_argument("PVR-HBlank-Interruptmodus 3 ist reserviert.");
        registers_[index(offset)] = value & 0x03FF33FFu;
    } else if (offset == pvr_register::SpgControl) {
        registers_[index(offset)] = value & 0x000003FFu;
    } else if (offset == pvr_register::ScalerControl) {
        registers_[index(offset)] = value & 0x0007FFFFu;
    } else if (offset == pvr_register::PaletteConfig) {
        registers_[index(offset)] = value & 0x3u;
    } else if (offset == pvr_register::PunchThroughAlphaReference) {
        registers_[index(offset)] = value & 0xFFu;
    } else {
        registers_[index(offset)] = value;
    }
    if (offset == pvr_register::FramebufferReadControl ||
        offset == pvr_register::SpgControl || offset == pvr_register::SpgLoad ||
        offset == pvr_register::SpgVblank ||
        offset == pvr_register::SpgVblankInterrupt ||
        offset == pvr_register::SpgHblankInterrupt ||
        offset == pvr_register::VideoControl)
        reschedule_scanout();
}

void PvrRegisterFile::initialize_register_defaults() noexcept {
    registers_.fill(0u);
    registers_[index(pvr_register::SoftReset)] = 0x00000007u;
    registers_[index(pvr_register::SpgHblankInterrupt)] = 0x031D0000u;
    registers_[index(pvr_register::SpgVblankInterrupt)] = 0x00150104u;
    registers_[index(pvr_register::ParameterConfig)] = 0x0007DF77u;
    registers_[index(pvr_register::HalfOffset)] = 0x00000007u;
    registers_[index(pvr_register::IspFeedConfig)] = 0x00402000u;
    registers_[index(pvr_register::SdramRefresh)] = 0x00000020u;
    registers_[index(pvr_register::SdramArbitration)] = 0x0000001Fu;
    registers_[index(pvr_register::SdramConfig)] = 0x15F28997u;
    registers_[index(pvr_register::SpgHblank)] = 0x007E0345u;
    registers_[index(pvr_register::SpgLoad)] = 0x01060359u;
    registers_[index(pvr_register::SpgVblank)] = 0x01500104u;
    registers_[index(pvr_register::SpgWidth)] = 0x07F1933Fu;
    registers_[index(pvr_register::VideoControl)] = 0x00000108u;
    registers_[index(pvr_register::VideoStartX)] = 0x0000009Du;
    registers_[index(pvr_register::VideoStartY)] = 0x00150015u;
    registers_[index(pvr_register::ScalerControl)] = 0x00000400u;
    registers_[index(pvr_register::FramebufferBurstControl)] = 0x00090639u;
    registers_[index(pvr_register::PunchThroughAlphaReference)] = 0x000000FFu;
}

void PvrRegisterFile::reset() {
    for (const auto& [event, job] : render_jobs_) {
        static_cast<void>(job);
        static_cast<void>(scheduler_.cancel(event));
    }
    render_jobs_.clear();
    cancel_scan_events();
    initialize_register_defaults();
    field_ = 0u;
    scan_frame_cycles_ = 0u;
    scan_epoch_cycle_ = scheduler_.current_cycle();
    in_vblank_ = false;
    ++resets_;
    reschedule_scanout();
    // Keep the PVR internally schedulable even when an externally supplied TA
    // reset observer reports a host-side lifecycle failure. The observer may
    // still propagate its exception, but it cannot strand an already committed
    // full reset without VBlank/HBlank events.
    if (ta_reset_observer_) ta_reset_observer_();
}

void PvrRegisterFile::complete_render(const SchedulerEventId event_id) {
    const auto found = render_jobs_.find(event_id);
    if (found == render_jobs_.end() || !found->second.published)
        throw std::logic_error(
            "PVR-Rendercompletion besitzt keinen veroeffentlichten Frozen Job.");
    auto frozen_job = std::move(found->second);
    render_jobs_.erase(found);
    const auto fail = [&](const PvrRenderError error,
                          std::string ta_packet_class,
                          std::string detail) -> void {
        ++render_failures_;
        last_render_failure_ = PvrRenderFailure{
            frozen_job.request,
            frozen_job.generation,
            error,
            std::move(ta_packet_class),
            frozen_job.payload_digest,
            scheduler_.current_cycle(),
            std::move(detail),
        };
        throw PvrRenderFailed(*last_render_failure_);
    };
    if (!frozen_job.execute) {
        fail(PvrRenderError::InternalLifecycle,
             "none",
             "PVR-Renderauftrag besitzt keinen Ausfuehrungspfad.");
    }
    auto result = PvrRenderResult::Failed;
    try {
        result = frozen_job.execute();
    } catch (const PvrRenderJobError& error) {
        fail(error.error(), error.ta_packet_class(), error.what());
    } catch (const std::exception& error) {
        fail(PvrRenderError::InternalLifecycle, "none", error.what());
    } catch (...) {
        fail(PvrRenderError::InternalLifecycle,
             "none",
             "Unbekannte Exception im PVR-Renderauftrag.");
    }
    if (result != PvrRenderResult::Success)
        fail(PvrRenderError::UnsupportedFeature,
             "none",
             "PVR-Renderauftrag wurde ohne Fehlerdetail abgelehnt.");
    ++render_completions_;
}

void PvrRegisterFile::handle_scheduler_reset() {
    render_jobs_.clear();
    vblank_in_event_.reset();
    vblank_out_event_.reset();
    hblank_event_.reset();
    vblank_in_event_rehydration_pending_ = false;
    vblank_out_event_rehydration_pending_ = false;
    hblank_event_rehydration_pending_ = false;
    hblank_event_line_.reset();
    reschedule_scanout();
}

std::uint64_t PvrRegisterFile::render_request_count() const noexcept {
    return render_requests_;
}
std::uint64_t PvrRegisterFile::render_completion_count() const noexcept {
    return render_completions_;
}
std::uint64_t PvrRegisterFile::render_failure_count() const noexcept {
    return render_failures_;
}
const std::optional<PvrRenderFailure>& PvrRegisterFile::last_render_failure() const noexcept {
    return last_render_failure_;
}
std::uint64_t PvrRegisterFile::render_overrun_count() const noexcept {
    return render_overruns_;
}
std::uint64_t PvrRegisterFile::reset_count() const noexcept {
    return resets_;
}
std::uint64_t PvrRegisterFile::vblank_in_count() const noexcept {
    return vblank_in_count_;
}
std::uint64_t PvrRegisterFile::vblank_out_count() const noexcept {
    return vblank_out_count_;
}
std::uint64_t PvrRegisterFile::hblank_count() const noexcept {
    return hblank_count_;
}
bool PvrRegisterFile::in_vblank() const noexcept {
    return in_vblank_;
}
std::uint32_t PvrRegisterFile::field() const noexcept {
    if (scan_frame_cycles_ == 0u) return field_;
    const auto control = registers_[index(pvr_register::SpgControl)];
    if ((control & 0x10u) == 0u) return (control >> 5u) & 1u;
    const auto elapsed = scheduler_.current_cycle() >= scan_epoch_cycle_
                             ? scheduler_.current_cycle() - scan_epoch_cycle_
                             : 0u;
    return static_cast<std::uint32_t>((elapsed / scan_frame_cycles_) & 1u);
}
void PvrRegisterFile::set_render_observer(std::function<void()> observer) {
    if (!observer) {
        render_result_observer_ = {};
        return;
    }
    render_result_observer_ =
        [observer = std::move(observer)]() mutable {
            observer();
            return PvrRenderResult::Success;
        };
}
void PvrRegisterFile::set_render_result_observer(
    std::function<PvrRenderResult()> observer) {
    render_result_observer_ = std::move(observer);
}
void PvrRegisterFile::set_render_job_factory(RenderJobFactory factory) {
    render_job_factory_ = std::move(factory);
}
void PvrRegisterFile::set_render_overrun_observer(std::function<void()> observer) {
    render_overrun_observer_ = std::move(observer);
}
void PvrRegisterFile::set_vblank_observer(std::function<void(bool)> observer) {
    vblank_observer_ = std::move(observer);
}
void PvrRegisterFile::set_hblank_observer(std::function<void()> observer) {
    hblank_observer_ = std::move(observer);
}
void PvrRegisterFile::set_ta_reset_observer(std::function<void()> observer) {
    ta_reset_observer_ = std::move(observer);
}
void PvrRegisterFile::set_ta_continue_observer(std::function<void()> observer) {
    ta_continue_observer_ = std::move(observer);
}
void PvrRegisterFile::record_ta_packet(const std::uint32_t bytes) {
    auto& position = registers_[index(pvr_register::TaVertexBufferPosition)];
    const auto end = registers_[index(pvr_register::TaVertexBufferEnd)];
    const auto next = static_cast<std::uint64_t>(position) + bytes;
    position = end != 0u && next > end ? end : static_cast<std::uint32_t>(next);
}

void PvrRegisterFile::cancel_scan_events() noexcept {
    if (vblank_in_event_) static_cast<void>(scheduler_.cancel(*vblank_in_event_));
    if (vblank_out_event_) static_cast<void>(scheduler_.cancel(*vblank_out_event_));
    if (hblank_event_) static_cast<void>(scheduler_.cancel(*hblank_event_));
    vblank_in_event_.reset();
    vblank_out_event_.reset();
    hblank_event_.reset();
    vblank_in_event_rehydration_pending_ = false;
    vblank_out_event_rehydration_pending_ = false;
    hblank_event_rehydration_pending_ = false;
    hblank_event_line_.reset();
}

void PvrRegisterFile::reschedule_scanout(const bool derive_current_vblank) {
    cancel_scan_events();
    const auto current_cycle = scheduler_.current_cycle();
    const auto preserve_scan_phase =
        scan_frame_cycles_ != 0u && current_cycle >= scan_epoch_cycle_;
    const auto load = registers_[index(pvr_register::SpgLoad)];
    const auto horizontal = static_cast<std::uint64_t>(load & 0x3FFu) + 1u;
    const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
    scan_frame_cycles_ = 0u;
    if (!preserve_scan_phase) scan_epoch_cycle_ = current_cycle;
    in_vblank_ = false;
    if (horizontal <= 1u || vertical <= 1u) return;
    const auto pixels = horizontal * vertical;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / timing_.guest_clock_hz)
        throw std::out_of_range("PVR-SPG-Frameperiode laeuft ueber.");
    const auto base_cycles = pixels * timing_.guest_clock_hz;
    const auto vclk_div =
        (registers_[index(pvr_register::FramebufferReadControl)] & (1u << 23u)) != 0u;
    const auto interlaced =
        (registers_[index(pvr_register::SpgControl)] & (1u << 4u)) != 0u;
    const auto ceil_div = [](const std::uint64_t numerator,
                             const std::uint64_t denominator) {
        return numerator / denominator + (numerator % denominator != 0u ? 1u : 0u);
    };
    if (!vclk_div && !interlaced) {
        if (base_cycles > std::numeric_limits<std::uint64_t>::max() / 2u)
            throw std::out_of_range("PVR-SPG-Frameperiode laeuft ueber.");
        scan_frame_cycles_ = ceil_div(base_cycles * 2u, timing_.pixel_clock_hz);
    } else if (vclk_div && interlaced) {
        // ceil(base_cycles / (pixel_clock * 2)) without overflowing the denominator.
        const auto quotient = base_cycles / timing_.pixel_clock_hz;
        const auto remainder = base_cycles % timing_.pixel_clock_hz;
        scan_frame_cycles_ = quotient / 2u +
                             ((quotient & 1u) != 0u || remainder != 0u ? 1u : 0u);
    } else {
        scan_frame_cycles_ = ceil_div(base_cycles, timing_.pixel_clock_hz);
    }
    scan_frame_cycles_ = std::max<std::uint64_t>(1u, scan_frame_cycles_);
    const auto vblank = registers_[index(pvr_register::SpgVblankInterrupt)];
    const auto start = vblank & 0x3FFu;
    const auto end = (vblank >> 16u) & 0x3FFu;
    if (derive_current_vblank && start < vertical && end < vertical) {
        const auto elapsed = current_cycle - scan_epoch_cycle_;
        const auto frame_cycle = elapsed % scan_frame_cycles_;
        const auto scanline = std::min<std::uint64_t>(
            vertical - 1u, frame_cycle * vertical / scan_frame_cycles_);
        in_vblank_ = start <= end ? scanline >= start && scanline < end
                                  : scanline >= start || scanline < end;
    }
    if (start < vertical) schedule_scan_event(start, true);
    if (end < vertical) schedule_scan_event(end, false);
    const auto hblank_interrupt = registers_[index(pvr_register::SpgHblankInterrupt)];
    const auto hblank_mode = (hblank_interrupt >> 12u) & 3u;
    if (hblank_mode == 0u) {
        const auto line_compare = hblank_interrupt & 0x3FFu;
        if (line_compare < vertical) schedule_hblank_event(line_compare);
    } else if (hblank_mode == 1u) {
        const auto line_compare = hblank_interrupt & 0x3FFu;
        if (line_compare < vertical) schedule_hblank_event(line_compare);
    } else if (hblank_mode == 2u) {
        schedule_hblank_event(0u);
    }
}

void PvrRegisterFile::schedule_scan_event(const std::uint32_t line, const bool entering) {
    const auto load = registers_[index(pvr_register::SpgLoad)];
    const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
    const auto target_frame_cycle =
        line == 0u
            ? 0u
            : std::max<std::uint64_t>(1u, scan_frame_cycles_ * line / vertical);
    const auto elapsed = scheduler_.current_cycle() >= scan_epoch_cycle_
                             ? scheduler_.current_cycle() - scan_epoch_cycle_
                             : 0u;
    const auto current_frame_cycle = elapsed % scan_frame_cycles_;
    const auto delay = target_frame_cycle > current_frame_cycle
                           ? target_frame_cycle - current_frame_cycle
                           : scan_frame_cycles_ - current_frame_cycle + target_frame_cycle;
    const auto event = scheduler_.schedule_after(
        delay,
        [this, entering](const auto id, const auto) { handle_scan_event(id, entering); },
        entering ? SchedulerEventKind::PvrVblankIn : SchedulerEventKind::PvrVblankOut);
    (entering ? vblank_in_event_ : vblank_out_event_) = event;
}

void PvrRegisterFile::handle_scan_event(const SchedulerEventId event_id, const bool entering) {
    auto& slot = entering ? vblank_in_event_ : vblank_out_event_;
    if (!slot || *slot != event_id)
        throw std::logic_error("PVR-SPG-Completion besitzt kein aktives Ereignis.");
    slot.reset();
    in_vblank_ = entering;
    if (entering) {
        ++vblank_in_count_;
    } else {
        ++vblank_out_count_;
    }
    if (vblank_observer_) vblank_observer_(entering);
    if (scan_frame_cycles_ != 0u) {
        const auto next = scheduler_.schedule_after(
            scan_frame_cycles_,
            [this, entering](const auto id, const auto) { handle_scan_event(id, entering); },
            entering ? SchedulerEventKind::PvrVblankIn : SchedulerEventKind::PvrVblankOut);
        slot = next;
    }
}

void PvrRegisterFile::schedule_hblank_event(const std::uint32_t line) {
    const auto load = registers_[index(pvr_register::SpgLoad)];
    const auto horizontal = static_cast<std::uint64_t>(load & 0x3FFu) + 1u;
    const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
    const auto total_pixels = horizontal * vertical;
    const auto horizontal_position = static_cast<std::uint64_t>(
        (registers_[index(pvr_register::SpgHblankInterrupt)] >> 16u) & 0x3FFu) %
                                     horizontal;
    const auto pixel = static_cast<std::uint64_t>(line) * horizontal + horizontal_position;
    const auto target_frame_cycle = std::max<std::uint64_t>(
        1u, (scan_frame_cycles_ * pixel + total_pixels - 1u) / total_pixels);
    const auto elapsed = scheduler_.current_cycle() >= scan_epoch_cycle_
                             ? scheduler_.current_cycle() - scan_epoch_cycle_
                             : 0u;
    const auto current_frame_cycle = elapsed % scan_frame_cycles_;
    const auto delay = target_frame_cycle > current_frame_cycle
                           ? target_frame_cycle - current_frame_cycle
                           : scan_frame_cycles_ - current_frame_cycle + target_frame_cycle;
    hblank_event_ = scheduler_.schedule_after(
        delay,
        [this, line](const auto id, const auto) { handle_hblank_event(id, line); },
        SchedulerEventKind::PvrHblank);
    hblank_event_line_ = line;
}

void PvrRegisterFile::handle_hblank_event(const SchedulerEventId event_id,
                                          const std::uint32_t line) {
    if (!hblank_event_ || *hblank_event_ != event_id)
        throw std::logic_error("PVR-HBlank-Completion besitzt kein aktives Ereignis.");
    hblank_event_.reset();
    hblank_event_line_.reset();
    ++hblank_count_;
    if (hblank_observer_) hblank_observer_();
    const auto mode = (registers_[index(pvr_register::SpgHblankInterrupt)] >> 12u) & 3u;
    if (scan_frame_cycles_ == 0u) return;
    std::uint64_t delay = scan_frame_cycles_;
    if (mode == 1u || mode == 2u) {
        const auto vertical =
            static_cast<std::uint64_t>((registers_[index(pvr_register::SpgLoad)] >> 16u) &
                                       0x3FFu) +
            1u;
        const auto lines = mode == 1u
                               ? static_cast<std::uint64_t>(
                                     registers_[index(pvr_register::SpgHblankInterrupt)] &
                                     0x3FFu) +
                                     1u
                               : 1u;
        delay = std::max<std::uint64_t>(
            1u, (scan_frame_cycles_ * lines + vertical - 1u) / vertical);
    } else if (mode != 0u) {
        return;
    }
    hblank_event_ = scheduler_.schedule_after(
        delay,
        [this, line](const auto id, const auto) { handle_hblank_event(id, line); },
        SchedulerEventKind::PvrHblank);
    hblank_event_line_ = line;
}

void configure_dreamcast_video(PvrRegisterFile& registers, const DreamcastVideoMode mode) {
    struct Profile {
        std::uint32_t load;
        std::uint32_t hblank;
        std::uint32_t vblank;
        std::uint32_t hblank_interrupt;
        std::uint32_t vblank_interrupt;
        std::uint32_t width;
        std::uint32_t control;
        std::uint32_t start_x;
        std::uint32_t start_y;
    };
    constexpr std::array profiles{
        Profile{0x01060359u, 0x007E0345u, 0x00120102u, 0x03450000u, 0x00150104u,
                0x03F1933Fu,
                0x00000140u, 0x000000A4u, 0x00120011u},
        Profile{0x020C0359u, 0x007E0345u, 0x00240204u, 0x03450000u, 0x00150104u,
                0x07D6C63Fu,
                0x00000150u, 0x000000A4u, 0x00120012u},
        Profile{0x0138035Fu, 0x008D034Bu, 0x002C026Cu, 0x034B0000u, 0x00150136u,
                0x07F1F53Fu,
                0x00000180u, 0x000000AEu, 0x002E002Eu},
        Profile{0x0270035Fu, 0x008D034Bu, 0x002C026Cu, 0x034B0000u, 0x00150136u,
                0x07D6A53Fu,
                0x00000190u, 0x000000AEu, 0x002E002Du},
        Profile{0x020C0359u, 0x007E0345u, 0x00280208u, 0x03450000u, 0x00150208u,
                0x03F1933Fu,
                0x00000100u, 0x000000A8u, 0x00280028u},
    };
    const auto selected = static_cast<std::size_t>(mode);
    if (selected >= profiles.size()) throw std::invalid_argument("Unbekannter Dreamcast-Videomodus.");
    const auto& profile = profiles[selected];
    registers.write(pvr_register::SpgHblankInterrupt, profile.hblank_interrupt);
    registers.write(pvr_register::SpgVblankInterrupt, profile.vblank_interrupt);
    registers.write(pvr_register::SpgHblank, profile.hblank);
    registers.write(pvr_register::SpgVblank, profile.vblank);
    registers.write(pvr_register::SpgWidth, profile.width);
    registers.write(pvr_register::VideoStartX, profile.start_x);
    registers.write(pvr_register::VideoStartY, profile.start_y);
    registers.write(pvr_register::VideoControl, 0x00160000u);
    registers.write(pvr_register::ScalerControl, 0x00000400u);
    registers.write(pvr_register::SpgLoad, profile.load);
    registers.write(pvr_register::SpgControl, profile.control);
}

namespace {
std::uint8_t expand5(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 3u) | (value >> 2u));
}
std::uint8_t expand6(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 2u) | (value >> 4u));
}

std::size_t
checked_multiply(const std::size_t left, const std::size_t right, const char* description) {
    if (right != 0u && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::out_of_range(description);
    }
    return left * right;
}

std::size_t checked_add(const std::size_t left, const std::size_t right, const char* description) {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        throw std::out_of_range(description);
    }
    return left + right;
}

std::size_t bytes_per_pixel(const PvrFramebufferFormat format) {
    if (format == PvrFramebufferFormat::Rgb888) return 3u;
    if (format == PvrFramebufferFormat::Rgb0888) return 4u;
    return 2u;
}

std::size_t render_bytes_per_pixel(const std::uint32_t pack_mode) {
    switch (pack_mode) {
    case 0u:
    case 1u:
    case 2u:
    case 3u:
        return 2u;
    case 5u:
    case 6u:
        return 4u;
    default:
        throw std::invalid_argument("PVR-Framebuffer-Packmodus 4 oder 7 ist reserviert.");
    }
}

void notify_pvr_vram_write(Memory* const memory,
                           const GuestMemoryAccessOrigin origin,
                           LinearMemoryDevice& vram,
                           const std::uint32_t physical_address,
                           const std::uint32_t linear_offset,
                           const MemoryAccessWidth width,
                           const std::uint32_t value,
                           const bool bytes_changed) noexcept {
    if (memory == nullptr || !memory->has_guest_memory_access_sink()) return;

    GuestMemoryAccessEvent event;
    event.operation = MemoryAccessOperation::Write;
    event.access_origin = origin;
    event.virtual_address = physical_address;
    event.physical_address = physical_address;
    event.width = width;
    event.value = value;
    event.size = static_cast<std::size_t>(width);
    event.write_source = CodeWriteSource::Fallback;
    event.scalar_value_valid = true;
    event.bytes_changed = bytes_changed;
    event.linear_backing = &vram;
    event.linear_offset = linear_offset;
    event.linear_size = event.size;
    event.linear_contiguous = true;
    event.linear_byte_count = static_cast<std::uint8_t>(event.size);
    for (std::uint8_t index = 0u; index < event.linear_byte_count; ++index)
        event.linear_byte_offsets[index] = linear_offset + index;
    memory->notify_external_guest_memory_access(event);
}

bool write_render_pixel(LinearMemoryDevice& vram,
                        Memory* const guest_memory_access_memory,
                        const std::uint32_t offset,
                        const std::uint32_t pack_mode,
                        const std::uint8_t alpha,
                        const std::uint8_t red,
                        const std::uint8_t green,
                        const std::uint8_t blue) {
    const auto logical_offset =
        offset & static_cast<std::uint32_t>(dreamcast_vram_size - 1u);
    const auto backing = dreamcast_vram_32bit_to_linear_offset(
        logical_offset);
    const auto physical_address =
        dreamcast_vram_32bit_physical_bases.front() + logical_offset;
    const auto write16 = [&](const std::uint16_t value) {
        const bool changed = vram.read_u16(backing) != value;
        vram.write_u16(backing, value);
        notify_pvr_vram_write(guest_memory_access_memory,
                              GuestMemoryAccessOrigin::PvrRender,
                              vram,
                              physical_address,
                              backing,
                              MemoryAccessWidth::Halfword,
                              value,
                              changed);
        return changed;
    };
    const auto write32 = [&](const std::uint32_t value) {
        const bool changed = vram.read_u32(backing) != value;
        vram.write_u32(backing, value);
        notify_pvr_vram_write(guest_memory_access_memory,
                              GuestMemoryAccessOrigin::PvrRender,
                              vram,
                              physical_address,
                              backing,
                              MemoryAccessWidth::Word,
                              value,
                              changed);
        return changed;
    };
    switch (pack_mode) {
    case 0u:
        return write16(static_cast<std::uint16_t>(((red >> 3u) << 10u) |
                                                  ((green >> 3u) << 5u) | (blue >> 3u)));
    case 1u:
        return write16(static_cast<std::uint16_t>(((red >> 3u) << 11u) |
                                                  ((green >> 2u) << 5u) | (blue >> 3u)));
    case 2u:
        return write16(static_cast<std::uint16_t>(((alpha >> 4u) << 12u) |
                                                  ((red >> 4u) << 8u) |
                                                  ((green >> 4u) << 4u) | (blue >> 4u)));
    case 3u:
        return write16(static_cast<std::uint16_t>(((alpha >= 0x80u) ? 0x8000u : 0u) |
                                                  ((red >> 3u) << 10u) |
                                                  ((green >> 3u) << 5u) | (blue >> 3u)));
    case 5u:
        return write32(static_cast<std::uint32_t>(red) << 16u |
                       static_cast<std::uint32_t>(green) << 8u | blue);
    case 6u:
        return write32(static_cast<std::uint32_t>(alpha) << 24u |
                       static_cast<std::uint32_t>(red) << 16u |
                       static_cast<std::uint32_t>(green) << 8u | blue);
    default:
        throw std::invalid_argument("PVR-Framebuffer-Packmodus 4 oder 7 ist reserviert.");
    }
}

struct Rgba8 {
    std::uint8_t r = 0u;
    std::uint8_t g = 0u;
    std::uint8_t b = 0u;
    std::uint8_t a = 0xFFu;
};

std::uint8_t expand4(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 4u) | value);
}

Rgba8 decode_16bit_color(const std::uint16_t pixel, const std::uint8_t format) {
    if (format == 0u)
        return {expand5(static_cast<std::uint16_t>((pixel >> 10u) & 0x1Fu)),
                expand5(static_cast<std::uint16_t>((pixel >> 5u) & 0x1Fu)),
                expand5(static_cast<std::uint16_t>(pixel & 0x1Fu)),
                (pixel & 0x8000u) != 0u ? std::uint8_t{0xFFu} : std::uint8_t{0u}};
    if (format == 1u)
        return {expand5(static_cast<std::uint16_t>((pixel >> 11u) & 0x1Fu)),
                expand6(static_cast<std::uint16_t>((pixel >> 5u) & 0x3Fu)),
                expand5(static_cast<std::uint16_t>(pixel & 0x1Fu)),
                0xFFu};
    if (format == 2u)
        return {expand4(static_cast<std::uint16_t>((pixel >> 8u) & 0xFu)),
                expand4(static_cast<std::uint16_t>((pixel >> 4u) & 0xFu)),
                expand4(static_cast<std::uint16_t>(pixel & 0xFu)),
                expand4(static_cast<std::uint16_t>((pixel >> 12u) & 0xFu))};
    throw std::runtime_error("PVR-Texturformat ist im allgemeinen Renderer nicht integriert.");
}

Rgba8 decode_bump_texel(const std::uint16_t pixel) {
    return {static_cast<std::uint8_t>(pixel >> 8u),
            static_cast<std::uint8_t>(pixel),
            0u,
            0xFFu};
}

Rgba8 read_render_pixel(const LinearMemoryDevice& vram,
                        const std::uint32_t offset,
                        const std::uint32_t pack_mode) {
    const auto backing = dreamcast_vram_32bit_to_linear_offset(
        offset & static_cast<std::uint32_t>(dreamcast_vram_size - 1u));
    if (pack_mode <= 3u) {
        const auto pixel = vram.read_u16(backing);
        if (pack_mode == 0u)
            return {expand5(static_cast<std::uint16_t>((pixel >> 10u) & 0x1Fu)),
                    expand5(static_cast<std::uint16_t>((pixel >> 5u) & 0x1Fu)),
                    expand5(static_cast<std::uint16_t>(pixel & 0x1Fu)),
                    0xFFu};
        if (pack_mode == 1u) return decode_16bit_color(pixel, 1u);
        if (pack_mode == 2u) return decode_16bit_color(pixel, 2u);
        return decode_16bit_color(pixel, 0u);
    }
    if (pack_mode == 4u || pack_mode == 7u)
        throw std::invalid_argument("PVR-Framebuffer-Packmodus 4 oder 7 ist reserviert.");
    const auto pixel = vram.read_u32(backing);
    return {static_cast<std::uint8_t>(pixel >> 16u),
            static_cast<std::uint8_t>(pixel >> 8u),
            static_cast<std::uint8_t>(pixel),
            pack_mode == 6u ? static_cast<std::uint8_t>(pixel >> 24u) : std::uint8_t{0xFFu}};
}

std::uint32_t twiddle_bits(const std::uint32_t value) noexcept {
    std::uint32_t result = 0u;
    for (unsigned bit = 0u; bit < 10u; ++bit)
        result |= ((value >> bit) & 1u) << (bit * 2u);
    return result;
}

std::uint64_t texture_pixel_index(const PvrMaterial& material,
                                  const std::uint32_t x,
                                  const std::uint32_t y) {
    if (!material.texture_twiddled) {
        const auto stride = material.texture_stride_width == 0u
                                ? material.texture_width
                                : material.texture_stride_width;
        return static_cast<std::uint64_t>(y) * stride + x;
    }
    const auto minimum = std::min(material.texture_width, material.texture_height);
    const auto mask = minimum - 1u;
    return static_cast<std::uint64_t>(twiddle_bits(y & mask) |
                                      (twiddle_bits(x & mask) << 1u)) +
           static_cast<std::uint64_t>(x / minimum + y / minimum) * minimum * minimum;
}

float texture_coordinate(float value, const bool clamp, const bool flip) {
    if (clamp) return std::clamp(value, 0.0f, 1.0f);
    const auto tile = static_cast<std::int64_t>(std::floor(value));
    auto fraction = value - static_cast<float>(tile);
    if (flip && (tile & 1) != 0) fraction = 1.0f - fraction;
    return fraction;
}

std::uint64_t mipmap_level_offset(const PvrMaterial& material) {
    if (!material.texture_mipmapped) return 0u;
    if (material.texture_width != material.texture_height ||
        !std::has_single_bit(material.texture_width))
        throw std::runtime_error("PVR-Mipmaps brauchen eine quadratische Zweierpotenz-Geometrie.");
    // PowerVR stores mip levels smallest-first. These offsets are the start of
    // the largest requested level, including the hardware padding before 1x1.
    static constexpr std::array<std::uint32_t, 11u> offsets{
        0x00006u, 0x00008u, 0x00010u, 0x00030u, 0x000B0u, 0x002B0u,
        0x00AB0u, 0x02AB0u, 0x0AAB0u, 0x2AAB0u, 0xAAAB0u};
    const auto level = static_cast<std::size_t>(std::countr_zero(material.texture_width));
    const auto byte_offset = offsets.at(level);
    if (material.texture_vq) return byte_offset / 8u;
    if (material.texture_format == 5u) return byte_offset / 4u;
    if (material.texture_format == 6u) return byte_offset / 2u;
    return byte_offset;
}

template <typename RegisterView>
Rgba8 decode_palette_color(const RegisterView& registers, const std::uint32_t index) {
    if (index >= 1024u) throw std::out_of_range("PVR-Palettenindex liegt ausserhalb des Palette-RAM.");
    const auto value = registers.read(pvr_register::PaletteTableBase + index * 4u);
    const auto format = registers.read(pvr_register::PaletteConfig) & 3u;
    if (format <= 2u)
        return decode_16bit_color(static_cast<std::uint16_t>(value),
                                  static_cast<std::uint8_t>(format));
    return {static_cast<std::uint8_t>(value >> 16u),
            static_cast<std::uint8_t>(value >> 8u),
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 24u)};
}

Rgba8 decode_yuv_pair(const std::uint16_t first,
                      const std::uint16_t second,
                      const bool second_pixel) {
    const auto luminance = static_cast<int>((second_pixel ? second : first) >> 8u);
    const auto u = static_cast<int>(first & 0xFFu) - 128;
    const auto v = static_cast<int>(second & 0xFFu) - 128;
    const auto clamp = [](const int value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    return {clamp(luminance + 11 * v / 8),
            clamp(luminance - 11 * u / 32 - 11 * v / 16),
            clamp(luminance + 55 * u / 32),
            0xFFu};
}

Rgba8 decode_register_color(const std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 16u),
            static_cast<std::uint8_t>(value >> 8u),
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 24u)};
}

Rgba8 apply_fog(const Rgba8 source, const Rgba8 fog, const std::uint8_t coefficient) {
    const auto blend = [coefficient](const std::uint8_t from, const std::uint8_t to) {
        return static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(255u - coefficient) * from +
             static_cast<std::uint32_t>(coefficient) * to + 127u) /
            255u);
    };
    return {blend(source.r, fog.r), blend(source.g, fog.g), blend(source.b, fog.b), source.a};
}

template <typename RegisterView>
std::uint8_t table_fog_coefficient(const RegisterView& registers, const float depth) {
    const auto encoded_density = registers.read(pvr_register::FogDensity) & 0xFFFFu;
    const auto mantissa = static_cast<std::uint8_t>(encoded_density >> 8u);
    if (mantissa == 0u || !std::isfinite(depth) || depth <= 0.0f) return 0u;
    const auto exponent = static_cast<std::int8_t>(encoded_density & 0xFFu);
    const auto density = std::ldexp(static_cast<float>(mantissa) / 128.0f, exponent);
    const auto inverse_w = std::clamp(depth * density, 1.0f, 255.9999f);
    const auto index_bits = std::clamp(static_cast<int>(std::floor(std::log2(inverse_w))), 0, 7);
    const auto normalized = std::ldexp(inverse_w, -index_bits);
    const auto position = std::clamp((normalized - 1.0f) * 16.0f, 0.0f, 15.9999f);
    const auto mantissa_bits = std::clamp(static_cast<int>(std::floor(position)), 0, 15);
    const auto fraction = position - static_cast<float>(mantissa_bits);
    const auto table_index = static_cast<std::uint32_t>(index_bits * 16 + mantissa_bits);
    const auto entry = registers.read(pvr_register::FogTableBase + table_index * 4u);
    const auto current = static_cast<float>((entry >> 8u) & 0xFFu);
    const auto next = static_cast<float>(entry & 0xFFu);
    return static_cast<std::uint8_t>(std::lround(current + (next - current) * fraction));
}

template <typename RegisterView>
Rgba8 clamp_fragment_color(const Rgba8 source, const RegisterView& registers) {
    const auto minimum = decode_register_color(registers.read(pvr_register::ColorClampMinimum));
    const auto maximum = decode_register_color(registers.read(pvr_register::ColorClampMaximum));
    if (minimum.r > maximum.r || minimum.g > maximum.g || minimum.b > maximum.b)
        throw std::runtime_error("PVR-Farbclamp besitzt vertauschte RGB-Grenzen.");
    return {std::clamp(source.r, minimum.r, maximum.r),
            std::clamp(source.g, minimum.g, maximum.g),
            std::clamp(source.b, minimum.b, maximum.b),
            source.a};
}

template <typename RegisterView>
Rgba8 sample_texture_nearest(const LinearMemoryDevice& vram,
                             const RegisterView& registers,
                             const PvrMaterial& material,
                             float u,
                             float v) {
    if (!material.textured) return {};
    if (material.texture_width == 0u || material.texture_height == 0u ||
        material.texture_width > 1024u || material.texture_height > 1024u)
        throw std::runtime_error("PVR-Texturgeometrie liegt ausserhalb des Hardwarevertrags.");
    if (material.texture_vq && material.texture_format != 4u &&
        material.texture_format > 2u)
        throw std::runtime_error(
            "PVR-VQ ist nur fuer 16-Bit-Farb- und Bumptexturen definiert.");
    if (material.texture_format > 6u)
        throw std::runtime_error("PVR-Texturformat ist im allgemeinen Renderer nicht integriert.");
    u = texture_coordinate(u, material.clamp_u, material.flip_u);
    v = texture_coordinate(v, material.clamp_v, material.flip_v);
    const auto x = std::min<std::uint32_t>(
        material.texture_width - 1u,
        static_cast<std::uint32_t>(u * static_cast<float>(material.texture_width)));
    const auto y = std::min<std::uint32_t>(
        material.texture_height - 1u,
        static_cast<std::uint32_t>(v * static_cast<float>(material.texture_height)));
    const auto level_offset = mipmap_level_offset(material);
    const auto index = texture_pixel_index(material, x, y);
    Rgba8 color;
    if (material.texture_vq) {
        auto block_material = material;
        block_material.texture_width = std::max(1u, material.texture_width / 2u);
        block_material.texture_height = std::max(1u, material.texture_height / 2u);
        if (block_material.texture_stride_width != 0u)
            block_material.texture_stride_width = std::max(1u, block_material.texture_stride_width / 2u);
        const auto block_index = texture_pixel_index(block_material, x / 2u, y / 2u);
        const auto index_offset = static_cast<std::uint64_t>(material.texture_base) + 2048u +
                                  level_offset + block_index;
        if (index_offset >= vram.size())
            throw std::out_of_range("PVR-VQ-Indexzugriff liegt ausserhalb des VRAM.");
        const auto code = vram.read_u8(static_cast<std::uint32_t>(index_offset));
        // Codebook texels are themselves twiddled: TL, BL, TR, BR.
        const auto texel = ((x & 1u) << 1u) | (y & 1u);
        const auto texel_offset = static_cast<std::uint64_t>(material.texture_base) +
                                  static_cast<std::uint64_t>(code) * 8u + texel * 2u;
        if (texel_offset + 2u > vram.size())
            throw std::out_of_range("PVR-VQ-Codebookzugriff liegt ausserhalb des VRAM.");
        const auto texel_value = vram.read_u16(static_cast<std::uint32_t>(texel_offset));
        color = material.texture_format == 4u
                    ? decode_bump_texel(texel_value)
                    : decode_16bit_color(texel_value, material.texture_format);
    } else if (material.texture_format <= 2u || material.texture_format == 4u) {
        const auto byte_offset = static_cast<std::uint64_t>(material.texture_base) +
                                 level_offset + index * 2u;
        if (byte_offset + 2u > vram.size())
            throw std::out_of_range("PVR-Texturzugriff liegt ausserhalb des VRAM.");
        const auto texel_value = vram.read_u16(static_cast<std::uint32_t>(byte_offset));
        color = material.texture_format == 4u
                    ? decode_bump_texel(texel_value)
                    : decode_16bit_color(texel_value, material.texture_format);
    } else if (material.texture_format == 3u) {
        std::uint64_t first_index = 0u;
        std::uint64_t second_index = 0u;
        bool second_pixel = false;
        if (material.texture_twiddled) {
            const auto group = index & ~std::uint64_t{3u};
            first_index = group + (index & 1u);
            second_index = first_index + 2u;
            second_pixel = (index & 2u) != 0u;
        } else {
            first_index = index & ~std::uint64_t{1u};
            second_index = first_index + 1u;
            second_pixel = (index & 1u) != 0u;
        }
        const auto first_offset = static_cast<std::uint64_t>(material.texture_base) +
                                  level_offset + first_index * 2u;
        const auto second_offset = static_cast<std::uint64_t>(material.texture_base) +
                                   level_offset + second_index * 2u;
        if (first_offset + 2u > vram.size() || second_offset + 2u > vram.size())
            throw std::out_of_range("PVR-YUV-Texturzugriff liegt ausserhalb des VRAM.");
        color = decode_yuv_pair(vram.read_u16(static_cast<std::uint32_t>(first_offset)),
                                vram.read_u16(static_cast<std::uint32_t>(second_offset)),
                                second_pixel);
    } else {
        std::uint32_t palette_index = 0u;
        if (material.texture_format == 5u) {
            const auto byte_offset = static_cast<std::uint64_t>(material.texture_base) +
                                     level_offset + index / 2u;
            if (byte_offset >= vram.size())
                throw std::out_of_range("PVR-4BPP-Texturzugriff liegt ausserhalb des VRAM.");
            const auto packed = vram.read_u8(static_cast<std::uint32_t>(byte_offset));
            palette_index = static_cast<std::uint32_t>(material.palette_bank) * 16u +
                            (((index & 1u) == 0u) ? (packed & 0xFu) : (packed >> 4u));
        } else {
            const auto byte_offset = static_cast<std::uint64_t>(material.texture_base) +
                                     level_offset + index;
            if (byte_offset >= vram.size())
                throw std::out_of_range("PVR-8BPP-Texturzugriff liegt ausserhalb des VRAM.");
            palette_index = static_cast<std::uint32_t>(material.palette_bank) * 256u +
                            vram.read_u8(static_cast<std::uint32_t>(byte_offset));
        }
        color = decode_palette_color(registers, palette_index);
    }
    if (material.texture_alpha_disabled && material.texture_format != 4u) color.a = 0xFFu;
    return color;
}

template <typename RegisterView>
Rgba8 sample_texture(const LinearMemoryDevice& vram,
                     const RegisterView& registers,
                     const PvrMaterial& material,
                     const float u,
                     const float v) {
    if (material.texture_filter >= 2u)
        throw std::runtime_error(
            "PVR-Trilinear-Pass A/B braucht eine echte D-basierte Mipmap-Levelwahl.");
    if (material.texture_supersampling) {
        auto sample_material = material;
        sample_material.texture_supersampling = false;
        const auto du = 0.25f / static_cast<float>(std::max(1u, material.texture_width));
        const auto dv = 0.25f / static_cast<float>(std::max(1u, material.texture_height));
        const std::array samples{
            sample_texture(vram, registers, sample_material, u - du, v - dv),
            sample_texture(vram, registers, sample_material, u + du, v - dv),
            sample_texture(vram, registers, sample_material, u - du, v + dv),
            sample_texture(vram, registers, sample_material, u + du, v + dv)};
        const auto average = [&](const auto member) {
            std::uint32_t sum = 0u;
            for (const auto& sample : samples) sum += sample.*member;
            return static_cast<std::uint8_t>((sum + 2u) / 4u);
        };
        return {average(&Rgba8::r),
                average(&Rgba8::g),
                average(&Rgba8::b),
                average(&Rgba8::a)};
    }
    if (material.texture_filter == 0u)
        return sample_texture_nearest(vram, registers, material, u, v);
    const auto texel_x = u * static_cast<float>(material.texture_width) - 0.5f;
    const auto texel_y = v * static_cast<float>(material.texture_height) - 0.5f;
    const auto x0 = std::floor(texel_x);
    const auto y0 = std::floor(texel_y);
    const auto fx = texel_x - x0;
    const auto fy = texel_y - y0;
    const auto coordinate_u = [&](const float x) {
        return (x + 0.5f) / static_cast<float>(material.texture_width);
    };
    const auto coordinate_v = [&](const float y) {
        return (y + 0.5f) / static_cast<float>(material.texture_height);
    };
    const auto c00 = sample_texture_nearest(
        vram, registers, material, coordinate_u(x0), coordinate_v(y0));
    const auto c10 = sample_texture_nearest(
        vram, registers, material, coordinate_u(x0 + 1.0f), coordinate_v(y0));
    const auto c01 = sample_texture_nearest(
        vram, registers, material, coordinate_u(x0), coordinate_v(y0 + 1.0f));
    const auto c11 = sample_texture_nearest(
        vram, registers, material, coordinate_u(x0 + 1.0f), coordinate_v(y0 + 1.0f));
    const auto interpolate = [&](const std::uint8_t a,
                                 const std::uint8_t b,
                                 const std::uint8_t c,
                                 const std::uint8_t d) {
        const auto top = static_cast<float>(a) + (static_cast<float>(b) - a) * fx;
        const auto bottom = static_cast<float>(c) + (static_cast<float>(d) - c) * fx;
        return static_cast<std::uint8_t>(std::lround(std::clamp(
            top + (bottom - top) * fy, 0.0f, 255.0f)));
    };
    return {interpolate(c00.r, c10.r, c01.r, c11.r),
            interpolate(c00.g, c10.g, c01.g, c11.g),
            interpolate(c00.b, c10.b, c01.b, c11.b),
            interpolate(c00.a, c10.a, c01.a, c11.a)};
}

Rgba8 shade_texture(const Rgba8 texture,
                    const Rgba8 vertex,
                    const std::uint8_t mode) noexcept {
    const auto multiply = [](const std::uint8_t left, const std::uint8_t right) {
        return static_cast<std::uint8_t>((static_cast<unsigned>(left) * right + 127u) / 255u);
    };
    if (mode == 0u) return texture;
    if (mode == 1u)
        return {multiply(texture.r, vertex.r),
                multiply(texture.g, vertex.g),
                multiply(texture.b, vertex.b),
                texture.a};
    if (mode == 2u) {
        const auto mix = [alpha = texture.a](const std::uint8_t foreground,
                                              const std::uint8_t background) {
            return static_cast<std::uint8_t>((static_cast<unsigned>(foreground) * alpha +
                                              static_cast<unsigned>(background) * (255u - alpha) +
                                              127u) /
                                             255u);
        };
        return {mix(texture.r, vertex.r),
                mix(texture.g, vertex.g),
                mix(texture.b, vertex.b),
                vertex.a};
    }
    return {multiply(texture.r, vertex.r),
            multiply(texture.g, vertex.g),
            multiply(texture.b, vertex.b),
            multiply(texture.a, vertex.a)};
}

Rgba8 add_offset_color(const Rgba8 source, const Rgba8 offset) noexcept {
    const auto add = [](const std::uint8_t left, const std::uint8_t right) {
        return static_cast<std::uint8_t>(
            std::min<unsigned>(255u, static_cast<unsigned>(left) + right));
    };
    return {add(source.r, offset.r), add(source.g, offset.g), add(source.b, offset.b), source.a};
}

bool depth_passes(const std::uint8_t comparison, const float source, const float destination) {
    switch (comparison) {
    case 0u: return false;
    case 1u: return source < destination;
    case 2u: return source == destination;
    case 3u: return source <= destination;
    case 4u: return source > destination;
    case 5u: return source != destination;
    case 6u: return source >= destination;
    case 7u: return true;
    default: return false;
    }
}

float blend_factor(const std::uint8_t mode,
                   const Rgba8 source,
                   const Rgba8 destination,
                   const unsigned channel) noexcept {
    const auto destination_channel =
        channel == 0u ? destination.r : channel == 1u ? destination.g : destination.b;
    switch (mode) {
    case 0u: return 0.0f;
    case 1u: return 1.0f;
    case 2u: return destination_channel / 255.0f;
    case 3u: return 1.0f - destination_channel / 255.0f;
    case 4u: return source.a / 255.0f;
    case 5u: return 1.0f - source.a / 255.0f;
    case 6u: return destination.a / 255.0f;
    case 7u: return 1.0f - destination.a / 255.0f;
    default: return 0.0f;
    }
}

Rgba8 blend_color(const Rgba8 source,
                  const Rgba8 destination,
                  const PvrMaterial& material) noexcept {
    const auto combine = [&](const std::uint8_t source_channel,
                             const std::uint8_t destination_channel,
                             const unsigned channel) {
        const auto value = source_channel * blend_factor(
                                                material.source_blend, source, destination, channel) +
                           destination_channel * blend_factor(
                                                     material.destination_blend,
                                                     source,
                                                     destination,
                                                     channel);
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
    };
    return {combine(source.r, destination.r, 0u),
            combine(source.g, destination.g, 1u),
            combine(source.b, destination.b, 2u),
            combine(source.a, destination.a, 3u)};
}
} // namespace

std::optional<PvrScanoutDescriptor> decode_pvr_scanout(const PvrRegisterFile& registers,
                                                       const std::size_t vram_size) {
    const auto control = registers.read(pvr_register::FramebufferReadControl);
    if ((control & 1u) == 0u) return std::nullopt;

    const auto depth = (control >> 2u) & 3u;
    const auto format = depth == 0u   ? PvrFramebufferFormat::Rgb0555
                        : depth == 1u ? PvrFramebufferFormat::Rgb565
                        : depth == 2u ? PvrFramebufferFormat::Rgb888
                                      : PvrFramebufferFormat::Rgb0888;
    const auto size = registers.read(pvr_register::FramebufferReadSize);
    const auto x_size = static_cast<std::size_t>(size & 0x3FFu);
    const auto line_words = x_size + 1u;
    const auto line_bytes =
        checked_multiply(line_words, 4u, "PVR-Scanout-Zeilenbreite ist zu gross.");
    const auto pixel_bytes = bytes_per_pixel(format);
    // Packed RGB888 consumes complete three-byte pixels from the byte stream. Any
    // trailing byte(s) in the final 32-bit word are padding, not invalid geometry.
    const auto source_width = line_bytes / pixel_bytes;
    const auto field_height = static_cast<std::size_t>((size >> 10u) & 0x3FFu) + 1u;
    const auto modulus_units = static_cast<std::size_t>((size >> 20u) & 0x3FFu);
    // FB_R_SIZE encodes line bytes as (x + 1) * 4 and the next-row distance as
    // (x + modulus) * 4. In particular, modulus zero intentionally overlaps the
    // preceding row by one 32-bit word.
    const auto stride = checked_multiply(
        checked_add(x_size, modulus_units, "PVR-Scanout-Stride ist zu gross."),
        4u,
        "PVR-Scanout-Stride ist zu gross.");
    const auto base =
        static_cast<std::size_t>(registers.read(pvr_register::FramebufferReadSof1) & 0x007FFFFCu);
    const auto second_base =
        static_cast<std::size_t>(registers.read(pvr_register::FramebufferReadSof2) & 0x007FFFFCu);
    const auto interlaced = (registers.read(pvr_register::SpgControl) & 0x10u) != 0u;
    const auto line_double = (control & 2u) != 0u;
    const auto weave_fields =
        interlaced && stride == checked_multiply(line_bytes, 2u, "PVR-Weave-Stride ist zu gross.") &&
        second_base == checked_add(base, line_bytes, "PVR-Weave-Feldadresse laeuft ueber.");
    const auto source_height = weave_fields
                                   ? checked_multiply(field_height,
                                                      2u,
                                                      "PVR-Scanout-Hoehe ist zu gross.")
                                   : field_height;
    const auto scaler = registers.read(pvr_register::ScalerControl);
    const auto horizontal_scale = (scaler & 0x00010000u) != 0u;
    const auto vertical_scale_factor = static_cast<std::uint16_t>(scaler & 0xFFFFu);
    if (vertical_scale_factor == 0u)
        throw std::invalid_argument("PVR-Vertikalskalierung besitzt einen Nullfaktor.");
    const auto width = horizontal_scale ? (source_width + 1u) / 2u : source_width;
    const auto scaled_height = checked_add(
                                   checked_multiply(source_height,
                                                    1024u,
                                                    "PVR-Skalierungshoehe ist zu gross."),
                                   vertical_scale_factor - 1u,
                                   "PVR-Skalierungshoehe ist zu gross.") /
                               vertical_scale_factor;
    const auto height = line_double
                            ? checked_multiply(scaled_height,
                                               2u,
                                               "PVR-Line-Doubling-Hoehe ist zu gross.")
                            : scaled_height;
    // The scan generator exposes ten-bit raster dimensions per field. Retain room
    // for the composed pair of interlaced fields, but reject register combinations
    // that would turn a tiny scale divisor into a multi-gigabyte host frame.
    constexpr std::size_t maximum_scanout_dimension = 2048u;
    constexpr std::size_t maximum_scanout_pixels =
        maximum_scanout_dimension * maximum_scanout_dimension;
    if (width > maximum_scanout_dimension || height > maximum_scanout_dimension)
        throw std::out_of_range("PVR-Scanout-Skalierung liegt ausserhalb der Hardwaregrenzen.");
    if (checked_multiply(width,
                         height,
                         "PVR-Scanout-Pixelzahl ist zu gross.") > maximum_scanout_pixels)
        throw std::out_of_range("PVR-Scanout-Pixelbudget ist ueberschritten.");
    const auto field_span = checked_add(
        checked_multiply(stride,
                         field_height - 1u,
                         "PVR-Scanout-VRAM-Ausdehnung ist zu gross."),
        line_bytes,
        "PVR-Scanout-VRAM-Ausdehnung ist zu gross.");
    if (checked_add(base, field_span, "PVR-Scanout-Endadresse laeuft ueber.") > vram_size ||
        (interlaced &&
         checked_add(second_base, field_span, "PVR-Scanout-Endadresse laeuft ueber.") >
             vram_size)) {
        throw std::out_of_range("PVR-Scanout liegt ausserhalb des VRAM-Abbilds.");
    }
    const auto border = registers.read(pvr_register::BorderColor);
    PvrScanoutDescriptor result;
    result.width = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    result.source_width = static_cast<std::uint32_t>(source_width);
    result.source_height = static_cast<std::uint32_t>(source_height);
    result.stride_bytes = static_cast<std::uint32_t>(stride);
    result.base_offset = base;
    result.second_base_offset = second_base;
    result.format = format;
    result.concat = static_cast<std::uint8_t>((control >> 4u) & 7u);
    result.line_double = line_double;
    result.interlaced = interlaced;
    result.weave_fields = weave_fields;
    result.horizontal_scale = horizontal_scale;
    result.vertical_scale_factor = vertical_scale_factor;
    result.video_blank = (registers.read(pvr_register::VideoControl) & 0x8u) != 0u;
    result.border_rgba = {static_cast<std::uint8_t>(border >> 16u),
                          static_cast<std::uint8_t>(border >> 8u),
                          static_cast<std::uint8_t>(border),
                          0xFFu};
    return result;
}

void PvrFramebuffer::configure(const std::uint32_t width,
                               const std::uint32_t height,
                               const std::uint32_t stride_bytes,
                               const PvrFramebufferFormat format,
                               const bool line_double,
                               const bool interlaced,
                               const std::uint32_t source_width,
                               const std::uint32_t source_height,
                               const std::uint8_t concat,
                               const bool logical_32bit_vram) {
    if (width == 0u || height == 0u) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    const auto effective_source_width = source_width == 0u ? width : source_width;
    const auto effective_source_height =
        source_height == 0u
            ? (line_double ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(height) + 1u) /
                                                        2u)
                           : height)
            : source_height;
    if (effective_source_width == 0u || effective_source_height == 0u)
        throw std::invalid_argument("PVR-Framebuffer-Quellgeometrie ist leer.");
    const auto pixel_bytes = bytes_per_pixel(format);
    const auto minimum_stride = checked_multiply(
        static_cast<std::size_t>(effective_source_width),
        pixel_bytes,
        "PVR-Framebuffer-Zeilenbreite ist zu gross.");
    const auto stride_is_valid =
        static_cast<std::size_t>(stride_bytes) >= minimum_stride ||
        ((stride_bytes & 3u) == 0u &&
         static_cast<std::size_t>(stride_bytes) + 4u >= minimum_stride);
    if (!stride_is_valid) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    width_ = width;
    height_ = height;
    source_width_ = effective_source_width;
    source_height_ = effective_source_height;
    stride_ = stride_bytes;
    format_ = format;
    concat_ = concat & 7u;
    line_double_ = line_double;
    interlaced_ = interlaced;
    logical_32bit_vram_ = logical_32bit_vram;
}

PvrFrame PvrFramebuffer::capture(const std::span<const std::uint8_t> vram,
                                 const std::size_t base_offset,
                                 const std::optional<std::size_t> second_base_offset,
                                 const std::optional<std::array<std::uint8_t, 4u>> solid_color) {
    if (width_ == 0u || height_ == 0u) {
        throw std::logic_error("PVR-Framebuffer wurde nicht konfiguriert.");
    }
    const auto pixel_count = checked_multiply(static_cast<std::size_t>(width_),
                                              static_cast<std::size_t>(height_),
                                              "PVR-Framebuffer-Pixelzahl ist zu gross.");
    const auto rgba_size =
        checked_multiply(pixel_count, 4u, "PVR-Framebuffer-RGBA-Ausgabe ist zu gross.");
    if (solid_color) {
        PvrFrame frame{width_, height_, std::vector<std::uint8_t>(rgba_size)};
        for (std::size_t pixel = 0u; pixel < pixel_count; ++pixel)
            std::copy(solid_color->begin(), solid_color->end(), frame.rgba.begin() + pixel * 4u);
        ++presented_frames_;
        return frame;
    }
    if (interlaced_ && !second_base_offset)
        throw std::invalid_argument("Interlaced PVR-Scanout braucht beide Feldadressen.");
    if (logical_32bit_vram_ && vram.size() != dreamcast_vram_size) {
        throw std::invalid_argument(
            "Die logische PVR-32-Bit-Sicht braucht exakt das 8-MiB-VRAM-Abbild.");
    }
    const auto field_rows = interlaced_ ? (static_cast<std::size_t>(source_height_) + 1u) / 2u
                                        : static_cast<std::size_t>(source_height_);
    const auto line_bytes = checked_multiply(static_cast<std::size_t>(source_width_),
                                             bytes_per_pixel(format_),
                                             "PVR-Framebuffer-Zeilenbreite ist zu gross.");
    const auto field_span = checked_add(
        checked_multiply(static_cast<std::size_t>(stride_),
                         field_rows - 1u,
                         "PVR-Framebuffer-VRAM-Ausdehnung ist zu gross."),
        line_bytes,
        "PVR-Framebuffer-VRAM-Ausdehnung ist zu gross.");
    const auto required = checked_add(
        base_offset, field_span, "PVR-Framebuffer-VRAM-Endadresse laeuft ueber.");
    const auto second_required = second_base_offset
                                     ? checked_add(*second_base_offset,
                                                   field_span,
                                                   "PVR-Framebuffer-VRAM-Endadresse laeuft ueber.")
                                     : 0u;
    if (!logical_32bit_vram_ &&
        (required > vram.size() || (second_base_offset && second_required > vram.size()))) {
        throw std::out_of_range("PVR-Framebuffer liegt ausserhalb des VRAM-Abbilds.");
    }
    const auto read_vram_byte = [&](const std::size_t logical_offset) {
        if (!logical_32bit_vram_) return vram[logical_offset];
        const auto wrapped = static_cast<std::uint32_t>(logical_offset % dreamcast_vram_size);
        return vram[dreamcast_vram_32bit_to_linear_offset(wrapped)];
    };
    PvrFrame frame{width_, height_, std::vector<std::uint8_t>(rgba_size)};
    for (std::uint32_t y = 0u; y < height_; ++y) {
        const auto source_line = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(y) * source_height_ / height_);
        const auto source_base = interlaced_ && (source_line & 1u) != 0u
                                     ? *second_base_offset
                                     : base_offset;
        const auto source_row = interlaced_ ? source_line / 2u : source_line;
        for (std::uint32_t x = 0u; x < width_; ++x) {
            const auto source_x = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x) * source_width_ / width_);
            const auto source =
                source_base + static_cast<std::size_t>(source_row) * stride_ +
                source_x * bytes_per_pixel(format_);
            const auto destination = (static_cast<std::size_t>(y) * width_ + x) * 4u;
            if (format_ == PvrFramebufferFormat::Rgb888) {
                frame.rgba[destination] = read_vram_byte(source + 2u);
                frame.rgba[destination + 1u] = read_vram_byte(source + 1u);
                frame.rgba[destination + 2u] = read_vram_byte(source);
                frame.rgba[destination + 3u] = 0xFFu;
                continue;
            }
            if (format_ == PvrFramebufferFormat::Rgb0888) {
                frame.rgba[destination] = read_vram_byte(source + 2u);
                frame.rgba[destination + 1u] = read_vram_byte(source + 1u);
                frame.rgba[destination + 2u] = read_vram_byte(source);
                frame.rgba[destination + 3u] = 0xFFu;
                continue;
            }
            const auto pixel = static_cast<std::uint16_t>(read_vram_byte(source)) |
                               static_cast<std::uint16_t>(read_vram_byte(source + 1u) << 8u);
            if (format_ == PvrFramebufferFormat::Rgb565) {
                frame.rgba[destination] =
                    static_cast<std::uint8_t>(((pixel >> 11u) & 0x1Fu) << 3u | concat_);
                frame.rgba[destination + 1u] =
                    static_cast<std::uint8_t>(((pixel >> 5u) & 0x3Fu) << 2u |
                                              (concat_ & 3u));
                frame.rgba[destination + 2u] =
                    static_cast<std::uint8_t>((pixel & 0x1Fu) << 3u | concat_);
                frame.rgba[destination + 3u] = 0xFFu;
            } else {
                frame.rgba[destination] =
                    static_cast<std::uint8_t>(((pixel >> 10u) & 0x1Fu) << 3u | concat_);
                frame.rgba[destination + 1u] =
                    static_cast<std::uint8_t>(((pixel >> 5u) & 0x1Fu) << 3u | concat_);
                frame.rgba[destination + 2u] =
                    static_cast<std::uint8_t>((pixel & 0x1Fu) << 3u | concat_);
                frame.rgba[destination + 3u] = 0xFFu;
            }
        }
    }
    ++presented_frames_;
    return frame;
}

std::uint64_t PvrFramebuffer::presented_frames() const noexcept {
    return presented_frames_;
}

std::uint8_t TileAccelerator::list_rank(const PvrListType type) noexcept {
    return static_cast<std::uint8_t>(type);
}

void TileAccelerator::begin_list(const PvrListType type) {
    if (list_open_) {
        throw PvrTaParserException(PvrTaInputErrorReason::InvalidListOrder,
                                   "Eine PVR-Primitivliste ist bereits offen.");
    }
    if (frame_has_list_ && list_rank(type) < highest_list_rank_) {
        throw PvrTaParserException(
            PvrTaInputErrorReason::InvalidListOrder,
            "PVR-Primitivlisten wurden in rueckwaertiger Reihenfolge begonnen.");
    }
    current_list_ = type;
    highest_list_rank_ = list_rank(type);
    frame_has_list_ = true;
    current_strip_.clear();
    list_open_ = true;
}

void TileAccelerator::set_material(PvrMaterial material) {
    if (!list_open_)
        throw PvrTaParserException(PvrTaInputErrorReason::InvalidListOrder,
                                   "PVR-Material ohne offene Primitivliste.");
    if (!current_strip_.empty())
        throw PvrTaParserException(
            PvrTaInputErrorReason::InvalidListOrder,
            "PVR-Material wechselt innerhalb eines Triangle-Strips.");
    current_material_ = std::move(material);
}

void TileAccelerator::submit_vertex(const PvrVertex& vertex, const bool end_of_strip) {
    if (!list_open_) {
        throw PvrTaParserException(PvrTaInputErrorReason::InvalidListOrder,
                                   "PVR-Vertex ohne offene Primitivliste.");
    }
    current_strip_.push_back(vertex);
    if (!end_of_strip) {
        return;
    }
    if (current_strip_.size() < 3u) {
        throw PvrTaParserException(
            PvrTaInputErrorReason::IncompletePacket,
            "Ein PVR-Triangle-Strip braucht mindestens drei Vertices.");
    }
    primitives_.push_back(
        PvrPrimitive{current_list_, std::move(current_strip_), current_material_});
    current_strip_.clear();
}

void TileAccelerator::end_list() {
    if (!list_open_) {
        throw PvrTaParserException(PvrTaInputErrorReason::InvalidListOrder,
                                   "Keine PVR-Primitivliste ist offen.");
    }
    if (!current_strip_.empty()) {
        throw PvrTaParserException(
            PvrTaInputErrorReason::IncompletePacket,
            "PVR-Primitivliste endet mit einem unvollstaendigen Strip.");
    }
    list_open_ = false;
}

PvrTaFrame TileAccelerator::finish_frame() {
    if (list_open_) {
        throw PvrTaParserException(PvrTaInputErrorReason::IncompletePacket,
                                   "PVR-Frame endet mit einer offenen Primitivliste.");
    }
    PvrTaFrame result{std::move(primitives_)};
    primitives_.clear();
    current_material_ = {};
    highest_list_rank_ = 0u;
    frame_has_list_ = false;
    return result;
}

bool TileAccelerator::list_open() const noexcept {
    return list_open_;
}

TileAcceleratorSnapshot TileAccelerator::snapshot() const {
    return {
        primitives_,
        current_strip_,
        current_list_,
        current_material_,
        highest_list_rank_,
        frame_has_list_,
        list_open_,
    };
}

void TileAccelerator::validate_state_restore(
    const TileAcceleratorSnapshot& state) const {
    if (!valid_pvr_list_type(state.current_list) ||
        state.highest_list_rank > 4u ||
        (state.list_open && !state.frame_has_list) ||
        (!state.list_open && !state.current_strip.empty()))
        throw std::invalid_argument(
            "PVR-TA-Handoff besitzt einen inkonsistenten Listenstatus.");
    validate_pvr_material(state.current_material);
    for (const auto& vertex : state.current_strip)
        validate_pvr_vertex(vertex);
    for (const auto& primitive : state.primitives)
        validate_pvr_primitive(primitive);
}

void TileAccelerator::restore_state_passive(
    TileAcceleratorSnapshot state) {
    validate_state_restore(state);
    detach_pvr_material_graphs(state);
    primitives_ = std::move(state.primitives);
    current_strip_ = std::move(state.current_strip);
    current_list_ = state.current_list;
    current_material_ = std::move(state.current_material);
    highest_list_rank_ = state.highest_list_rank;
    frame_has_list_ = state.frame_has_list;
    list_open_ = state.list_open;
}

namespace {

std::uint32_t ta_u32(const std::span<const std::uint8_t> packet, const std::size_t offset) {
    if (offset > packet.size() || packet.size() - offset < 4u)
        throw PvrTaParserException(PvrTaInputErrorReason::BufferOverflow,
                                   "TA-Paket ist abgeschnitten.");
    std::uint32_t value = 0u;
    std::memcpy(&value, packet.data() + offset, sizeof(value));
    return value;
}

PvrListType decode_list_type(const std::uint32_t pcw) {
    switch ((pcw >> 24u) & 7u) {
    case 0u:
        return PvrListType::Opaque;
    case 1u:
        return PvrListType::OpaqueModifier;
    case 2u:
        return PvrListType::Translucent;
    case 3u:
        return PvrListType::TranslucentModifier;
    case 4u:
        return PvrListType::PunchThrough;
    default:
        throw PvrTaParserException(
            PvrTaInputErrorReason::UnsupportedPacket,
            "TA-Objektliste wird vom allgemeinen Polygonpfad abgewiesen.");
    }
}

float decode_uv16_component(const std::uint32_t packed, const bool u) {
    const auto bits = u ? packed & 0xFFFF0000u : packed << 16u;
    return std::bit_cast<float>(bits);
}

std::uint32_t decode_ta_float_color(const std::span<const std::uint8_t> packet,
                                    const std::size_t offset) {
    const auto channel = [&](const std::size_t component) {
        const auto value = std::bit_cast<float>(ta_u32(packet, offset + component));
        if (!std::isfinite(value))
            throw PvrTaParserException(PvrTaInputErrorReason::InvalidPacket,
                                       "TA-Floatfarbe ist nicht endlich.");
        return static_cast<std::uint32_t>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(0u) << 24u) | (channel(4u) << 16u) | (channel(8u) << 8u) |
           channel(12u);
}

std::uint32_t scale_ta_face_color(const std::uint32_t color, const float intensity) {
    const auto scale = [intensity](const std::uint32_t value) {
        return static_cast<std::uint32_t>(std::lround(value * intensity));
    };
    return (color & 0xFF000000u) | (scale((color >> 16u) & 0xFFu) << 16u) |
           (scale((color >> 8u) & 0xFFu) << 8u) | scale(color & 0xFFu);
}

void decode_ta_texture_words(PvrMaterial& material,
                             const std::uint32_t mode2,
                             const std::uint32_t mode3) {
    material.texture_height = 8u << (mode2 & 7u);
    material.texture_width = 8u << ((mode2 >> 3u) & 7u);
    material.texture_shading = static_cast<std::uint8_t>((mode2 >> 6u) & 3u);
    material.texture_mipmap_bias = static_cast<std::uint8_t>((mode2 >> 8u) & 0xFu);
    material.texture_supersampling = (mode2 & 0x00001000u) != 0u;
    material.texture_filter = static_cast<std::uint8_t>((mode2 >> 13u) & 3u);
    material.clamp_v = (mode2 & 0x00008000u) != 0u;
    material.clamp_u = (mode2 & 0x00010000u) != 0u;
    material.flip_v = (mode2 & 0x00020000u) != 0u;
    material.flip_u = (mode2 & 0x00040000u) != 0u;
    material.texture_alpha_disabled = (mode2 & 0x00080000u) != 0u;
    material.vertex_alpha_enabled = (mode2 & 0x00100000u) != 0u;
    material.color_clamp_enabled = (mode2 & 0x00200000u) != 0u;
    material.fog_mode = static_cast<std::uint8_t>((mode2 >> 22u) & 3u);
    material.blend_destination_accumulation = (mode2 & 0x01000000u) != 0u;
    material.blend_source_accumulation = (mode2 & 0x02000000u) != 0u;
    material.destination_blend = static_cast<std::uint8_t>((mode2 >> 26u) & 7u);
    material.source_blend = static_cast<std::uint8_t>((mode2 >> 29u) & 7u);
    material.texture_format = static_cast<std::uint8_t>((mode3 >> 27u) & 7u);
    material.texture_base =
        (mode3 & (material.texture_format == 5u ? 0x001FFFFFu : 0x01FFFFFFu)) << 3u;
    material.texture_x32_stride =
        material.texture_format < 5u && (mode3 & 0x02000000u) != 0u;
    material.texture_twiddled =
        material.texture_format >= 5u || (mode3 & 0x04000000u) == 0u;
    if (material.texture_format == 5u)
        material.palette_bank = static_cast<std::uint8_t>((mode3 >> 21u) & 0x3Fu);
    else if (material.texture_format == 6u)
        material.palette_bank = static_cast<std::uint8_t>((mode3 >> 25u) & 3u);
    material.texture_vq = (mode3 & 0x40000000u) != 0u;
    material.texture_mipmapped = (mode3 & 0x80000000u) != 0u;
}

float interpolate_sprite_z(const PvrVertex& a,
                           const PvrVertex& b,
                           const PvrVertex& c,
                           const float x,
                           const float y) {
    const auto bax = b.x - a.x;
    const auto bay = b.y - a.y;
    const auto cax = c.x - a.x;
    const auto cay = c.y - a.y;
    const auto determinant = bax * cay - bay * cax;
    if (std::fabs(determinant) <= std::numeric_limits<float>::epsilon())
        return (a.z + b.z + c.z) / 3.0f;
    const auto dx = x - a.x;
    const auto dy = y - a.y;
    const auto b_weight = (dx * cay - dy * cax) / determinant;
    const auto c_weight = (bax * dy - bay * dx) / determinant;
    return a.z + b_weight * (b.z - a.z) + c_weight * (c.z - a.z);
}

} // namespace

PvrTaFifo::PvrTaFifo(std::function<void(PvrListType)> list_observer)
    : list_observer_(std::move(list_observer)) {}

void PvrTaFifo::submit(const std::span<const std::uint8_t> packet) {
    if (packet.size() != 32u)
        throw PvrTaParserException(PvrTaInputErrorReason::InvalidPacket,
                                   "TA-FIFO erwartet 32-Byte-Parameter.");
    if (frame_packets_ == pvr_ta_maximum_frame_packets)
        throw PvrTaParserException(
            PvrTaInputErrorReason::BufferOverflow,
            "TA-Frame ueberschreitet das 8-MiB-Parameterfenster.");
    ++frame_packets_;
    ++metrics_.packets;
    const auto normalized_kind =
        pending_intensity_header_
            ? PvrTaPacketKind::IntensityContinuation
        : pending_modifier_vertex_packet_
            ? PvrTaPacketKind::ModifierVertexContinuation
        : pending_extended_vertex_
            ? PvrTaPacketKind::ExtendedVertexContinuation
        : pending_sprite_vertex_
            ? PvrTaPacketKind::SpriteContinuation
            : static_cast<PvrTaPacketKind>((ta_u32(packet, 0u) >> 29u) & 7u);
    ++metrics_.normalized_packets[static_cast<std::size_t>(normalized_kind)];
    if (pending_intensity_header_) {
        active_header_argb_ = decode_ta_float_color(packet, 0u);
        if (active_two_volume_) {
            active_volume_header_argb_ = decode_ta_float_color(packet, 16u);
            active_header_oargb_ = 0xFFFFFFFFu;
        } else {
            active_header_oargb_ = decode_ta_float_color(packet, 16u);
        }
        intensity_face_color_valid_ = true;
        pending_intensity_header_ = false;
        return;
    }
    if (pending_modifier_vertex_packet_) {
        if (!active_modifier_volume_ || *active_modifier_volume_ >= modifier_volumes_.size())
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidListOrder,
                "TA-Modifier-Vertex besitzt keinen aktiven Volume-Header.");
        const auto first = std::span<const std::uint8_t>(*pending_modifier_vertex_packet_);
        std::array<PvrVertex, 3u> triangle{
            PvrVertex{std::bit_cast<float>(ta_u32(first, 4u)),
                      std::bit_cast<float>(ta_u32(first, 8u)),
                      std::bit_cast<float>(ta_u32(first, 12u))},
            PvrVertex{std::bit_cast<float>(ta_u32(first, 16u)),
                      std::bit_cast<float>(ta_u32(first, 20u)),
                      std::bit_cast<float>(ta_u32(first, 24u))},
            PvrVertex{std::bit_cast<float>(ta_u32(first, 28u)),
                      std::bit_cast<float>(ta_u32(packet, 0u)),
                      std::bit_cast<float>(ta_u32(packet, 4u))}};
        for (const auto& vertex : triangle) {
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
                !std::isfinite(vertex.z))
                throw PvrTaParserException(
                    PvrTaInputErrorReason::InvalidPacket,
                    "TA-Modifier-Volume besitzt nicht-endliche Koordinaten.");
        }
        modifier_volumes_[*active_modifier_volume_].triangles.push_back(triangle);
        pending_modifier_vertex_packet_.reset();
        metrics_.vertices += 3u;
        return;
    }
    if (pending_extended_vertex_) {
        auto vertex = *pending_extended_vertex_;
        if (active_two_volume_) {
            if (active_uv16_) {
                const auto packed_uv = ta_u32(packet, 0u);
                vertex.volume_u = decode_uv16_component(packed_uv, true);
                vertex.volume_v = decode_uv16_component(packed_uv, false);
            } else {
                vertex.volume_u = std::bit_cast<float>(ta_u32(packet, 0u));
                vertex.volume_v = std::bit_cast<float>(ta_u32(packet, 4u));
            }
            if (active_color_type_ == 0u) {
                vertex.volume_argb = ta_u32(packet, 8u);
                vertex.volume_oargb = ta_u32(packet, 12u);
            } else if (active_color_type_ == 2u || active_color_type_ == 3u) {
                const auto base_intensity = std::clamp(
                    std::bit_cast<float>(ta_u32(packet, 8u)), 0.0f, 1.0f);
                vertex.volume_argb =
                    scale_ta_face_color(active_volume_header_argb_, base_intensity);
                if (active_material_.offset_color_enabled) {
                    const auto offset_intensity = std::clamp(
                        std::bit_cast<float>(ta_u32(packet, 12u)), 0.0f, 1.0f);
                    vertex.volume_oargb =
                        scale_ta_face_color(active_header_oargb_, offset_intensity);
                }
            } else {
                throw PvrTaParserException(
                    PvrTaInputErrorReason::IncompletePacket,
                    "TA-Zwei-Volumen-Vertexformat ist inkonsistent.");
            }
        } else {
            vertex.argb = decode_ta_float_color(packet, 0u);
            vertex.oargb = decode_ta_float_color(packet, 16u);
        }
        if (!std::isfinite(vertex.volume_u) || !std::isfinite(vertex.volume_v))
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidPacket,
                "TA-Zwei-Volumen-Vertex besitzt nicht-endliche UV-Koordinaten.");
        accelerator_.submit_vertex(vertex, pending_extended_end_of_strip_);
        pending_extended_vertex_.reset();
        pending_extended_end_of_strip_ = false;
        ++metrics_.vertices;
        return;
    }
    if (pending_sprite_vertex_) {
        const auto first = std::span<const std::uint8_t>(*pending_sprite_vertex_);
        PvrVertex a{std::bit_cast<float>(ta_u32(first, 4u)),
                    std::bit_cast<float>(ta_u32(first, 8u)),
                    std::bit_cast<float>(ta_u32(first, 12u)),
                    0.0f,
                    0.0f,
                    active_header_argb_,
                    active_header_oargb_};
        PvrVertex b{std::bit_cast<float>(ta_u32(first, 16u)),
                    std::bit_cast<float>(ta_u32(first, 20u)),
                    std::bit_cast<float>(ta_u32(first, 24u)),
                    0.0f,
                    0.0f,
                    active_header_argb_,
                    active_header_oargb_};
        PvrVertex c{std::bit_cast<float>(ta_u32(first, 28u)),
                    std::bit_cast<float>(ta_u32(packet, 0u)),
                    std::bit_cast<float>(ta_u32(packet, 4u)),
                    0.0f,
                    0.0f,
                    active_header_argb_,
                    active_header_oargb_};
        PvrVertex d{std::bit_cast<float>(ta_u32(packet, 8u)),
                    std::bit_cast<float>(ta_u32(packet, 12u)),
                    0.0f,
                    0.0f,
                    0.0f,
                    active_header_argb_,
                    active_header_oargb_};
        d.z = interpolate_sprite_z(a, b, c, d.x, d.y);
        if (active_textured_) {
            const auto auv = ta_u32(packet, 20u);
            const auto buv = ta_u32(packet, 24u);
            const auto cuv = ta_u32(packet, 28u);
            a.u = decode_uv16_component(auv, true);
            a.v = decode_uv16_component(auv, false);
            b.u = decode_uv16_component(buv, true);
            b.v = decode_uv16_component(buv, false);
            c.u = decode_uv16_component(cuv, true);
            c.v = decode_uv16_component(cuv, false);
            d.u = b.u + c.u - a.u;
            d.v = b.v + c.v - a.v;
        }
        for (const auto& vertex : {a, b, c}) accelerator_.submit_vertex(vertex, false);
        accelerator_.submit_vertex(d, true);
        metrics_.vertices += 4u;
        pending_sprite_vertex_.reset();
        return;
    }
    const auto pcw = ta_u32(packet, 0u);
    const auto parameter_type = (pcw >> 29u) & 7u;
    if (parameter_type == 0u) {
        if (!accelerator_.list_open()) return;
        accelerator_.end_list();
        active_modifier_volume_.reset();
        ++metrics_.list_completions;
        if (list_observer_) list_observer_(active_list_);
        return;
    }
    if (parameter_type == 1u) {
        const auto start_x = ta_u32(packet, 12u) & 0x3Fu;
        const auto start_y = ta_u32(packet, 16u) & 0x1Fu;
        const auto end_x = ta_u32(packet, 20u) & 0x3Fu;
        const auto end_y = ta_u32(packet, 24u) & 0x1Fu;
        if (start_x > end_x || start_y > end_y)
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidPacket,
                "TA-Userclip liegt ausserhalb des 32-Pixel-Tilebereichs.");
        user_clip_start_x_ = static_cast<std::uint16_t>(start_x);
        user_clip_start_y_ = static_cast<std::uint16_t>(start_y);
        user_clip_end_x_ = static_cast<std::uint16_t>(end_x);
        user_clip_end_y_ = static_cast<std::uint16_t>(end_y);
        return;
    }
    if (parameter_type == 2u) {
        const auto selected = decode_list_type(pcw);
        if (!accelerator_.list_open()) {
            active_list_ = selected;
            accelerator_.begin_list(selected);
        } else if (selected != active_list_) {
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidListOrder,
                "TA-Objektlistenauswahl wechselt eine offene Liste.");
        }
        return;
    }
    if (parameter_type == 4u) {
        const auto selected = decode_list_type(pcw);
        if (!accelerator_.list_open()) {
            accelerator_.begin_list(selected);
        } else if (selected != active_list_) {
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidListOrder,
                "TA-Polygonheader wechselt eine offene Objektliste.");
        }
        active_list_ = selected;
        const auto mode1 = ta_u32(packet, 4u);
        if (selected == PvrListType::OpaqueModifier ||
            selected == PvrListType::TranslucentModifier) {
            PvrModifierVolume volume;
            volume.list = selected;
            volume.depth_mode = static_cast<std::uint8_t>((mode1 >> 29u) & 7u);
            if (volume.depth_mode > 2u)
                throw PvrTaParserException(
                    PvrTaInputErrorReason::UnsupportedPacket,
                    "TA-Modifier-Volume besitzt einen reservierten Depth-Mode.");
            volume.culling = static_cast<std::uint8_t>((mode1 >> 27u) & 3u);
            volume.volume_last = (pcw & 0x40u) != 0u;
            volume.user_clip_mode = static_cast<std::uint8_t>((pcw >> 16u) & 3u);
            volume.user_clip_start_x = user_clip_start_x_;
            volume.user_clip_start_y = user_clip_start_y_;
            volume.user_clip_end_x = user_clip_end_x_;
            volume.user_clip_end_y = user_clip_end_y_;
            modifier_volumes_.push_back(std::move(volume));
            active_modifier_volume_ = modifier_volumes_.size() - 1u;
            active_sprite_ = false;
            active_two_volume_ = false;
            ++metrics_.polygon_headers;
            return;
        }
        active_modifier_volume_.reset();
        active_uv16_ = (pcw & 0x1u) != 0u;
        active_textured_ = (pcw & 0x8u) != 0u;
        active_color_type_ = static_cast<std::uint8_t>((pcw >> 4u) & 3u);
        active_two_volume_ = (pcw & 0x40u) != 0u;
        active_sprite_ = false;
        const auto mode2 = ta_u32(packet, 8u);
        const auto mode3 = ta_u32(packet, 12u);
        active_material_ = {};
        active_material_.gouraud = (pcw & 2u) != 0u;
        active_material_.textured = active_textured_;
        active_material_.shadow_enabled = (pcw & 0x80u) != 0u;
        active_material_.user_clip_mode = static_cast<std::uint8_t>((pcw >> 16u) & 3u);
        active_material_.user_clip_start_x = user_clip_start_x_;
        active_material_.user_clip_start_y = user_clip_start_y_;
        active_material_.user_clip_end_x = user_clip_end_x_;
        active_material_.user_clip_end_y = user_clip_end_y_;
        active_material_.depth_compare = static_cast<std::uint8_t>((mode1 >> 29u) & 7u);
        active_material_.culling = static_cast<std::uint8_t>((mode1 >> 27u) & 3u);
        active_material_.depth_write = (mode1 & 0x04000000u) == 0u;
        active_material_.offset_color_enabled = (pcw & 0x4u) != 0u;
        decode_ta_texture_words(active_material_, mode2, mode3);
        if (active_two_volume_) {
            if (active_color_type_ == 1u)
                throw PvrTaParserException(
                    PvrTaInputErrorReason::UnsupportedPacket,
                    "TA-Zwei-Volumen-Modus besitzt kein Floating-Color-Vertexformat.");
            active_material_.volume_material = std::make_shared<PvrMaterial>(active_material_);
            active_material_.volume_material->volume_material.reset();
            decode_ta_texture_words(*active_material_.volume_material,
                                    ta_u32(packet, 16u),
                                    ta_u32(packet, 20u));
        }
        if (active_color_type_ == 2u) {
            if (active_two_volume_ || active_material_.offset_color_enabled) {
                pending_intensity_header_ = true;
            } else {
                active_header_argb_ = decode_ta_float_color(packet, 16u);
                active_header_oargb_ = 0u;
                intensity_face_color_valid_ = true;
            }
        } else if (active_color_type_ == 3u && !intensity_face_color_valid_) {
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidListOrder,
                "TA-Intensity-Mode 2 wurde vor einer Face-Color aus Mode 1 verwendet.");
        }
        accelerator_.set_material(active_material_);
        ++metrics_.polygon_headers;
        return;
    }
    if (parameter_type == 5u) {
        const auto selected = decode_list_type(pcw);
        if (!accelerator_.list_open()) {
            accelerator_.begin_list(selected);
        } else if (selected != active_list_) {
            throw PvrTaParserException(
                PvrTaInputErrorReason::InvalidListOrder,
                "TA-Spriteheader wechselt eine offene Objektliste.");
        }
        active_list_ = selected;
        active_textured_ = (pcw & 0x8u) != 0u;
        active_two_volume_ = false;
        active_sprite_ = true;
        active_header_argb_ = ta_u32(packet, 16u);
        active_header_oargb_ = ta_u32(packet, 20u);
        const auto mode1 = ta_u32(packet, 4u);
        const auto mode2 = ta_u32(packet, 8u);
        const auto mode3 = ta_u32(packet, 12u);
        active_material_ = {};
        active_material_.gouraud = false;
        active_material_.textured = active_textured_;
        active_material_.shadow_enabled = (pcw & 0x80u) != 0u;
        active_material_.user_clip_mode = static_cast<std::uint8_t>((pcw >> 16u) & 3u);
        active_material_.user_clip_start_x = user_clip_start_x_;
        active_material_.user_clip_start_y = user_clip_start_y_;
        active_material_.user_clip_end_x = user_clip_end_x_;
        active_material_.user_clip_end_y = user_clip_end_y_;
        active_material_.depth_compare = static_cast<std::uint8_t>((mode1 >> 29u) & 7u);
        active_material_.depth_write = (mode1 & 0x04000000u) == 0u;
        active_material_.offset_color_enabled = (pcw & 0x4u) != 0u;
        decode_ta_texture_words(active_material_, mode2, mode3);
        accelerator_.set_material(active_material_);
        ++metrics_.polygon_headers;
        return;
    }
    if (parameter_type == 7u) {
        if (active_list_ == PvrListType::OpaqueModifier ||
            active_list_ == PvrListType::TranslucentModifier) {
            if (!active_modifier_volume_ || *active_modifier_volume_ >= modifier_volumes_.size())
                throw PvrTaParserException(
                    PvrTaInputErrorReason::InvalidListOrder,
                    "TA-Modifier-Vertex wurde vor einem Volume-Header gesendet.");
            std::array<std::uint8_t, 32u> first{};
            std::copy(packet.begin(), packet.end(), first.begin());
            pending_modifier_vertex_packet_ = first;
            return;
        }
        if (active_sprite_) {
            std::array<std::uint8_t, 32u> first{};
            std::copy(packet.begin(), packet.end(), first.begin());
            pending_sprite_vertex_ = first;
            return;
        }
        PvrVertex vertex;
        vertex.x = std::bit_cast<float>(ta_u32(packet, 4u));
        vertex.y = std::bit_cast<float>(ta_u32(packet, 8u));
        vertex.z = std::bit_cast<float>(ta_u32(packet, 12u));
        std::size_t color_offset = active_two_volume_ && !active_textured_ ? 16u : 24u;
        if (active_textured_) {
            if (active_uv16_) {
                const auto packed_uv = ta_u32(packet, 16u);
                vertex.u = decode_uv16_component(packed_uv, true);
                vertex.v = decode_uv16_component(packed_uv, false);
                color_offset = 24u;
            } else {
                vertex.u = std::bit_cast<float>(ta_u32(packet, 16u));
                vertex.v = std::bit_cast<float>(ta_u32(packet, 20u));
                color_offset = 24u;
            }
        }
        if (active_color_type_ == 1u) {
            if (active_textured_) {
                pending_extended_vertex_ = vertex;
                pending_extended_end_of_strip_ = (pcw & 0x10000000u) != 0u;
                return;
            }
            vertex.argb = decode_ta_float_color(packet, 16u);
        } else if (active_color_type_ == 2u || active_color_type_ == 3u) {
            const auto base_intensity = std::clamp(
                std::bit_cast<float>(ta_u32(packet, color_offset)), 0.0f, 1.0f);
            auto offset_intensity = 1.0f;
            if (active_textured_ && active_material_.offset_color_enabled) {
                offset_intensity = std::clamp(
                    std::bit_cast<float>(ta_u32(packet, color_offset + 4u)), 0.0f, 1.0f);
            }
            vertex.argb = scale_ta_face_color(active_header_argb_, base_intensity);
            vertex.oargb = scale_ta_face_color(active_header_oargb_, offset_intensity);
        } else {
            vertex.argb = ta_u32(packet, color_offset);
            if (active_two_volume_ && !active_textured_)
                vertex.volume_argb = ta_u32(packet, color_offset + 4u);
            else if (color_offset + 4u < packet.size())
                vertex.oargb = ta_u32(packet, color_offset + 4u);
        }
        if (active_two_volume_ && !active_textured_ &&
            (active_color_type_ == 2u || active_color_type_ == 3u)) {
            const auto volume_intensity = std::clamp(
                std::bit_cast<float>(ta_u32(packet, color_offset + 4u)), 0.0f, 1.0f);
            vertex.volume_argb =
                scale_ta_face_color(active_volume_header_argb_, volume_intensity);
        }
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z) || !std::isfinite(vertex.u) || !std::isfinite(vertex.v))
            throw PvrTaParserException(PvrTaInputErrorReason::InvalidPacket,
                                       "TA-Vertex besitzt nicht-endliche Koordinaten.");
        if (active_two_volume_ && active_textured_) {
            pending_extended_vertex_ = vertex;
            pending_extended_end_of_strip_ = (pcw & 0x10000000u) != 0u;
            return;
        }
        accelerator_.submit_vertex(vertex, (pcw & 0x10000000u) != 0u);
        ++metrics_.vertices;
        return;
    }
    throw PvrTaParserException(
        PvrTaInputErrorReason::UnsupportedPacket,
        "TA-Parametertyp ist noch nicht in den Polygonpfad integrierbar.");
}

PvrTaSubmitResult PvrTaFifo::submit_guest(const std::span<const std::uint8_t> packet) {
    const auto metrics_before = metrics_;
    try {
        submit(packet);
    } catch (const PvrTaParserException& error) {
        // A rejected guest packet may have reached a late parser check. Quiesce all
        // partially mutated frame state, but preserve cumulative accepted/rejected
        // diagnostics and the first-error contract.
        metrics_ = metrics_before;
        return reject_guest(error.reason(), error.what());
    } catch (...) {
        discard_frame_state();
        metrics_ = metrics_before;
        throw;
    }
    return {true, std::nullopt};
}

PvrTaSubmitResult PvrTaFifo::reject_guest(const PvrTaInputErrorReason reason,
                                          std::string detail) {
    const auto packet_sequence = metrics_.packets + metrics_.rejected_packets + 1u;
    discard_frame_state();
    ++metrics_.rejected_packets;
    PvrTaInputError fault{reason, packet_sequence, std::move(detail)};
    if (!first_input_error_) first_input_error_ = fault;
    return {false, std::move(fault)};
}

PvrTaFrame PvrTaFifo::finish_frame() {
    if (pending_sprite_vertex_ || pending_extended_vertex_ || pending_intensity_header_ ||
        pending_modifier_vertex_packet_)
        throw PvrTaParserException(PvrTaInputErrorReason::IncompletePacket,
                                   "TA-Frame endet innerhalb eines 64-Byte-Parameters.");
    auto frame = accelerator_.finish_frame();
    frame.modifier_volumes = std::move(modifier_volumes_);
    modifier_volumes_.clear();
    active_modifier_volume_.reset();
    ++metrics_.frames;
    frame_packets_ = 0u;
    return frame;
}

const PvrTaMetrics& PvrTaFifo::metrics() const noexcept {
    return metrics_;
}

PvrTaFifoSnapshot PvrTaFifo::snapshot() const {
    return {
        accelerator_.snapshot(),
        active_list_,
        active_textured_,
        active_uv16_,
        active_color_type_,
        active_sprite_,
        active_two_volume_,
        active_header_argb_,
        active_header_oargb_,
        active_volume_header_argb_,
        intensity_face_color_valid_,
        active_material_,
        user_clip_start_x_,
        user_clip_start_y_,
        user_clip_end_x_,
        user_clip_end_y_,
        pending_sprite_vertex_,
        pending_extended_vertex_,
        pending_intensity_header_,
        pending_extended_end_of_strip_,
        modifier_volumes_,
        active_modifier_volume_,
        pending_modifier_vertex_packet_,
        metrics_,
        frame_packets_,
        first_input_error_,
    };
}

void PvrTaFifo::validate_state_restore(
    const PvrTaFifoSnapshot& state) const {
    accelerator_.validate_state_restore(state.accelerator);
    if (!valid_pvr_list_type(state.active_list) ||
        state.active_color_type > 3u ||
        state.user_clip_start_x > 1023u ||
        state.user_clip_end_x > 1023u ||
        state.user_clip_start_y > 1023u ||
        state.user_clip_end_y > 1023u ||
        state.frame_packets > pvr_ta_maximum_frame_packets ||
        state.metrics.packets < state.frame_packets)
        throw std::invalid_argument(
            "PVR-TA-FIFO-Handoff besitzt ungueltige Parserdaten.");
    validate_pvr_material(state.active_material);
    if (state.pending_extended_vertex)
        validate_pvr_vertex(*state.pending_extended_vertex);
    for (const auto& volume : state.modifier_volumes)
        validate_pvr_modifier_volume(volume);
    if (state.active_modifier_volume &&
        *state.active_modifier_volume >= state.modifier_volumes.size())
        throw std::invalid_argument(
            "PVR-TA-FIFO-Handoff besitzt einen ungueltigen Modifierindex.");
    if (state.first_input_error) {
        if (state.metrics.rejected_packets == 0u ||
            !valid_pvr_input_error_reason(
                state.first_input_error->reason) ||
            state.first_input_error->packet == 0u)
            throw std::invalid_argument(
                "PVR-TA-FIFO-Handoff besitzt ungueltige Fehlerdaten.");
    } else if (state.metrics.rejected_packets != 0u) {
        throw std::invalid_argument(
            "PVR-TA-FIFO-Handoff hat Ablehnungen ohne ersten Fehler.");
    }
}

void PvrTaFifo::restore_state_passive(PvrTaFifoSnapshot state) {
    validate_state_restore(state);
    detach_pvr_material_graphs(state);
    accelerator_.restore_state_passive(std::move(state.accelerator));
    active_list_ = state.active_list;
    active_textured_ = state.active_textured;
    active_uv16_ = state.active_uv16;
    active_color_type_ = state.active_color_type;
    active_sprite_ = state.active_sprite;
    active_two_volume_ = state.active_two_volume;
    active_header_argb_ = state.active_header_argb;
    active_header_oargb_ = state.active_header_oargb;
    active_volume_header_argb_ = state.active_volume_header_argb;
    intensity_face_color_valid_ = state.intensity_face_color_valid;
    active_material_ = std::move(state.active_material);
    user_clip_start_x_ = state.user_clip_start_x;
    user_clip_start_y_ = state.user_clip_start_y;
    user_clip_end_x_ = state.user_clip_end_x;
    user_clip_end_y_ = state.user_clip_end_y;
    pending_sprite_vertex_ = std::move(state.pending_sprite_vertex);
    pending_extended_vertex_ = std::move(state.pending_extended_vertex);
    pending_intensity_header_ = state.pending_intensity_header;
    pending_extended_end_of_strip_ =
        state.pending_extended_end_of_strip;
    modifier_volumes_ = std::move(state.modifier_volumes);
    active_modifier_volume_ = state.active_modifier_volume;
    pending_modifier_vertex_packet_ =
        std::move(state.pending_modifier_vertex_packet);
    metrics_ = state.metrics;
    frame_packets_ = state.frame_packets;
    first_input_error_ = std::move(state.first_input_error);
}

void PvrTaFifo::continue_list() {
    if (accelerator_.list_open() || pending_sprite_vertex_ || pending_extended_vertex_ ||
        pending_intensity_header_ || pending_modifier_vertex_packet_)
        throw PvrTaParserException(
            PvrTaInputErrorReason::IncompletePacket,
            "TA-Listenfortsetzung beginnt innerhalb eines Parameters oder einer Liste.");
    active_list_ = PvrListType::Opaque;
    active_textured_ = false;
    active_uv16_ = false;
    active_color_type_ = 0u;
    active_sprite_ = false;
    active_two_volume_ = false;
    active_header_argb_ = 0xFFFFFFFFu;
    active_header_oargb_ = 0u;
    active_volume_header_argb_ = 0xFFFFFFFFu;
    intensity_face_color_valid_ = false;
    active_material_ = {};
    user_clip_start_x_ = 0u;
    user_clip_start_y_ = 0u;
    user_clip_end_x_ = 0u;
    user_clip_end_y_ = 0u;
    pending_extended_end_of_strip_ = false;
    active_modifier_volume_.reset();
    ++metrics_.continuations;
}

void PvrTaFifo::discard_frame_state() noexcept {
    accelerator_ = TileAccelerator{};
    active_list_ = PvrListType::Opaque;
    active_textured_ = false;
    active_uv16_ = false;
    active_color_type_ = 0u;
    active_sprite_ = false;
    active_two_volume_ = false;
    active_header_argb_ = 0xFFFFFFFFu;
    active_header_oargb_ = 0u;
    active_volume_header_argb_ = 0xFFFFFFFFu;
    intensity_face_color_valid_ = false;
    active_material_ = {};
    user_clip_start_x_ = 0u;
    user_clip_start_y_ = 0u;
    user_clip_end_x_ = 0u;
    user_clip_end_y_ = 0u;
    pending_sprite_vertex_.reset();
    pending_extended_vertex_.reset();
    pending_intensity_header_ = false;
    pending_extended_end_of_strip_ = false;
    modifier_volumes_.clear();
    active_modifier_volume_.reset();
    pending_modifier_vertex_packet_.reset();
    frame_packets_ = 0u;
}

void PvrTaFifo::reset() noexcept {
    discard_frame_state();
    metrics_ = {};
    first_input_error_.reset();
}

bool PvrChannel2DestinationPlan::destination_progresses() const noexcept {
    return kind == PvrChannel2DestinationKind::DirectTexture64 ||
           kind == PvrChannel2DestinationKind::DirectTexture32;
}

std::uint32_t
PvrChannel2DestinationPlan::destination_for_unit(const std::size_t unit) const {
    if (unit >= unit_count)
        throw std::out_of_range("PVR-Channel-2-Zieleinheit liegt ausserhalb des Transfers.");
    if (!destination_progresses()) return initial_address;
    const auto address = static_cast<std::uint64_t>(initial_address) +
                         static_cast<std::uint64_t>(unit) *
                             pvr_channel2_transfer_unit_size;
    if (address > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("PVR-Channel-2-Zieladresse ist uebergelaufen.");
    return static_cast<std::uint32_t>(address);
}

PvrChannel2DestinationPlan
plan_pvr_channel2_destination(const std::uint32_t destination,
                              const std::size_t byte_count) {
    constexpr std::size_t maximum_channel2_length = 0x00FFFFE0u;
    constexpr std::uint64_t direct_texture_window_size = 0x01000000u;
    if (byte_count == 0u || byte_count > maximum_channel2_length ||
        (byte_count % pvr_channel2_transfer_unit_size) != 0u)
        throw std::invalid_argument(
            "PVR-Channel-2-Transfer braucht eine gueltige positive 32-Byte-Laenge.");
    if ((destination & (pvr_channel2_transfer_unit_size - 1u)) != 0u)
        throw std::invalid_argument(
            "PVR-Channel-2-Zieladresse muss auf 32 Byte ausgerichtet sein.");

    const auto window = destination & 0xFF000000u;
    PvrChannel2DestinationKind kind;
    switch (window) {
    case 0x10000000u:
    case 0x12000000u:
        kind = (destination & 0x00800000u) == 0u
                   ? PvrChannel2DestinationKind::TaFifo
                   : PvrChannel2DestinationKind::YuvConverter;
        break;
    case 0x11000000u:
        kind = PvrChannel2DestinationKind::DirectTexture64;
        break;
    case 0x13000000u:
        kind = PvrChannel2DestinationKind::DirectTexture32;
        break;
    default:
        throw std::out_of_range(
            "PVR-Channel-2-Ziel liegt ausserhalb der Area-4-PVR-Fenster.");
    }

    if (kind == PvrChannel2DestinationKind::DirectTexture64 ||
        kind == PvrChannel2DestinationKind::DirectTexture32) {
        const auto end = static_cast<std::uint64_t>(destination) + byte_count;
        const auto window_end = static_cast<std::uint64_t>(window) +
                                direct_texture_window_size;
        if (end > window_end)
            throw std::out_of_range(
                "PVR-Direct-Texture-Transfer verlaesst sein 16-MiB-Zielfenster.");
    }
    return {
        destination,
        byte_count,
        byte_count / pvr_channel2_transfer_unit_size,
        kind,
    };
}

PvrTaFifoMemoryDevice::PvrTaFifoMemoryDevice(std::shared_ptr<PvrTaFifo> fifo,
                                             std::shared_ptr<PvrRegisterFile> registers)
    : fifo_(std::move(fifo)), registers_(std::move(registers)) {
    if (!fifo_) throw std::invalid_argument("TA-FIFO-Apertur braucht eine FIFO-Instanz.");
}

std::size_t PvrTaFifoMemoryDevice::size() const noexcept {
    return aperture_size;
}

std::uint8_t PvrTaFifoMemoryDevice::read_u8(const std::uint32_t) const {
    throw std::runtime_error("Die TA-FIFO-Apertur unterstuetzt keine Lesezugriffe.");
}

void PvrTaFifoMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    if (offset >= aperture_size) throw std::out_of_range("TA-FIFO-Aperturoffset ist ungueltig.");
    const auto base = offset & ~std::uint32_t{31u};
    const auto byte = offset & 31u;
    if (!packet_active_) {
        packet_active_ = true;
        packet_base_ = base;
        written_mask_ = 0u;
        packet_.fill(0u);
    } else if (base != packet_base_) {
        reset();
        const auto rejection = fifo_->reject_guest(
            PvrTaInputErrorReason::IncompletePacket,
            "TA-FIFO-Schreibfolge verlaesst ein unvollstaendiges 32-Byte-Parameterpaket.");
        throw PvrTaParserException(rejection.error->reason, rejection.error->detail);
    }
    const auto bit = std::uint32_t{1u} << byte;
    if ((written_mask_ & bit) != 0u) {
        reset();
        const auto rejection = fifo_->reject_guest(
            PvrTaInputErrorReason::InvalidPacket,
            "TA-FIFO-Parameterbyte wurde vor Completion doppelt geschrieben.");
        throw PvrTaParserException(rejection.error->reason, rejection.error->detail);
    }
    packet_[byte] = value;
    written_mask_ |= bit;
    if (written_mask_ == std::numeric_limits<std::uint32_t>::max()) {
        const auto completed_packet = packet_;
        reset();
        const auto result = fifo_->submit_guest(completed_packet);
        if (!result.accepted) {
            throw PvrTaParserException(
                result.error ? result.error->reason : PvrTaInputErrorReason::InvalidPacket,
                result.error ? result.error->detail
                             : "TA-FIFO-Aperturpaket wurde ohne Detail abgelehnt.");
        }
        if (registers_) registers_->record_ta_packet(32u);
    }
}

PvrTaFifoMemoryDevice::Snapshot PvrTaFifoMemoryDevice::snapshot() const noexcept {
    return {packet_, packet_base_, written_mask_, packet_active_};
}

void PvrTaFifoMemoryDevice::validate_state_restore(
    const Snapshot& state) const {
    if (!state.packet_active) {
        if (state.packet_base != 0u || state.written_mask != 0u ||
            std::any_of(state.packet.begin(),
                        state.packet.end(),
                        [](const auto byte) { return byte != 0u; }))
            throw std::invalid_argument(
                "PVR-TA-Apertur-Handoff besitzt inaktive Paketdaten.");
        return;
    }
    if ((state.packet_base & 31u) != 0u ||
        state.packet_base >= aperture_size ||
        state.written_mask == 0u ||
        state.written_mask ==
            std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "PVR-TA-Apertur-Handoff besitzt einen ungueltigen Paketstatus.");
    for (std::size_t byte = 0u; byte < state.packet.size(); ++byte) {
        if ((state.written_mask & (std::uint32_t{1u} << byte)) == 0u &&
            state.packet[byte] != 0u)
            throw std::invalid_argument(
                "PVR-TA-Apertur-Handoff besitzt ungeschriebene Nutzdaten.");
    }
}

void PvrTaFifoMemoryDevice::restore_state_passive(Snapshot state) {
    validate_state_restore(state);
    packet_ = std::move(state.packet);
    packet_base_ = state.packet_base;
    written_mask_ = state.written_mask;
    packet_active_ = state.packet_active;
}

void PvrTaFifoMemoryDevice::reset() noexcept {
    packet_.fill(0u);
    packet_base_ = 0u;
    written_mask_ = 0u;
    packet_active_ = false;
}

PvrYuvConverterMemoryDevice::PvrYuvConverterMemoryDevice(
    std::shared_ptr<PvrRegisterFile> registers,
    std::shared_ptr<LinearMemoryDevice> vram,
    std::function<void()> completion_observer)
    : registers_(std::move(registers)), vram_(std::move(vram)),
      completion_observer_(std::move(completion_observer)) {
    if (!registers_ || !vram_)
        throw std::invalid_argument("PVR-YUV-Konverter braucht Register und VRAM.");
    input_.reserve(512u);
}

std::size_t PvrYuvConverterMemoryDevice::size() const noexcept {
    return aperture_size;
}

std::uint8_t PvrYuvConverterMemoryDevice::read_u8(const std::uint32_t) const {
    throw std::runtime_error("Die PVR-YUV-Apertur unterstuetzt keine Lesezugriffe.");
}

void PvrYuvConverterMemoryDevice::refresh_configuration() {
    const auto configuration = registers_->read(pvr_register::YuvConfig);
    const auto destination = registers_->read(pvr_register::YuvAddress) & 0x007FFFFFu;
    if (configuration == configuration_ && destination == destination_) return;
    configuration_ = configuration;
    destination_ = destination;
    frame_macroblock_ = 0u;
    input_.clear();
    registers_->write(pvr_register::YuvStatus, 0u);
}

void PvrYuvConverterMemoryDevice::write_u8(const std::uint32_t offset,
                                            const std::uint8_t value) {
    if (offset >= aperture_size)
        throw std::out_of_range("PVR-YUV-Aperturoffset ist ungueltig.");
    refresh_configuration();
    const bool yuv422 = (configuration_ & 0x01000000u) != 0u;
    const auto macroblock_size = yuv422 ? 512u : 384u;
    input_.push_back(value);
    if (input_.size() == macroblock_size) convert_macroblock();
}

void PvrYuvConverterMemoryDevice::set_guest_memory_access_memory(Memory* const memory) noexcept {
    guest_memory_access_memory_ = memory;
}

void PvrYuvConverterMemoryDevice::reset() noexcept {
    input_.clear();
    configuration_ = std::numeric_limits<std::uint32_t>::max();
    destination_ = std::numeric_limits<std::uint32_t>::max();
    frame_macroblock_ = 0u;
    converted_macroblocks_ = 0u;
}

void PvrYuvConverterMemoryDevice::convert_macroblock() {
    const bool yuv422 = (configuration_ & 0x01000000u) != 0u;
    const auto blocks_x = (configuration_ & 0x3Fu) + 1u;
    const auto blocks_y = ((configuration_ >> 8u) & 0x3Fu) + 1u;
    const auto total_blocks = static_cast<std::uint64_t>(blocks_x) * blocks_y;
    if (frame_macroblock_ >= total_blocks)
        throw std::runtime_error("PVR-YUV-Eingabe ueberschreitet die konfigurierte Framegroesse.");
    const auto block_x = frame_macroblock_ % blocks_x;
    const auto block_y = frame_macroblock_ / blocks_x;
    const auto output_width = static_cast<std::uint64_t>(blocks_x) * 16u;
    const auto frame_bytes = output_width * static_cast<std::uint64_t>(blocks_y) * 16u * 2u;
    if (static_cast<std::uint64_t>(destination_) + frame_bytes > vram_->size())
        throw std::out_of_range("PVR-YUV-Ausgabe liegt ausserhalb des VRAM.");
    const bool trace_writes = guest_memory_access_memory_ != nullptr &&
                              guest_memory_access_memory_->has_guest_memory_access_sink();

    const auto y_sample = [&](const std::uint32_t x, const std::uint32_t y) {
        if (!yuv422) {
            const auto quadrant = (y / 8u) * 2u + x / 8u;
            return input_[128u + quadrant * 64u + (y & 7u) * 8u + (x & 7u)];
        }
        const auto half = y / 8u;
        const auto quadrant = x / 8u;
        return input_[half * 256u + 128u + quadrant * 64u + (y & 7u) * 8u +
                      (x & 7u)];
    };
    const auto chroma_sample = [&](const bool v_plane,
                                   const std::uint32_t x,
                                   const std::uint32_t y) {
        if (!yuv422)
            return input_[(v_plane ? 64u : 0u) + (y / 2u) * 8u + x / 2u];
        const auto half = y / 8u;
        return input_[half * 256u + (v_plane ? 64u : 0u) + (y & 7u) * 8u + x / 2u];
    };
    for (std::uint32_t y = 0u; y < 16u; ++y) {
        for (std::uint32_t x = 0u; x < 16u; x += 2u) {
            const auto u = chroma_sample(false, x, y);
            const auto v = chroma_sample(true, x, y);
            const auto global_x = static_cast<std::uint64_t>(block_x) * 16u + x;
            const auto global_y = static_cast<std::uint64_t>(block_y) * 16u + y;
            const auto output = static_cast<std::uint64_t>(destination_) +
                                (global_y * output_width + global_x) * 2u;
            const auto first_offset = static_cast<std::uint32_t>(output);
            const auto first_value = static_cast<std::uint16_t>(y_sample(x, y) << 8u | u);
            const bool first_changed =
                !trace_writes || vram_->read_u16(first_offset) != first_value;
            vram_->write_u16(first_offset, first_value);
            if (trace_writes)
                notify_pvr_vram_write(
                    guest_memory_access_memory_,
                    GuestMemoryAccessOrigin::PvrYuv,
                    *vram_,
                    dreamcast_vram_64bit_physical_bases.front() + first_offset,
                    first_offset,
                    MemoryAccessWidth::Halfword,
                    first_value,
                    first_changed);

            const auto second_offset = static_cast<std::uint32_t>(output + 2u);
            const auto second_value =
                static_cast<std::uint16_t>(y_sample(x + 1u, y) << 8u | v);
            const bool second_changed =
                !trace_writes || vram_->read_u16(second_offset) != second_value;
            vram_->write_u16(second_offset, second_value);
            if (trace_writes)
                notify_pvr_vram_write(
                    guest_memory_access_memory_,
                    GuestMemoryAccessOrigin::PvrYuv,
                    *vram_,
                    dreamcast_vram_64bit_physical_bases.front() + second_offset,
                    second_offset,
                    MemoryAccessWidth::Halfword,
                    second_value,
                    second_changed);
        }
    }
    input_.clear();
    ++frame_macroblock_;
    ++converted_macroblocks_;
    registers_->write(pvr_register::YuvStatus, frame_macroblock_);
    if (frame_macroblock_ == total_blocks && completion_observer_) completion_observer_();
}

std::uint64_t PvrYuvConverterMemoryDevice::converted_macroblocks() const noexcept {
    return converted_macroblocks_;
}

PvrYuvConverterMemoryDevice::Snapshot PvrYuvConverterMemoryDevice::snapshot() const {
    return {
        input_,
        configuration_,
        destination_,
        frame_macroblock_,
        converted_macroblocks_,
        guest_memory_access_memory_ != nullptr,
    };
}

void PvrYuvConverterMemoryDevice::validate_state_restore(
    const Snapshot& state) const {
    if (state.guest_memory_access_bound !=
        (guest_memory_access_memory_ != nullptr))
        throw std::invalid_argument(
            "PVR-YUV-Handoff passt nicht zum Runtime-Memory-Sink-Vertrag.");
    const auto unconfigured =
        state.configuration == std::numeric_limits<std::uint32_t>::max() &&
        state.destination == std::numeric_limits<std::uint32_t>::max();
    if (unconfigured) {
        if (!state.input.empty() || state.frame_macroblock != 0u)
            throw std::invalid_argument(
                "PVR-YUV-Handoff besitzt Daten ohne Konfiguration.");
        return;
    }
    if (state.configuration == std::numeric_limits<std::uint32_t>::max() ||
        state.destination == std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "PVR-YUV-Handoff besitzt eine unvollstaendige Konfiguration.");
    const auto macroblock_size =
        (state.configuration & 0x01000000u) != 0u ? 512u : 384u;
    if (state.input.size() >= macroblock_size)
        throw std::invalid_argument(
            "PVR-YUV-Handoff besitzt einen uebervollen Eingabepuffer.");
    const auto blocks_x =
        static_cast<std::uint64_t>(state.configuration & 0x3Fu) + 1u;
    const auto blocks_y =
        static_cast<std::uint64_t>(
            (state.configuration >> 8u) & 0x3Fu) +
        1u;
    const auto total_blocks = blocks_x * blocks_y;
    if (state.frame_macroblock > total_blocks)
        throw std::invalid_argument(
            "PVR-YUV-Handoff besitzt einen ungueltigen Makroblockindex.");
    const auto frame_bytes = blocks_x * 16u * blocks_y * 16u * 2u;
    if (state.destination >= vram_->size() ||
        frame_bytes >
            static_cast<std::uint64_t>(vram_->size()) -
                state.destination)
        throw std::invalid_argument(
            "PVR-YUV-Handoff-Ziel liegt ausserhalb des Runtime-VRAM.");
}

void PvrYuvConverterMemoryDevice::restore_state_passive(Snapshot state) {
    validate_state_restore(state);
    input_ = std::move(state.input);
    configuration_ = state.configuration;
    destination_ = state.destination;
    frame_macroblock_ = state.frame_macroblock;
    converted_macroblocks_ = state.converted_macroblocks;
}

void PvrSoftwareRenderer::render(const PvrTaFrame& frame,
                                 const PvrRegisterFile& registers,
                                 LinearMemoryDevice& vram) {
    render(frame, registers.snapshot(), vram);
}

void PvrSoftwareRenderer::render(const PvrTaFrame& frame,
                                 const PvrRegisterSnapshot& registers,
                                 LinearMemoryDevice& vram) {
    render(frame, registers, vram, next_render_generation_);
}

void PvrSoftwareRenderer::render(const PvrTaFrame& frame,
                                 const PvrRegisterSnapshot& registers,
                                 LinearMemoryDevice& vram,
                                 const std::uint64_t render_generation) {
    if (render_generation == 0u)
        throw std::invalid_argument("PVR-Rendergeneration Null ist ungueltig.");
    if (render_generation <= last_render_generation_)
        throw std::logic_error("PVR-Rendergeneration ist nicht streng monoton.");
    std::uint64_t frame_pixel_writes = 0u;
    const auto x_clip = registers.read(pvr_register::FramebufferXClip);
    const auto y_clip = registers.read(pvr_register::FramebufferYClip);
    const auto minimum_clip_x = x_clip & 0x7FFu;
    const auto maximum_clip_x = (x_clip >> 16u) & 0x7FFu;
    const auto minimum_clip_y = y_clip & 0x3FFu;
    const auto maximum_clip_y = (y_clip >> 16u) & 0x3FFu;
    if (maximum_clip_x < minimum_clip_x || maximum_clip_y < minimum_clip_y)
        throw std::invalid_argument("PVR-Renderclip besitzt vertauschte Grenzen.");
    const auto width = maximum_clip_x + 1u;
    const auto height = maximum_clip_y + 1u;
    if (width > 2048u || height > 1024u)
        throw std::out_of_range("PVR-Renderclip ist ausserhalb der Hardwaregrenzen.");
    const auto scaler = registers.read(pvr_register::ScalerControl);
    const auto scaler_interlaced = (scaler & 0x00020000u) != 0u;
    const auto scaler_field = (scaler & 0x00040000u) != 0u;
    const auto write_sof = scaler_interlaced && scaler_field
                               ? pvr_register::FramebufferWriteSof2
                               : pvr_register::FramebufferWriteSof1;
    const auto write_start = registers.read(write_sof);
    const auto base = write_start & 0x007FFFFCu;
    const auto pack_mode = registers.read(pvr_register::FramebufferWriteControl) & 7u;
    const auto pixel_bytes = render_bytes_per_pixel(pack_mode);
    const auto configured_modulo =
        static_cast<std::uint64_t>(registers.read(pvr_register::FramebufferRenderModulo) &
                                   0x3FFu) *
        8u;
    const auto minimum_stride = static_cast<std::uint64_t>(width) * pixel_bytes;
    const auto stride = configured_modulo == 0u ? minimum_stride : configured_modulo;
    if (stride < minimum_stride)
        throw std::invalid_argument("PVR-Render-Modulo ist kleiner als die Clipbreite.");
    if (base + stride * height > vram.size())
        throw std::out_of_range("PVR-Renderziel liegt ausserhalb des VRAM.");
    const auto render_pixel_count = static_cast<std::size_t>(width) * height;
    Memory* const trace_memory =
        guest_memory_access_memory_ != nullptr &&
                guest_memory_access_memory_->has_guest_memory_access_sink()
            ? guest_memory_access_memory_
            : nullptr;
    std::vector<std::uint32_t> original_pixels(render_pixel_count, 0u);
    std::vector<std::uint8_t> touched_pixels(render_pixel_count, 0u);
    const auto read_packed_pixel = [&](const std::uint32_t offset) {
        const auto backing = dreamcast_vram_32bit_to_linear_offset(
            offset & static_cast<std::uint32_t>(dreamcast_vram_size - 1u));
        return pixel_bytes == 2u ? static_cast<std::uint32_t>(vram.read_u16(backing))
                                 : vram.read_u32(backing);
    };
    const auto write_pixel = [&](const std::uint32_t offset,
                                 const std::size_t pixel_index,
                                 const Rgba8 color) {
        ++frame_pixel_writes;
        if (touched_pixels[pixel_index] == 0u) {
            touched_pixels[pixel_index] = 1u;
            original_pixels[pixel_index] = read_packed_pixel(offset);
        }
        static_cast<void>(write_render_pixel(
            vram,
            trace_memory,
            offset,
            pack_mode,
            color.a,
            color.r,
            color.g,
            color.b));
    };
    std::vector<float> depth(render_pixel_count,
                             -std::numeric_limits<float>::infinity());
    std::vector<std::uint8_t> shadow_eligible(static_cast<std::size_t>(width) * height, 0u);
    std::vector<std::uint8_t> volume_material_eligible(shadow_eligible.size(), 0u);
    std::vector<Rgba8> secondary_accumulation(
        shadow_eligible.size(), Rgba8{0u, 0u, 0u, 0u});
    const std::vector<std::uint8_t>* volume_selection_mask = nullptr;

    const auto edge = [](const PvrVertex& a, const PvrVertex& b, const float x, const float y) {
        return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    };
    const auto background_config = registers.read(pvr_register::BackgroundPlaneConfig);
    const auto tag_offset = background_config & 0x7u;
    const auto tag_address = (background_config >> 3u) & 0x001FFFFFu;
    const auto skip = (background_config >> 24u) & 0x7u;
    const auto shadow = (background_config & 0x08000000u) != 0u;
    const auto parameter_bank = registers.read(pvr_register::ParameterBase) & 0x00F00000u;
    const auto background_address =
        (static_cast<std::uint64_t>(parameter_bank) +
         static_cast<std::uint64_t>(tag_address) * 4u) &
        (dreamcast_vram_size - 1u);
    const auto background_isp =
        vram.read_u32(static_cast<std::uint32_t>(background_address & 0x007FFFFCu));
    const auto read_background_word = [&](const std::uint64_t address) {
        return vram.read_u32(static_cast<std::uint32_t>(address & 0x007FFFFCu));
    };
    const auto background_tsp = read_background_word(background_address + 4u);
    const auto background_tcw = read_background_word(background_address + 8u);
    const auto textured = (background_isp & 0x02000000u) != 0u;
    const auto uv16 = (background_isp & 0x00400000u) != 0u;
    const auto offset_color = (background_isp & 0x01000000u) != 0u;
    const auto required_vertex_payload_words =
        (textured ? (uv16 ? 1u : 2u) : 0u) + 1u + (offset_color ? 1u : 0u);
    if (skip < required_vertex_payload_words)
        throw std::invalid_argument(
            "PVR-Hintergrundvertex ist kleiner als sein ISP/TSP-Format.");
    auto vertex_words = 3u + skip;
    const auto intensity_shadow =
        (registers.read(pvr_register::ShadingScale) & 0x00000100u) != 0u;
    if (shadow && intensity_shadow) vertex_words += skip;
    const auto vertex_stride = static_cast<std::uint64_t>(vertex_words) * 4u;
    const auto first_vertex = background_address + 12u + tag_offset * vertex_stride;
    const auto background_depth =
        std::bit_cast<float>(registers.read(pvr_register::BackgroundPlaneDepth));
    PvrMaterial background_material;
    background_material.gouraud = (background_isp & 0x00800000u) != 0u;
    background_material.textured = textured;
    background_material.offset_color_enabled = offset_color;
    background_material.shadow_enabled = shadow;
    decode_ta_texture_words(background_material, background_tsp, background_tcw);
    if (background_material.texture_x32_stride) {
        background_material.texture_stride_width =
            (registers.read(pvr_register::TextureModulo) & 0x1Fu) * 32u;
        if (background_material.texture_stride_width < background_material.texture_width)
            throw std::runtime_error(
                "PVR-Hintergrundtexturstride ist kleiner als die logische Texturbreite.");
    }
    const auto read_vertex = [&](const std::uint64_t address) {
        PvrVertex vertex;
        vertex.x = std::bit_cast<float>(read_background_word(address));
        vertex.y = std::bit_cast<float>(read_background_word(address + 4u));
        vertex.z = background_depth;
        auto cursor = address + 12u;
        if (textured) {
            if (uv16) {
                const auto packed_uv = read_background_word(cursor);
                vertex.u = decode_uv16_component(packed_uv, true);
                vertex.v = decode_uv16_component(packed_uv, false);
                cursor += 4u;
            } else {
                vertex.u = std::bit_cast<float>(read_background_word(cursor));
                vertex.v = std::bit_cast<float>(read_background_word(cursor + 4u));
                cursor += 8u;
            }
        }
        vertex.argb = read_background_word(cursor);
        if (offset_color) vertex.oargb = read_background_word(cursor + 4u);
        return vertex;
    };
    auto a = read_vertex(first_vertex);
    auto b = read_vertex(first_vertex + vertex_stride);
    auto c = read_vertex(first_vertex + vertex_stride * 2u);
    const auto horizontal_scale = (scaler & 0x00010000u) != 0u ? 2.0f : 1.0f;
    if (!textured) {
        // The untextured background parameter supplies attributes, not raster
        // coverage coordinates. The PVR defines a fixed oversized screen plane.
        a.x = -256.0f * horizontal_scale;
        a.y = 0.0f;
        b.x = 896.0f * horizontal_scale;
        b.y = 0.0f;
        c.x = a.x;
        c.y = 480.0f;
    } else {
        // Textured backgrounds extend beyond both horizontal viewport edges.
        // Extend U by the same affine slope before applying horizontal doubling.
        if (c.x == b.x) {
            c.x = a.x;
            c.u = a.u;
        }
        const auto extension_u = (b.u - a.u) * 0.4f;
        a.x = (a.x - 256.0f) * horizontal_scale;
        a.u -= extension_u;
        b.x = (b.x + 256.0f) * horizontal_scale;
        b.u += extension_u;
        c.x = (c.x - 256.0f) * horizontal_scale;
        c.u -= extension_u;
    }
    auto d = c;
    d.x = b.x;
    if (textured) d.u = b.u;
    const auto first_triangle_area = edge(a, b, c.x, c.y);
    const auto second_triangle_area = edge(b, d, c.x, c.y);
    if (!std::isfinite(first_triangle_area) || !std::isfinite(second_triangle_area) ||
        !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(c.x) ||
        !std::isfinite(c.y) || !std::isfinite(d.x) || !std::isfinite(d.y) ||
        !std::isfinite(a.z) || !std::isfinite(b.z) || !std::isfinite(c.z) ||
        !std::isfinite(d.z) ||
        (textured && (!std::isfinite(a.u) || !std::isfinite(a.v) || !std::isfinite(b.u) ||
                      !std::isfinite(b.v) || !std::isfinite(c.u) || !std::isfinite(c.v) ||
                      !std::isfinite(d.u) || !std::isfinite(d.v))))
        throw std::invalid_argument("PVR-Hintergrundebene besitzt ungueltige Koordinaten.");
    if (textured && (first_triangle_area == 0.0f || second_triangle_area == 0.0f))
        throw std::invalid_argument("Texturierte PVR-Hintergrundebene ist degeneriert.");
    const auto first_triangle_side = edge(b, c, a.x, a.y);
    const auto channel = [](const std::uint32_t argb, const unsigned shift) {
        return static_cast<float>((argb >> shift) & 0xFFu);
    };
    for (std::uint32_t y = minimum_clip_y; y <= maximum_clip_y; ++y) {
        for (std::uint32_t x = minimum_clip_x; x <= maximum_clip_x; ++x) {
            const auto px = static_cast<float>(x) + 0.5f;
            const auto py = static_cast<float>(y) + 0.5f;
            const auto use_first_triangle =
                edge(b, c, px, py) * first_triangle_side >= 0.0f;
            const auto& first = use_first_triangle ? a : b;
            const auto& second = use_first_triangle ? b : d;
            const auto& third = c;
            const auto triangle_area =
                use_first_triangle ? first_triangle_area : second_triangle_area;
            const auto w0 = triangle_area == 0.0f
                                ? 1.0f
                                : edge(second, third, px, py) / triangle_area;
            const auto w1 = triangle_area == 0.0f
                                ? 0.0f
                                : edge(third, first, px, py) / triangle_area;
            const auto w2 = triangle_area == 0.0f
                                ? 0.0f
                                : edge(first, second, px, py) / triangle_area;
            const auto interpolate_channel = [&](const std::uint32_t first,
                                                 const std::uint32_t second,
                                                 const std::uint32_t third,
                                                 const unsigned shift) {
                const auto value = background_material.gouraud
                                       ? w0 * channel(first, shift) +
                                             w1 * channel(second, shift) +
                                             w2 * channel(third, shift)
                                       : channel(first, shift);
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(value, 0.0f, 255.0f)));
            };
            Rgba8 color{interpolate_channel(first.argb, second.argb, third.argb, 16u),
                        interpolate_channel(first.argb, second.argb, third.argb, 8u),
                        interpolate_channel(first.argb, second.argb, third.argb, 0u),
                        background_material.vertex_alpha_enabled
                            ? interpolate_channel(first.argb, second.argb, third.argb, 24u)
                            : std::uint8_t{0xFFu}};
            const Rgba8 interpolated_offset{
                interpolate_channel(first.oargb, second.oargb, third.oargb, 16u),
                interpolate_channel(first.oargb, second.oargb, third.oargb, 8u),
                interpolate_channel(first.oargb, second.oargb, third.oargb, 0u),
                interpolate_channel(first.oargb, second.oargb, third.oargb, 24u)};
            std::uint8_t fog_coefficient = 0u;
            if (background_material.fog_mode == 0u || background_material.fog_mode == 3u)
                fog_coefficient = table_fog_coefficient(registers, background_depth);
            else if (background_material.fog_mode == 1u && offset_color)
                fog_coefficient = interpolated_offset.a;
            if (background_material.fog_mode == 3u) {
                const auto fog =
                    decode_register_color(registers.read(pvr_register::FogTableColor));
                color = {fog.r, fog.g, fog.b, fog_coefficient};
            }
            if (textured) {
                const auto u = w0 * first.u + w1 * second.u + w2 * third.u;
                const auto v = w0 * first.v + w1 * second.v + w2 * third.v;
                const auto texture =
                    sample_texture(vram, registers, background_material, u, v);
                if (background_material.texture_format == 4u) {
                    constexpr auto pi = 3.14159265358979323846f;
                    const auto s = static_cast<float>(texture.r) * (0.5f * pi / 255.0f);
                    const auto r = static_cast<float>(texture.g) * (2.0f * pi / 255.0f);
                    const auto k1 = static_cast<float>(interpolated_offset.a) / 255.0f;
                    const auto k2 = static_cast<float>(interpolated_offset.r) / 255.0f;
                    const auto k3 = static_cast<float>(interpolated_offset.g) / 255.0f;
                    const auto q = static_cast<float>(interpolated_offset.b) / 255.0f;
                    const auto alpha = std::clamp(
                        k1 + k2 * std::sin(s) +
                            k3 * std::cos(s) * std::cos(r - 2.0f * pi * q),
                        0.0f,
                        1.0f);
                    color = {0xFFu,
                             0xFFu,
                             0xFFu,
                             static_cast<std::uint8_t>(std::lround(alpha * 255.0f))};
                } else {
                    color = shade_texture(texture, color, background_material.texture_shading);
                }
            }
            if (offset_color && background_material.texture_format != 4u)
                color = add_offset_color(color, interpolated_offset);
            if (background_material.fog_mode == 0u) {
                color = apply_fog(color,
                                  decode_register_color(
                                      registers.read(pvr_register::FogTableColor)),
                                  fog_coefficient);
            } else if (background_material.fog_mode == 1u && offset_color) {
                color = apply_fog(color,
                                  decode_register_color(
                                      registers.read(pvr_register::FogVertexColor)),
                                  fog_coefficient);
            }
            if (background_material.color_clamp_enabled)
                color = clamp_fragment_color(color, registers);
            const auto offset = static_cast<std::uint32_t>(
                base + static_cast<std::uint64_t>(y) * stride +
                static_cast<std::uint64_t>(x) * pixel_bytes);
            const auto pixel_index = static_cast<std::size_t>(y) * width + x;
            write_pixel(offset, pixel_index, color);
            depth[pixel_index] = w0 * first.z + w1 * second.z + w2 * third.z;
        }
    }
    const auto render_primitive = [&](const PvrPrimitive& primitive) {
        auto texture_material = primitive.material;
        if (texture_material.blend_source_accumulation)
            texture_material.textured = false;
        if (texture_material.texture_x32_stride) {
            texture_material.texture_stride_width =
                (registers.read(pvr_register::TextureModulo) & 0x1Fu) * 32u;
            if (texture_material.texture_stride_width < texture_material.texture_width)
                throw std::runtime_error(
                    "PVR-Texturstride ist kleiner als die logische Texturbreite.");
        }
        for (std::size_t index = 2u; index < primitive.vertices.size(); ++index) {
            auto* a = &primitive.vertices[index - 2u];
            auto* b = &primitive.vertices[index - 1u];
            const auto* c = &primitive.vertices[index];
            if ((index & 1u) != 0u) std::swap(a, b);
            const auto area = edge(*a, *b, c->x, c->y);
            if (area == 0.0f) continue;
            if ((primitive.material.culling == 1u && std::fabs(area) < 1.0f) ||
                (primitive.material.culling == 2u && area > 0.0f) ||
                (primitive.material.culling == 3u && area < 0.0f))
                continue;
            const auto minimum_x = std::clamp(
                static_cast<int>(std::floor(std::min({a->x, b->x, c->x}))),
                static_cast<int>(minimum_clip_x),
                static_cast<int>(maximum_clip_x + 1u));
            const auto maximum_x = std::clamp(
                static_cast<int>(std::ceil(std::max({a->x, b->x, c->x}))),
                static_cast<int>(minimum_clip_x),
                static_cast<int>(maximum_clip_x + 1u));
            const auto minimum_y = std::clamp(static_cast<int>(std::floor(std::min({a->y, b->y, c->y}))),
                                              static_cast<int>(minimum_clip_y),
                                              static_cast<int>(maximum_clip_y + 1u));
            const auto maximum_y = std::clamp(static_cast<int>(std::ceil(std::max({a->y, b->y, c->y}))),
                                              static_cast<int>(minimum_clip_y),
                                              static_cast<int>(maximum_clip_y + 1u));
            const auto channel = [](const std::uint32_t argb, const unsigned shift) {
                return static_cast<float>((argb >> shift) & 0xFFu);
            };
            for (auto y = minimum_y; y < maximum_y; ++y) {
                for (auto x = minimum_x; x < maximum_x; ++x) {
                    if (primitive.material.user_clip_mode != 0u) {
                        if (primitive.material.user_clip_mode == 1u)
                            throw std::runtime_error("TA-Userclip-Modus 1 ist reserviert.");
                        const auto tile_x = static_cast<std::uint32_t>(x) / 32u;
                        const auto tile_y = static_cast<std::uint32_t>(y) / 32u;
                        const bool inside =
                            tile_x >= primitive.material.user_clip_start_x &&
                            tile_x <= primitive.material.user_clip_end_x &&
                            tile_y >= primitive.material.user_clip_start_y &&
                            tile_y <= primitive.material.user_clip_end_y;
                        if ((primitive.material.user_clip_mode == 2u && !inside) ||
                            (primitive.material.user_clip_mode == 3u && inside))
                            continue;
                    }
                    const auto px = static_cast<float>(x) + 0.5f;
                    const auto py = static_cast<float>(y) + 0.5f;
                    const auto w0 = edge(*b, *c, px, py);
                    const auto w1 = edge(*c, *a, px, py);
                    const auto w2 = edge(*a, *b, px, py);
                    if ((area > 0.0f && (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)) ||
                        (area < 0.0f && (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)))
                        continue;
                    const auto interpolate_channel = [&](const unsigned shift) {
                        const auto value = primitive.material.gouraud
                                               ? (w0 * channel(a->argb, shift) +
                                                  w1 * channel(b->argb, shift) +
                                                  w2 * channel(c->argb, shift)) /
                                                     area
                                               : channel(a->argb, shift);
                        return static_cast<std::uint8_t>(
                            std::lround(std::clamp(value, 0.0f, 255.0f)));
                    };
                    const auto interpolate_float = [&](const float av,
                                                       const float bv,
                                                       const float cv) {
                        return (w0 * av + w1 * bv + w2 * cv) / area;
                    };
                    const auto pixel_index = static_cast<std::size_t>(y) * width +
                                             static_cast<std::size_t>(x);
                    if (volume_selection_mask && (*volume_selection_mask)[pixel_index] == 0u)
                        continue;
                    const auto fragment_depth = interpolate_float(a->z, b->z, c->z);
                    if (!depth_passes(primitive.material.depth_compare,
                                      fragment_depth,
                                      depth[pixel_index]))
                        continue;
                    Rgba8 source{interpolate_channel(16u),
                                 interpolate_channel(8u),
                                 interpolate_channel(0u),
                                 primitive.material.vertex_alpha_enabled
                                     ? interpolate_channel(24u)
                                     : std::uint8_t{0xFFu}};
                    const Rgba8 offset_color{
                        static_cast<std::uint8_t>(primitive.material.gouraud
                                                      ? std::lround(std::clamp(
                                                            (w0 * channel(a->oargb, 16u) +
                                                             w1 * channel(b->oargb, 16u) +
                                                             w2 * channel(c->oargb, 16u)) /
                                                                area,
                                                            0.0f,
                                                            255.0f))
                                                      : channel(a->oargb, 16u)),
                        static_cast<std::uint8_t>(primitive.material.gouraud
                                                      ? std::lround(std::clamp(
                                                            (w0 * channel(a->oargb, 8u) +
                                                             w1 * channel(b->oargb, 8u) +
                                                             w2 * channel(c->oargb, 8u)) /
                                                                area,
                                                            0.0f,
                                                            255.0f))
                                                      : channel(a->oargb, 8u)),
                        static_cast<std::uint8_t>(primitive.material.gouraud
                                                      ? std::lround(std::clamp(
                                                            (w0 * channel(a->oargb, 0u) +
                                                             w1 * channel(b->oargb, 0u) +
                                                             w2 * channel(c->oargb, 0u)) /
                                                                area,
                                                            0.0f,
                                                            255.0f))
                                                      : channel(a->oargb, 0u)),
                        static_cast<std::uint8_t>(primitive.material.gouraud
                                                      ? std::lround(std::clamp(
                                                            (w0 * channel(a->oargb, 24u) +
                                                             w1 * channel(b->oargb, 24u) +
                                                             w2 * channel(c->oargb, 24u)) /
                                                                area,
                                                            0.0f,
                                                            255.0f))
                                                      : channel(a->oargb, 24u))};
                    std::uint8_t fog_coefficient = 0u;
                    if (primitive.material.fog_mode == 0u ||
                        primitive.material.fog_mode == 3u) {
                        fog_coefficient = table_fog_coefficient(registers, fragment_depth);
                    } else if (primitive.material.fog_mode == 1u &&
                               primitive.material.offset_color_enabled) {
                        fog_coefficient = offset_color.a;
                    }
                    if (primitive.material.fog_mode == 3u) {
                        const auto fog =
                            decode_register_color(registers.read(pvr_register::FogTableColor));
                        source = {fog.r, fog.g, fog.b, fog_coefficient};
                    }
                    if (texture_material.textured) {
                        if (std::fabs(fragment_depth) <= std::numeric_limits<float>::epsilon())
                            throw std::runtime_error(
                                "PVR-Perspektivinterpolation besitzt eine Nulltiefe.");
                        const auto u = interpolate_float(a->u * a->z, b->u * b->z,
                                                         c->u * c->z) /
                                       fragment_depth;
                        const auto v = interpolate_float(a->v * a->z, b->v * b->z,
                                                         c->v * c->z) /
                                       fragment_depth;
                        const auto texture = sample_texture(vram, registers, texture_material, u, v);
                        if (texture_material.texture_format == 4u) {
                            constexpr auto pi = 3.14159265358979323846f;
                            const auto s = static_cast<float>(texture.r) * (0.5f * pi / 255.0f);
                            const auto r = static_cast<float>(texture.g) * (2.0f * pi / 255.0f);
                            const auto k1 = static_cast<float>(offset_color.a) / 255.0f;
                            const auto k2 = static_cast<float>(offset_color.r) / 255.0f;
                            const auto k3 = static_cast<float>(offset_color.g) / 255.0f;
                            const auto q = static_cast<float>(offset_color.b) / 255.0f;
                            const auto alpha = std::clamp(
                                k1 + k2 * std::sin(s) +
                                    k3 * std::cos(s) * std::cos(r - 2.0f * pi * q),
                                0.0f,
                                1.0f);
                            source = {0xFFu,
                                      0xFFu,
                                      0xFFu,
                                      static_cast<std::uint8_t>(std::lround(alpha * 255.0f))};
                        } else {
                            source = shade_texture(texture,
                                                   source,
                                                   primitive.material.texture_shading);
                        }
                    }
                    if (primitive.material.offset_color_enabled &&
                        texture_material.texture_format != 4u)
                        source = add_offset_color(source, offset_color);
                    if (primitive.material.fog_mode == 0u) {
                        source = apply_fog(
                            source,
                            decode_register_color(registers.read(pvr_register::FogTableColor)),
                            fog_coefficient);
                    } else if (primitive.material.fog_mode == 1u &&
                               primitive.material.offset_color_enabled) {
                        source = apply_fog(
                            source,
                            decode_register_color(registers.read(pvr_register::FogVertexColor)),
                            fog_coefficient);
                    }
                    if (primitive.material.color_clamp_enabled)
                        source = clamp_fragment_color(source, registers);
                    if (primitive.material.blend_source_accumulation) {
                        source = secondary_accumulation[pixel_index];
                    }
                    if (primitive.list == PvrListType::PunchThrough &&
                        source.a <
                            (registers.read(pvr_register::PunchThroughAlphaReference) & 0xFFu))
                        continue;
                    const auto offset = static_cast<std::uint32_t>(
                        base + static_cast<std::uint64_t>(y) * stride +
                        static_cast<std::uint64_t>(x) * pixel_bytes);
                    const auto destination =
                        primitive.material.blend_destination_accumulation
                            ? secondary_accumulation[pixel_index]
                            : read_render_pixel(vram, offset, pack_mode);
                    if (primitive.list == PvrListType::Translucent)
                        source = blend_color(source, destination, primitive.material);
                    if (primitive.material.blend_destination_accumulation) {
                        secondary_accumulation[pixel_index] = source;
                    } else {
                        write_pixel(offset, pixel_index, source);
                    }
                    shadow_eligible[pixel_index] = primitive.material.shadow_enabled ? 1u : 0u;
                    volume_material_eligible[pixel_index] =
                        primitive.material.shadow_enabled && primitive.material.volume_material
                            ? 1u
                            : 0u;
                    if (primitive.material.depth_write) depth[pixel_index] = fragment_depth;
                    ++metrics_.pixels;
                }
            }
            ++metrics_.triangles;
        }
    };

    const auto apply_modifier_volumes =
        [&](const PvrListType list) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> volume_result(shadow_eligible.size(), 0u);
        std::vector<std::uint8_t> area_one(shadow_eligible.size(), 0u);
        bool volume_open = false;
        for (const auto& volume : frame.modifier_volumes) {
            if (volume.list != list) continue;
            const auto use_union = !volume.volume_last && volume.depth_mode > 0u;
            for (const auto& triangle : volume.triangles) {
                const auto& a = triangle[0];
                const auto& b = triangle[1];
                const auto& c = triangle[2];
                const auto triangle_area = edge(a, b, c.x, c.y);
                if (triangle_area == 0.0f) continue;
                if ((volume.culling == 1u && std::fabs(triangle_area) < 1.0f) ||
                    (volume.culling == 2u && triangle_area > 0.0f) ||
                    (volume.culling == 3u && triangle_area < 0.0f))
                    continue;
                const auto triangle_minimum_x = std::clamp(
                    static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))),
                    static_cast<int>(minimum_clip_x),
                    static_cast<int>(maximum_clip_x + 1u));
                const auto triangle_maximum_x = std::clamp(
                    static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))),
                    static_cast<int>(minimum_clip_x),
                    static_cast<int>(maximum_clip_x + 1u));
                const auto triangle_minimum_y = std::clamp(
                    static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))),
                    static_cast<int>(minimum_clip_y),
                    static_cast<int>(maximum_clip_y + 1u));
                const auto triangle_maximum_y = std::clamp(
                    static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))),
                    static_cast<int>(minimum_clip_y),
                    static_cast<int>(maximum_clip_y + 1u));
                for (auto y = triangle_minimum_y; y < triangle_maximum_y; ++y) {
                    for (auto x = triangle_minimum_x; x < triangle_maximum_x; ++x) {
                        if (volume.user_clip_mode != 0u) {
                            if (volume.user_clip_mode == 1u)
                                throw std::runtime_error(
                                    "TA-Modifier-Userclip-Modus 1 ist reserviert.");
                            const auto tile_x = static_cast<std::uint32_t>(x) / 32u;
                            const auto tile_y = static_cast<std::uint32_t>(y) / 32u;
                            const bool inside_clip =
                                tile_x >= volume.user_clip_start_x &&
                                tile_x <= volume.user_clip_end_x &&
                                tile_y >= volume.user_clip_start_y &&
                                tile_y <= volume.user_clip_end_y;
                            if ((volume.user_clip_mode == 2u && !inside_clip) ||
                                (volume.user_clip_mode == 3u && inside_clip))
                                continue;
                        }
                        const auto px = static_cast<float>(x) + 0.5f;
                        const auto py = static_cast<float>(y) + 0.5f;
                        const auto w0 = edge(b, c, px, py);
                        const auto w1 = edge(c, a, px, py);
                        const auto w2 = edge(a, b, px, py);
                        if ((triangle_area > 0.0f &&
                             (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)) ||
                            (triangle_area < 0.0f &&
                             (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)))
                            continue;
                        const auto pixel_index = static_cast<std::size_t>(y) * width +
                                                 static_cast<std::size_t>(x);
                        const auto modifier_depth =
                            (w0 * a.z + w1 * b.z + w2 * c.z) / triangle_area;
                        if (modifier_depth <= depth[pixel_index]) continue;
                        if (use_union)
                            volume_result[pixel_index] = 1u;
                        else
                            volume_result[pixel_index] ^= 1u;
                    }
                }
            }
            if (volume.depth_mode == 0u) {
                volume_open = true;
                continue;
            }
            for (std::size_t pixel = 0u; pixel < area_one.size(); ++pixel) {
                if (volume.depth_mode == 1u)
                    area_one[pixel] |= volume_result[pixel];
                else
                    area_one[pixel] &= static_cast<std::uint8_t>(!volume_result[pixel]);
                volume_result[pixel] = 0u;
            }
            volume_open = false;
        }
        if (volume_open)
            throw std::logic_error(
                "TA-Modifier-Volume endet ohne Inclusion-/Exclusion-Abschluss.");

        bool affected = false;
        for (std::size_t pixel = 0u; pixel < area_one.size(); ++pixel) {
            if (area_one[pixel] != 0u && shadow_eligible[pixel] != 0u) {
                affected = true;
                break;
            }
        }
        if (!affected) return area_one;
        const auto shading = registers.read(pvr_register::ShadingScale);
        const auto scale = static_cast<std::uint32_t>(shading & 0xFFu);
        for (std::uint32_t y = minimum_clip_y; y <= maximum_clip_y; ++y) {
            for (std::uint32_t x = minimum_clip_x; x <= maximum_clip_x; ++x) {
                const auto pixel_index = static_cast<std::size_t>(y) * width + x;
                if (area_one[pixel_index] == 0u || shadow_eligible[pixel_index] == 0u ||
                    volume_material_eligible[pixel_index] != 0u)
                    continue;
                const auto offset = static_cast<std::uint32_t>(
                    base + static_cast<std::uint64_t>(y) * stride +
                    static_cast<std::uint64_t>(x) * pixel_bytes);
                auto color = read_render_pixel(vram, offset, pack_mode);
                color.r = static_cast<std::uint8_t>((color.r * scale + 127u) / 256u);
                color.g = static_cast<std::uint8_t>((color.g * scale + 127u) / 256u);
                color.b = static_cast<std::uint8_t>((color.b * scale + 127u) / 256u);
                write_pixel(offset, pixel_index, color);
                ++metrics_.pixels;
            }
        }
        return area_one;
    };

    const auto render_volume_materials = [&](const PvrListType list,
                                              const std::vector<std::uint8_t>& selection) {
        if (std::none_of(selection.begin(), selection.end(), [](const auto value) {
                return value != 0u;
            }))
            return;
        volume_selection_mask = &selection;
        for (const auto& primitive : frame.primitives) {
            const auto applies =
                (list == PvrListType::OpaqueModifier &&
                 (primitive.list == PvrListType::Opaque ||
                  primitive.list == PvrListType::PunchThrough)) ||
                (list == PvrListType::TranslucentModifier &&
                 primitive.list == PvrListType::Translucent);
            if (!applies || !primitive.material.shadow_enabled ||
                !primitive.material.volume_material)
                continue;
            auto selected = primitive;
            selected.material = *primitive.material.volume_material;
            selected.material.volume_material.reset();
            selected.material.depth_compare = 2u;
            selected.material.depth_write = false;
            for (auto& vertex : selected.vertices) {
                vertex.u = vertex.volume_u;
                vertex.v = vertex.volume_v;
                vertex.argb = vertex.volume_argb;
                vertex.oargb = vertex.volume_oargb;
            }
            render_primitive(selected);
        }
        volume_selection_mask = nullptr;
    };

    for (const auto& primitive : frame.primitives) {
        if (primitive.list == PvrListType::Opaque ||
            primitive.list == PvrListType::PunchThrough)
            render_primitive(primitive);
    }
    const auto opaque_volume_area = apply_modifier_volumes(PvrListType::OpaqueModifier);
    render_volume_materials(PvrListType::OpaqueModifier, opaque_volume_area);
    std::fill(shadow_eligible.begin(), shadow_eligible.end(), std::uint8_t{0u});
    std::fill(volume_material_eligible.begin(),
              volume_material_eligible.end(),
              std::uint8_t{0u});
    for (const auto& primitive : frame.primitives) {
        if (primitive.list == PvrListType::Translucent) render_primitive(primitive);
    }
    const auto translucent_volume_area =
        apply_modifier_volumes(PvrListType::TranslucentModifier);
    render_volume_materials(PvrListType::TranslucentModifier, translucent_volume_area);
    PvrRenderGenerationEvidence evidence;
    evidence.generation = render_generation;
    evidence.write_base = base;
    evidence.stride_bytes = static_cast<std::uint32_t>(stride);
    evidence.width = width;
    evidence.height = height;
    evidence.pixel_bytes = static_cast<std::uint8_t>(pixel_bytes);
    evidence.render_to_texture = (write_start & 0x01000000u) != 0u;
    evidence.pixel_writes = frame_pixel_writes;
    for (std::size_t pixel_index = 0u; pixel_index < render_pixel_count; ++pixel_index) {
        if (touched_pixels[pixel_index] == 0u) continue;
        const auto y = pixel_index / width;
        const auto x = pixel_index % width;
        const auto offset = static_cast<std::uint32_t>(
            base + static_cast<std::uint64_t>(y) * stride +
            static_cast<std::uint64_t>(x) * pixel_bytes);
        const auto packed_value = read_packed_pixel(offset);
        if (packed_value == original_pixels[pixel_index]) continue;
        ++evidence.changed_pixels;
        std::uint32_t changed_byte_mask = 0u;
        for (std::size_t byte = 0u; byte < pixel_bytes; ++byte) {
            const auto shift = static_cast<unsigned>(byte * 8u);
            if (((packed_value >> shift) & 0xFFu) !=
                ((original_pixels[pixel_index] >> shift) & 0xFFu))
                changed_byte_mask |= 1u << byte;
        }
        evidence.changed_pixel_values.push_back(
            PvrChangedPixelEvidence{offset, packed_value, changed_byte_mask});
    }
    const auto frame_changed_pixels = evidence.changed_pixels;
    last_render_generation_ = evidence.generation;
    next_render_generation_ =
        render_generation == std::numeric_limits<std::uint64_t>::max()
            ? 0u
            : render_generation + 1u;
    if (!evidence.changed_pixel_values.empty()) {
        const auto evidence_bytes =
            evidence.changed_pixel_values.size() * sizeof(PvrChangedPixelEvidence);
        while (!pending_render_evidence_.empty() &&
               (pending_render_evidence_.size() >= render_evidence_capacity ||
                pending_render_evidence_bytes_ + evidence_bytes >
                    render_evidence_byte_capacity)) {
            pending_render_evidence_bytes_ -=
                pending_render_evidence_.front().changed_pixel_values.size() *
                sizeof(PvrChangedPixelEvidence);
            pending_render_evidence_.pop_front();
            ++metrics_.dropped_render_evidence_generations;
        }
        if (evidence_bytes <= render_evidence_byte_capacity) {
            next_evidence_scan_generation_ = evidence.generation;
            pending_render_evidence_bytes_ += evidence_bytes;
            pending_render_evidence_.push_back(std::move(evidence));
        } else {
            ++metrics_.dropped_render_evidence_generations;
        }
    }
    metrics_.pixel_writes += frame_pixel_writes;
    metrics_.changed_pixels += frame_changed_pixels;
    metrics_.last_frame_pixel_writes = frame_pixel_writes;
    metrics_.last_frame_changed_pixels = frame_changed_pixels;
    ++metrics_.frames;
}

void PvrSoftwareRenderer::set_guest_memory_access_memory(Memory* const memory) noexcept {
    guest_memory_access_memory_ = memory;
}

void PvrSoftwareRenderer::observe_vram_write(const std::uint32_t address,
                                             const std::size_t size,
                                             const bool bytes_changed) {
    if (!bytes_changed || size == 0u) return;
    const auto physical = address < 0xE0000000u ? address & 0x1FFFFFFFu : address;
    const auto write_begin = static_cast<std::uint64_t>(physical);
    const auto write_size = static_cast<std::uint64_t>(size);
    const auto write_end =
        write_size > std::numeric_limits<std::uint64_t>::max() - write_begin
            ? std::numeric_limits<std::uint64_t>::max()
            : write_begin + write_size;
    bool direct_vram = false;
    const auto mark_backing_range = [&](const std::uint32_t begin,
                                        const std::uint32_t end) {
        if (begin >= end) return;
        const auto first_word = static_cast<std::size_t>(begin / 64u);
        const auto last_word = static_cast<std::size_t>((end - 1u) / 64u);
        const auto add_mask = [&](const std::size_t word_index,
                                  const std::uint64_t mask) {
            auto& word = direct_dirty_words_[word_index];
            direct_dirty_byte_count_ += std::popcount(mask & ~word);
            word |= mask;
        };
        const auto first_bit = begin & 63u;
        const auto end_bit = end & 63u;
        const auto first_mask = ~std::uint64_t{0u} << first_bit;
        const auto last_mask =
            end_bit == 0u ? ~std::uint64_t{0u}
                          : (std::uint64_t{1u} << end_bit) - 1u;
        if (first_word == last_word) {
            add_mask(first_word, first_mask & last_mask);
            return;
        }
        add_mask(first_word, first_mask);
        for (auto word = first_word + 1u; word < last_word; ++word)
            add_mask(word, ~std::uint64_t{0u});
        add_mask(last_word, last_mask);
    };
    const auto mark_mapping = [&](const std::uint32_t base, const bool logical_32bit) {
        const auto range_begin = static_cast<std::uint64_t>(base);
        const auto range_end = range_begin + dreamcast_vram_size;
        const auto overlap_begin = std::max(write_begin, range_begin);
        const auto overlap_end = std::min(write_end, range_end);
        if (overlap_begin >= overlap_end) return;
        direct_vram = true;
        if (direct_dirty_words_.empty())
            direct_dirty_words_.resize((dreamcast_vram_size + 63u) / 64u, 0u);
        const auto offset_begin =
            static_cast<std::uint32_t>(overlap_begin - range_begin);
        const auto offset_end =
            static_cast<std::uint32_t>(overlap_end - range_begin);
        if (!logical_32bit) {
            mark_backing_range(offset_begin, offset_end);
            return;
        }
        // The 32-bit aperture interleaves four-byte guest words across the two
        // VRAM banks. Mark each mapped word as one bitmap range instead of
        // walking and setting every byte individually.
        auto current = offset_begin;
        while (current < offset_end) {
            const auto next_word =
                std::min<std::uint32_t>(offset_end, (current & ~3u) + 4u);
            const auto backing = dreamcast_vram_32bit_to_linear_offset(current);
            mark_backing_range(backing, backing + (next_word - current));
            current = next_word;
        }
    };
    for (const auto base : dreamcast_vram_64bit_physical_bases) mark_mapping(base, false);
    for (const auto base : dreamcast_vram_32bit_physical_bases) mark_mapping(base, true);
    mark_mapping(0x11000000u, false);
    mark_mapping(0x11800000u, false);
    mark_mapping(0x13000000u, true);
    mark_mapping(0x13800000u, true);
    if (!direct_vram) return;
    const auto generation = next_direct_write_generation_;
    if (next_direct_write_generation_ != std::numeric_limits<std::uint64_t>::max())
        ++next_direct_write_generation_;
    if (pending_direct_first_write_generation_ == 0u)
        pending_direct_first_write_generation_ = generation;
    pending_direct_last_write_generation_ = generation;
}

void PvrSoftwareRenderer::reset_guest_frame_evidence(
    const std::span<const std::uint8_t> vram) {
    if (!vram.empty() && vram.size() != dreamcast_vram_size)
        throw std::invalid_argument("PVR-Evidenzreset braucht ein vollstaendiges 8-MiB-VRAM-Abbild.");
    pending_render_evidence_.clear();
    pending_render_evidence_bytes_ = 0u;
    next_evidence_scan_generation_ = 0u;
    pending_direct_first_write_generation_ = 0u;
    pending_direct_last_write_generation_ = 0u;
    std::fill(direct_dirty_words_.begin(), direct_dirty_words_.end(), std::uint64_t{0u});
    direct_dirty_byte_count_ = 0u;
    if (vram.empty()) {
        direct_vram_shadow_.clear();
        direct_vram_shadow_valid_ = false;
    } else {
        direct_vram_shadow_.assign(vram.begin(), vram.end());
        direct_vram_shadow_valid_ = true;
    }
    queued_guest_frame_proof_.reset();
    queued_scanout_frame_.reset();
}

void PvrSoftwareRenderer::observe_vblank_scanout(const PvrRegisterFile& registers,
                                                 const std::span<const std::uint8_t> vram) {
    const auto scanout = decode_pvr_scanout(registers, vram.size());
    if (!scanout) return;

    const auto scanout_field = scanout->interlaced ? (registers.field() & 1u) : 0u;
    const auto active_base = scanout_field == 0u ? scanout->base_offset
                                                 : scanout->second_base_offset;
    const auto capture_scanout =
        [&](const std::span<const std::uint8_t> source_vram,
            const std::optional<std::array<std::uint8_t, 4u>> solid_color = std::nullopt) {
        PvrFramebuffer framebuffer;
        const auto weave_fields = scanout->interlaced && scanout->weave_fields;
        framebuffer.configure(scanout->width,
                              scanout->height,
                              scanout->stride_bytes,
                              scanout->format,
                              scanout->line_double,
                              weave_fields,
                              scanout->source_width,
                              scanout->source_height,
                              scanout->concat,
                              true);
        return framebuffer.capture(
            source_vram,
            weave_fields ? scanout->base_offset : active_base,
            weave_fields ? std::optional<std::size_t>{scanout->second_base_offset}
                         : std::nullopt,
            solid_color);
    };
    if (scanout->video_blank) {
        queued_scanout_frame_ = capture_scanout({}, scanout->border_rgba);
        return;
    }
    if (direct_vram_shadow_.empty()) direct_vram_shadow_.resize(dreamcast_vram_size, 0u);
    auto frame = capture_scanout(vram);
    // Keep the first unconsumed proof intact, but never let diagnostic evidence
    // suppress a newer real scanout. The host queue is latest-wins and bounded to
    // one frame.
    if (queued_guest_frame_proof_) {
        queued_scanout_frame_ = std::move(frame);
        return;
    }
    if (!direct_vram_shadow_valid_) {
        std::copy(vram.begin(), vram.end(), direct_vram_shadow_.begin());
        std::fill(direct_dirty_words_.begin(), direct_dirty_words_.end(), std::uint64_t{0u});
        direct_dirty_byte_count_ = 0u;
        pending_direct_first_write_generation_ = 0u;
        pending_direct_last_write_generation_ = 0u;
        direct_vram_shadow_valid_ = true;
    }
    const auto needs_previous_frame =
        pending_direct_first_write_generation_ != 0u && direct_dirty_byte_count_ != 0u;
    const auto previous_frame = needs_previous_frame
                                    ? std::optional<PvrFrame>{capture_scanout(
                                          std::span<const std::uint8_t>(direct_vram_shadow_))}
                                    : std::nullopt;
    struct DirectScanoutResult {
        std::uint64_t first_generation = 0u;
        std::uint64_t last_generation = 0u;
        std::uint64_t changed_pixels = 0u;
    };
    const auto observe_direct_scanout = [&]() {
        DirectScanoutResult result{pending_direct_first_write_generation_,
                                   pending_direct_last_write_generation_,
                                   0u};
        const auto dirty = [&](const std::uint32_t backing) {
            if (direct_dirty_words_.empty()) return false;
            return (direct_dirty_words_[backing / 64u] &
                    (std::uint64_t{1u} << (backing & 63u))) != 0u;
        };
        const auto consume_dirty = [&](const std::uint32_t backing) {
            if (direct_dirty_words_.empty()) return;
            auto& word = direct_dirty_words_[backing / 64u];
            const auto mask = std::uint64_t{1u} << (backing & 63u);
            if ((word & mask) == 0u) return;
            word &= ~mask;
            --direct_dirty_byte_count_;
        };
        const auto pixel_bytes = bytes_per_pixel(scanout->format);
        for (std::uint32_t y = 0u; y < frame.height; ++y) {
            const auto source_line = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y) * scanout->source_height / frame.height);
            if (scanout->weave_fields && (source_line & 1u) != scanout_field) continue;
            const auto source_row = scanout->weave_fields ? source_line / 2u : source_line;
            const auto row_begin = static_cast<std::size_t>(y) * frame.width * 4u;
            for (std::uint32_t x = 0u; x < frame.width; ++x) {
                const auto source_x = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * scanout->source_width / frame.width);
                const auto logical = static_cast<std::uint64_t>(active_base) +
                                     static_cast<std::uint64_t>(source_row) *
                                         scanout->stride_bytes +
                                     static_cast<std::uint64_t>(source_x) * pixel_bytes;
                bool pixel_is_dirty = false;
                std::array<std::uint32_t, 4u> backing_offsets{};
                for (std::size_t byte = 0u; byte < pixel_bytes; ++byte) {
                    const auto logical_byte = static_cast<std::uint32_t>(
                        (logical + byte) % dreamcast_vram_size);
                    backing_offsets[byte] =
                        dreamcast_vram_32bit_to_linear_offset(logical_byte);
                    pixel_is_dirty = pixel_is_dirty || dirty(backing_offsets[byte]);
                }
                const auto pixel = row_begin + static_cast<std::size_t>(x) * 4u;
                if (pixel_is_dirty && previous_frame &&
                    !std::equal(frame.rgba.begin() + pixel,
                                frame.rgba.begin() + pixel + 4u,
                                previous_frame->rgba.begin() + pixel))
                    ++result.changed_pixels;
                for (std::size_t byte = 0u; byte < pixel_bytes; ++byte) {
                    consume_dirty(backing_offsets[byte]);
                    direct_vram_shadow_[backing_offsets[byte]] = vram[backing_offsets[byte]];
                }
            }
        }
        if (direct_dirty_byte_count_ == 0u) {
            pending_direct_first_write_generation_ = 0u;
            pending_direct_last_write_generation_ = 0u;
        }
        return result;
    };
    const auto direct_scanout = observe_direct_scanout();
    const auto queue_direct_scanout = [&] {
        if (direct_scanout.changed_pixels == 0u) return false;
        auto proof = PvrGuestFrameProof{direct_scanout.last_generation,
                                        direct_scanout.changed_pixels,
                                        scanout_field,
                                        *scanout,
                                        std::move(frame),
                                        PvrGuestFrameProofSource::DirectFramebuffer};
        proof.write_generation_first = direct_scanout.first_generation;
        proof.write_generation_last = direct_scanout.last_generation;
        queued_guest_frame_proof_ = std::move(proof);
        ++metrics_.proven_guest_frames;
        ++metrics_.direct_scanout_frames;
        metrics_.direct_scanout_changed_pixels += direct_scanout.changed_pixels;
        queued_scanout_frame_.reset();
        return true;
    };
    if (pending_render_evidence_.empty()) {
        if (!queue_direct_scanout()) queued_scanout_frame_ = std::move(frame);
        return;
    }

    const auto scanout_pixel_bytes = bytes_per_pixel(scanout->format);
    const auto ceil_divide = [](const std::uint64_t numerator,
                                const std::uint64_t denominator) {
        return numerator / denominator + (numerator % denominator != 0u ? 1u : 0u);
    };
    const auto coordinate_is_sampled = [&](const std::uint32_t coordinate,
                                           const std::uint32_t source_extent,
                                           const std::uint32_t output_extent) {
        const auto first_output = ceil_divide(
            static_cast<std::uint64_t>(coordinate) * output_extent, source_extent);
        const auto after_output = ceil_divide(
            static_cast<std::uint64_t>(coordinate + 1u) * output_extent, source_extent);
        return first_output != after_output;
    };
    const auto active_field_rows = scanout->weave_fields ? scanout->source_height / 2u
                                                         : scanout->source_height;
    const auto active_line_bytes =
        static_cast<std::size_t>(scanout->source_width) * scanout_pixel_bytes;
    const auto pixel_is_visible = [&](const PvrRenderGenerationEvidence& evidence,
                                      const PvrChangedPixelEvidence& pixel) {
        const auto pixel_end = static_cast<std::size_t>(pixel.offset) + evidence.pixel_bytes;
        if (pixel_end > vram.size()) return false;
        std::uint32_t current_value = 0u;
        for (std::size_t byte = 0u; byte < evidence.pixel_bytes; ++byte) {
            const auto logical = static_cast<std::uint32_t>(pixel.offset + byte) &
                                 static_cast<std::uint32_t>(dreamcast_vram_size - 1u);
            current_value |=
                static_cast<std::uint32_t>(
                    vram[dreamcast_vram_32bit_to_linear_offset(logical)])
                << static_cast<unsigned>(byte * 8u);
        }
        if (current_value != pixel.packed_value) return false;
        for (std::size_t byte = 0u; byte < evidence.pixel_bytes; ++byte) {
            if ((pixel.changed_byte_mask & (1u << byte)) == 0u) continue;
            const auto address = static_cast<std::size_t>(pixel.offset) + byte;
            if (address < active_base) continue;
            const auto relative = address - active_base;
            const auto source_row = scanout->stride_bytes == 0u
                                        ? std::size_t{0u}
                                        : relative / scanout->stride_bytes;
            const auto source_column = scanout->stride_bytes == 0u
                                           ? relative
                                           : relative % scanout->stride_bytes;
            if (source_row >= active_field_rows || source_column >= active_line_bytes)
                continue;
            const auto source_x =
                static_cast<std::uint32_t>(source_column / scanout_pixel_bytes);
            const auto source_line = static_cast<std::uint32_t>(
                scanout->weave_fields ? source_row * 2u + scanout_field : source_row);
            if (coordinate_is_sampled(source_x, scanout->source_width, scanout->width) &&
                coordinate_is_sampled(source_line, scanout->source_height, scanout->height))
                return true;
        }
        return false;
    };
    const auto active_end = active_base +
                            static_cast<std::size_t>(active_field_rows - 1u) *
                                scanout->stride_bytes +
                            active_line_bytes;
    const auto generation_overlaps_active_field = [&](const PvrRenderGenerationEvidence& value) {
        const auto generation_end = static_cast<std::uint64_t>(value.write_base) +
                                    static_cast<std::uint64_t>(value.height - 1u) *
                                        value.stride_bytes +
                                    static_cast<std::uint64_t>(value.width) *
                                        value.pixel_bytes;
        return static_cast<std::uint64_t>(value.write_base) < active_end &&
               active_base < generation_end;
    };
    const auto evidence_count = pending_render_evidence_.size();
    auto start_index = evidence_count - 1u;
    if (next_evidence_scan_generation_ != 0u) {
        for (std::size_t index = evidence_count; index != 0u; --index) {
            if (pending_render_evidence_[index - 1u].generation <=
                next_evidence_scan_generation_) {
                start_index = index - 1u;
                break;
            }
        }
    }
    std::optional<std::size_t> selected_index;
    std::size_t examined_pixels = 0u;
    for (std::size_t step = 0u; step < evidence_count; ++step) {
        if (examined_pixels == render_evidence_scan_pixel_budget) {
            ++metrics_.render_evidence_scan_budget_exhaustions;
            break;
        }
        const auto index = (start_index + evidence_count - step) % evidence_count;
        auto& evidence = pending_render_evidence_[index];
        const auto previous_index = index == 0u ? evidence_count - 1u : index - 1u;
        if (!generation_overlaps_active_field(evidence)) {
            ++metrics_.render_evidence_range_rejections;
            evidence.validation_cursor = 0u;
            next_evidence_scan_generation_ =
                pending_render_evidence_[previous_index].generation;
            continue;
        }
        auto cursor = std::min(evidence.validation_cursor,
                               evidence.changed_pixel_values.size());
        for (; cursor < evidence.changed_pixel_values.size() &&
               examined_pixels < render_evidence_scan_pixel_budget;
             ++cursor) {
            ++examined_pixels;
            ++metrics_.render_evidence_pixels_examined;
            if (pixel_is_visible(evidence, evidence.changed_pixel_values[cursor])) {
                selected_index = index;
                break;
            }
        }
        if (selected_index) break;
        if (cursor != evidence.changed_pixel_values.size()) {
            evidence.validation_cursor = cursor;
            next_evidence_scan_generation_ = evidence.generation;
            ++metrics_.render_evidence_scan_budget_exhaustions;
            break;
        }
        evidence.validation_cursor = 0u;
        next_evidence_scan_generation_ = pending_render_evidence_[previous_index].generation;
    }
    if (!selected_index) {
        if (!queue_direct_scanout()) queued_scanout_frame_ = std::move(frame);
        return;
    }
    const auto selected_generation = pending_render_evidence_[*selected_index].generation;
    const auto selected_changed_pixels =
        pending_render_evidence_[*selected_index].changed_pixels;

    queued_guest_frame_proof_ = PvrGuestFrameProof{selected_generation,
                                                   selected_changed_pixels,
                                                   scanout_field,
                                                   *scanout,
                                                   std::move(frame)};
    queued_scanout_frame_.reset();
    for (auto evidence = pending_render_evidence_.begin();
         evidence != pending_render_evidence_.end() &&
         evidence->generation <= selected_generation;) {
        pending_render_evidence_bytes_ -=
            evidence->changed_pixel_values.size() * sizeof(PvrChangedPixelEvidence);
        evidence = pending_render_evidence_.erase(evidence);
    }
    next_evidence_scan_generation_ = pending_render_evidence_.empty()
                                         ? 0u
                                         : pending_render_evidence_.back().generation;
    ++metrics_.proven_guest_frames;
}

std::optional<PvrGuestFrameProof> PvrSoftwareRenderer::take_guest_frame_proof() {
    if (!queued_guest_frame_proof_) return std::nullopt;
    auto proof = std::move(queued_guest_frame_proof_);
    queued_guest_frame_proof_.reset();
    return proof;
}

std::optional<PvrFrame> PvrSoftwareRenderer::take_scanout_frame() {
    if (!queued_scanout_frame_) return std::nullopt;
    auto frame = std::move(queued_scanout_frame_);
    queued_scanout_frame_.reset();
    return frame;
}

const PvrSoftwareRenderMetrics& PvrSoftwareRenderer::metrics() const noexcept {
    return metrics_;
}

std::uint64_t PvrSoftwareRenderer::last_render_generation() const noexcept {
    return last_render_generation_;
}

std::size_t PvrSoftwareRenderer::pending_render_generations() const noexcept {
    return pending_render_evidence_.size();
}

std::size_t PvrSoftwareRenderer::pending_render_evidence_bytes() const noexcept {
    return pending_render_evidence_bytes_;
}

const char* pvr_render_error_name(const PvrRenderError error) noexcept {
    switch (error) {
    case PvrRenderError::InvalidTaState:
        return "invalid-ta-state";
    case PvrRenderError::InvalidConfiguration:
        return "invalid-configuration";
    case PvrRenderError::MemoryRange:
        return "memory-range";
    case PvrRenderError::UnsupportedFeature:
        return "unsupported-feature";
    case PvrRenderError::InternalLifecycle:
        return "internal-lifecycle";
    }
    return "unknown";
}

void PvrSoftwareRenderer::record_error(const PvrRenderError error,
                                       const std::uint64_t render_request,
                                       std::string detail) {
    if (!first_error_)
        first_error_ = PvrRenderFirstError{error, render_request, std::move(detail)};
}

const std::optional<PvrRenderFirstError>& PvrSoftwareRenderer::first_error() const noexcept {
    return first_error_;
}

PvrSoftwareRendererSnapshot PvrSoftwareRenderer::snapshot() const {
    return {
        metrics_,
        next_render_generation_,
        last_render_generation_,
        pending_render_evidence_,
        pending_render_evidence_bytes_,
        next_evidence_scan_generation_,
        next_direct_write_generation_,
        pending_direct_last_write_generation_,
        pending_direct_first_write_generation_,
        pending_direct_last_write_generation_,
        direct_dirty_words_,
        direct_dirty_byte_count_,
        direct_vram_shadow_,
        guest_memory_access_memory_ != nullptr,
        direct_vram_shadow_valid_,
        queued_guest_frame_proof_,
        first_error_,
    };
}

void PvrSoftwareRenderer::validate_state_restore(
    const PvrSoftwareRendererSnapshot& state) const {
    if (state.guest_memory_access_bound !=
        (guest_memory_access_memory_ != nullptr))
        throw std::invalid_argument(
            "PVR-Renderer-Handoff passt nicht zum Runtime-Memory-Sink-Vertrag.");
    if (state.next_render_generation != 0u &&
        state.next_render_generation <= state.last_render_generation)
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt eine zuruecklaufende Generation.");
    if (state.pending_render_evidence.size() >
        render_evidence_capacity)
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt zu viele Evidenzgenerationen.");
    std::size_t evidence_bytes = 0u;
    std::uint64_t previous_generation = 0u;
    bool scan_generation_found =
        state.next_evidence_scan_generation == 0u;
    for (const auto& evidence : state.pending_render_evidence) {
        if (evidence.generation == 0u ||
            evidence.generation <= previous_generation ||
            evidence.generation > state.last_render_generation ||
            evidence.width == 0u || evidence.width > 2048u ||
            evidence.height == 0u || evidence.height > 1024u ||
            (evidence.pixel_bytes != 2u &&
             evidence.pixel_bytes != 3u &&
             evidence.pixel_bytes != 4u) ||
            evidence.stride_bytes <
                static_cast<std::uint64_t>(evidence.width) *
                    evidence.pixel_bytes ||
            evidence.changed_pixels !=
                evidence.changed_pixel_values.size() ||
            evidence.pixel_writes < evidence.changed_pixels ||
            evidence.validation_cursor >
                evidence.changed_pixel_values.size())
            throw std::invalid_argument(
                "PVR-Renderer-Handoff besitzt ungueltige Renderevidenz.");
        for (const auto& pixel : evidence.changed_pixel_values) {
            const auto valid_mask =
                (std::uint32_t{1u} << evidence.pixel_bytes) - 1u;
            if (pixel.offset >= dreamcast_vram_size ||
                evidence.pixel_bytes >
                    dreamcast_vram_size - pixel.offset ||
                pixel.changed_byte_mask == 0u ||
                (pixel.changed_byte_mask & ~valid_mask) != 0u)
                throw std::invalid_argument(
                    "PVR-Renderer-Handoff besitzt ungueltige Pixelevidenz.");
        }
        const auto bytes =
            evidence.changed_pixel_values.size() *
            sizeof(PvrChangedPixelEvidence);
        if (bytes > render_evidence_byte_capacity - evidence_bytes)
            throw std::invalid_argument(
                "PVR-Renderer-Handoff ueberschreitet das Evidenzbudget.");
        evidence_bytes += bytes;
        previous_generation = evidence.generation;
        scan_generation_found =
            scan_generation_found ||
            evidence.generation ==
                state.next_evidence_scan_generation;
    }
    if (evidence_bytes != state.pending_render_evidence_bytes ||
        !scan_generation_found ||
        (state.pending_render_evidence.empty() &&
         state.next_evidence_scan_generation != 0u))
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt inkonsistente Evidenzindizes.");

    constexpr auto dirty_word_count =
        (dreamcast_vram_size + 63u) / 64u;
    if (!state.direct_dirty_words.empty() &&
        state.direct_dirty_words.size() != dirty_word_count)
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt eine ungueltige Dirty-Bitmap.");
    std::size_t dirty_bytes = 0u;
    for (const auto word : state.direct_dirty_words)
        dirty_bytes += std::popcount(word);
    if (dirty_bytes != state.direct_dirty_byte_count)
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt einen inkonsistenten Dirty-Zaehler.");
    if (!state.direct_vram_shadow.empty() &&
        state.direct_vram_shadow.size() != dreamcast_vram_size)
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt kein vollstaendiges VRAM-Schattenabbild.");
    if (!state.direct_vram_shadow_valid &&
        !state.direct_vram_shadow.empty())
        throw std::invalid_argument(
            "PVR-Renderer-Handoff markiert ein vorhandenes Schattenabbild als ungueltig.");
    if (state.pending_direct_write_generation !=
            state.pending_direct_last_write_generation ||
        ((state.pending_direct_first_write_generation == 0u) !=
         (state.pending_direct_last_write_generation == 0u)) ||
        (state.pending_direct_first_write_generation != 0u &&
         state.pending_direct_first_write_generation >
             state.pending_direct_last_write_generation) ||
        (state.pending_direct_last_write_generation != 0u &&
         state.next_direct_write_generation !=
             std::numeric_limits<std::uint64_t>::max() &&
         state.next_direct_write_generation <=
             state.pending_direct_last_write_generation))
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt inkonsistente Direct-Write-Generationen.");
    if (state.queued_guest_frame_proof) {
        const auto& proof = *state.queued_guest_frame_proof;
        if ((proof.source != PvrGuestFrameProofSource::TaRender &&
             proof.source !=
                 PvrGuestFrameProofSource::DirectFramebuffer) ||
            proof.changed_pixels == 0u || proof.frame.width == 0u ||
            proof.frame.height == 0u ||
            proof.frame.width >
                std::numeric_limits<std::size_t>::max() /
                    proof.frame.height ||
            static_cast<std::size_t>(proof.frame.width) *
                    proof.frame.height >
                std::numeric_limits<std::size_t>::max() / 4u ||
            proof.frame.rgba.size() !=
                static_cast<std::size_t>(proof.frame.width) *
                    proof.frame.height * 4u ||
            proof.scanout_field > 1u)
            throw std::invalid_argument(
                "PVR-Renderer-Handoff besitzt einen ungueltigen Framebeweis.");
    }
    if (state.first_error &&
        !valid_pvr_render_error(state.first_error->error))
        throw std::invalid_argument(
            "PVR-Renderer-Handoff besitzt einen ungueltigen Fehler.");
}

void PvrSoftwareRenderer::restore_state_passive(
    PvrSoftwareRendererSnapshot state) {
    validate_state_restore(state);
    metrics_ = state.metrics;
    next_render_generation_ = state.next_render_generation;
    last_render_generation_ = state.last_render_generation;
    pending_render_evidence_ =
        std::move(state.pending_render_evidence);
    pending_render_evidence_bytes_ =
        state.pending_render_evidence_bytes;
    next_evidence_scan_generation_ =
        state.next_evidence_scan_generation;
    next_direct_write_generation_ =
        state.next_direct_write_generation;
    pending_direct_first_write_generation_ =
        state.pending_direct_first_write_generation;
    pending_direct_last_write_generation_ =
        state.pending_direct_last_write_generation;
    direct_dirty_words_ = std::move(state.direct_dirty_words);
    direct_dirty_byte_count_ = state.direct_dirty_byte_count;
    direct_vram_shadow_ = std::move(state.direct_vram_shadow);
    direct_vram_shadow_valid_ = state.direct_vram_shadow_valid;
    queued_guest_frame_proof_ =
        std::move(state.queued_guest_frame_proof);
    queued_scanout_frame_.reset();
    first_error_ = std::move(state.first_error);
}

DreamcastPvrStateSnapshot snapshot_dreamcast_pvr_state(
    const PvrRegisterFile& registers,
    const PvrTaFifo& ta_fifo,
    const PvrTaFifoMemoryDevice& ta_aperture,
    const PvrYuvConverterMemoryDevice& yuv,
    const PvrSoftwareRenderer& renderer) {
    DreamcastPvrStateSnapshot state;
    state.registers = registers.snapshot();
    state.ta_fifo = ta_fifo.snapshot();
    state.ta_aperture = ta_aperture.snapshot();
    state.yuv = yuv.snapshot();
    state.renderer = renderer.snapshot();
    validate_dreamcast_pvr_state_restore(
        registers, ta_fifo, ta_aperture, yuv, renderer, state);
    state.registers.vblank_in_event_rehydration_pending =
        state.registers.vblank_in_event.has_value() ||
        state.registers.vblank_in_event_rehydration_pending;
    state.registers.vblank_out_event_rehydration_pending =
        state.registers.vblank_out_event.has_value() ||
        state.registers.vblank_out_event_rehydration_pending;
    state.registers.hblank_event_rehydration_pending =
        state.registers.hblank_event.has_value() ||
        state.registers.hblank_event_rehydration_pending;
    state.registers.vblank_in_event.reset();
    state.registers.vblank_out_event.reset();
    state.registers.hblank_event.reset();
    detach_pvr_material_graphs(state.ta_fifo);
    validate_dreamcast_pvr_state_restore(
        registers, ta_fifo, ta_aperture, yuv, renderer, state);
    return state;
}

void validate_dreamcast_pvr_state_restore(
    const PvrRegisterFile& registers,
    const PvrTaFifo& ta_fifo,
    const PvrTaFifoMemoryDevice& ta_aperture,
    const PvrYuvConverterMemoryDevice& yuv,
    const PvrSoftwareRenderer& renderer,
    const DreamcastPvrStateSnapshot& state) {
    if (state.contract_version != dreamcast_pvr_state_contract_version)
        throw std::invalid_argument(
            "PVR-Handoff besitzt eine unbekannte Vertragsversion.");
    registers.validate_state_restore(state.registers);
    ta_fifo.validate_state_restore(state.ta_fifo);
    ta_aperture.validate_state_restore(state.ta_aperture);
    yuv.validate_state_restore(state.yuv);
    renderer.validate_state_restore(state.renderer);
}

void restore_dreamcast_pvr_state_passive(
    PvrRegisterFile& registers,
    PvrTaFifo& ta_fifo,
    PvrTaFifoMemoryDevice& ta_aperture,
    PvrYuvConverterMemoryDevice& yuv,
    PvrSoftwareRenderer& renderer,
    DreamcastPvrStateSnapshot state) {
    validate_dreamcast_pvr_state_restore(
        registers, ta_fifo, ta_aperture, yuv, renderer, state);
    ta_fifo.restore_state_passive(std::move(state.ta_fifo));
    ta_aperture.restore_state_passive(std::move(state.ta_aperture));
    yuv.restore_state_passive(std::move(state.yuv));
    renderer.restore_state_passive(std::move(state.renderer));
    registers.restore_state_passive(std::move(state.registers));
}

namespace {

constexpr std::array<std::uint8_t, 8u> pvr_state_magic{
    'K', 'A', 'T', 'P', 'V', 'R', '1', '\n'};
constexpr std::size_t maximum_pvr_state_payload_size = 128u << 20u;
constexpr std::size_t maximum_pvr_state_string_size = 1u << 20u;
constexpr std::size_t maximum_pvr_state_vertices = 2u << 20u;
constexpr std::size_t maximum_pvr_state_primitives = 1u << 20u;
constexpr std::size_t maximum_pvr_state_modifier_volumes = 1u << 20u;

class PvrStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8u));
    }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void f32(const float value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void raw(const std::span<const std::uint8_t> bytes) {
        if (bytes_.size() > maximum_pvr_state_payload_size ||
            bytes.size() >
                maximum_pvr_state_payload_size - bytes_.size())
            throw std::invalid_argument(
                "PVR-Handoff-Payload ueberschreitet das Groessenlimit.");
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    void string(const std::string& value) {
        if (value.size() > maximum_pvr_state_string_size)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt einen zu langen Diagnosetext.");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size()));
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        if (bytes_.size() > maximum_pvr_state_payload_size)
            throw std::invalid_argument(
                "PVR-Handoff-Payload ueberschreitet das Groessenlimit.");
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class PvrStateReader final {
  public:
    explicit PvrStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {
        if (bytes_.size() > maximum_pvr_state_payload_size)
            throw std::invalid_argument(
                "PVR-Handoff-Payload ueberschreitet das Groessenlimit.");
    }
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }
    [[nodiscard]] std::uint16_t u16() {
        require(2u);
        const auto value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[offset_]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes_[offset_ + 1u])
                << 8u));
        offset_ += 2u;
        return value;
    }
    [[nodiscard]] std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 4u;
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        require(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 8u;
        return value;
    }
    [[nodiscard]] float f32() {
        const auto result = std::bit_cast<float>(u32());
        if (!std::isfinite(result))
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt einen nicht endlichen Float.");
        return result;
    }
    void raw(const std::span<std::uint8_t> destination) {
        require(destination.size());
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            destination.size(),
            destination.begin());
        offset_ += destination.size();
    }
    [[nodiscard]] std::vector<std::uint8_t> bytes(
        const std::size_t size,
        const std::size_t maximum) {
        if (size > maximum)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt einen zu grossen Bytevektor.");
        require(size);
        std::vector<std::uint8_t> result(size);
        raw(result);
        return result;
    }
    [[nodiscard]] std::string string() {
        const auto size = u32();
        if (size > maximum_pvr_state_string_size)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt einen zu langen Diagnosetext.");
        require(size);
        std::string result(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            size);
        offset_ += size;
        return result;
    }
    [[nodiscard]] bool matches(
        const std::span<const std::uint8_t> expected) {
        require(expected.size());
        const auto equal = std::equal(
            expected.begin(),
            expected.end(),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ += expected.size();
        return equal;
    }
    void expect_end() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() ||
            size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "PVR-Handoff-Payload ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

template <typename Enum>
void write_pvr_enum(PvrStateWriter& writer, const Enum value) {
    static_assert(std::is_enum_v<Enum>);
    writer.u32(static_cast<std::uint32_t>(value));
}

template <typename Enum>
[[nodiscard]] Enum read_pvr_enum(PvrStateReader& reader) {
    static_assert(std::is_enum_v<Enum>);
    return static_cast<Enum>(reader.u32());
}

void encode_pvr_vertex(PvrStateWriter& writer,
                       const PvrVertex& vertex) {
    validate_pvr_vertex(vertex);
    writer.f32(vertex.x);
    writer.f32(vertex.y);
    writer.f32(vertex.z);
    writer.f32(vertex.u);
    writer.f32(vertex.v);
    writer.u32(vertex.argb);
    writer.u32(vertex.oargb);
    writer.f32(vertex.volume_u);
    writer.f32(vertex.volume_v);
    writer.u32(vertex.volume_argb);
    writer.u32(vertex.volume_oargb);
}

[[nodiscard]] PvrVertex decode_pvr_vertex(PvrStateReader& reader) {
    PvrVertex vertex;
    vertex.x = reader.f32();
    vertex.y = reader.f32();
    vertex.z = reader.f32();
    vertex.u = reader.f32();
    vertex.v = reader.f32();
    vertex.argb = reader.u32();
    vertex.oargb = reader.u32();
    vertex.volume_u = reader.f32();
    vertex.volume_v = reader.f32();
    vertex.volume_argb = reader.u32();
    vertex.volume_oargb = reader.u32();
    return vertex;
}

void encode_pvr_material(PvrStateWriter& writer,
                         const PvrMaterial& material,
                         const std::size_t depth = 0u) {
    validate_pvr_material(material, depth);
    std::uint32_t flags = 0u;
    const std::array values{
        material.gouraud,
        material.textured,
        material.texture_twiddled,
        material.texture_vq,
        material.texture_mipmapped,
        material.texture_x32_stride,
        material.texture_alpha_disabled,
        material.vertex_alpha_enabled,
        material.offset_color_enabled,
        material.color_clamp_enabled,
        material.texture_supersampling,
        material.shadow_enabled,
        material.blend_destination_accumulation,
        material.blend_source_accumulation,
        material.clamp_u,
        material.clamp_v,
        material.flip_u,
        material.flip_v,
        material.depth_write,
    };
    for (std::size_t bit = 0u; bit < values.size(); ++bit)
        if (values[bit]) flags |= std::uint32_t{1u} << bit;
    writer.u32(flags);
    writer.u8(material.depth_compare);
    writer.u8(material.culling);
    writer.u8(material.texture_format);
    writer.u8(material.texture_shading);
    writer.u8(material.texture_filter);
    writer.u8(material.texture_mipmap_bias);
    writer.u8(material.fog_mode);
    writer.u8(material.source_blend);
    writer.u8(material.destination_blend);
    writer.u8(material.palette_bank);
    writer.u8(material.user_clip_mode);
    writer.u16(material.user_clip_start_x);
    writer.u16(material.user_clip_start_y);
    writer.u16(material.user_clip_end_x);
    writer.u16(material.user_clip_end_y);
    writer.u32(material.texture_width);
    writer.u32(material.texture_height);
    writer.u32(material.texture_base);
    writer.u32(material.texture_stride_width);
    writer.boolean(material.volume_material != nullptr);
    if (material.volume_material)
        encode_pvr_material(
            writer, *material.volume_material, depth + 1u);
}

[[nodiscard]] PvrMaterial decode_pvr_material(
    PvrStateReader& reader,
    const std::size_t depth = 0u) {
    if (depth > 8u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu tiefen Materialgraph.");
    PvrMaterial material;
    const auto flags = reader.u32();
    constexpr std::uint32_t valid_flags =
        (std::uint32_t{1u} << 19u) - 1u;
    if ((flags & ~valid_flags) != 0u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt ungueltige Materialflags.");
    std::array<bool*, 19u> values{
        &material.gouraud,
        &material.textured,
        &material.texture_twiddled,
        &material.texture_vq,
        &material.texture_mipmapped,
        &material.texture_x32_stride,
        &material.texture_alpha_disabled,
        &material.vertex_alpha_enabled,
        &material.offset_color_enabled,
        &material.color_clamp_enabled,
        &material.texture_supersampling,
        &material.shadow_enabled,
        &material.blend_destination_accumulation,
        &material.blend_source_accumulation,
        &material.clamp_u,
        &material.clamp_v,
        &material.flip_u,
        &material.flip_v,
        &material.depth_write,
    };
    for (std::size_t bit = 0u; bit < values.size(); ++bit)
        *values[bit] = (flags & (std::uint32_t{1u} << bit)) != 0u;
    material.depth_compare = reader.u8();
    material.culling = reader.u8();
    material.texture_format = reader.u8();
    material.texture_shading = reader.u8();
    material.texture_filter = reader.u8();
    material.texture_mipmap_bias = reader.u8();
    material.fog_mode = reader.u8();
    material.source_blend = reader.u8();
    material.destination_blend = reader.u8();
    material.palette_bank = reader.u8();
    material.user_clip_mode = reader.u8();
    material.user_clip_start_x = reader.u16();
    material.user_clip_start_y = reader.u16();
    material.user_clip_end_x = reader.u16();
    material.user_clip_end_y = reader.u16();
    material.texture_width = reader.u32();
    material.texture_height = reader.u32();
    material.texture_base = reader.u32();
    material.texture_stride_width = reader.u32();
    if (reader.boolean())
        material.volume_material = std::make_shared<PvrMaterial>(
            decode_pvr_material(reader, depth + 1u));
    validate_pvr_material(material);
    return material;
}

void encode_pvr_primitive(PvrStateWriter& writer,
                          const PvrPrimitive& primitive) {
    validate_pvr_primitive(primitive);
    write_pvr_enum(writer, primitive.list);
    if (primitive.vertices.size() > maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Vertices.");
    writer.u32(
        static_cast<std::uint32_t>(primitive.vertices.size()));
    for (const auto& vertex : primitive.vertices)
        encode_pvr_vertex(writer, vertex);
    encode_pvr_material(writer, primitive.material);
}

[[nodiscard]] PvrPrimitive decode_pvr_primitive(
    PvrStateReader& reader) {
    PvrPrimitive primitive;
    primitive.list = read_pvr_enum<PvrListType>(reader);
    const auto count = reader.u32();
    if (count > maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Vertices.");
    primitive.vertices.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index)
        primitive.vertices.push_back(decode_pvr_vertex(reader));
    primitive.material = decode_pvr_material(reader);
    validate_pvr_primitive(primitive);
    return primitive;
}

void encode_pvr_modifier_volume(PvrStateWriter& writer,
                                const PvrModifierVolume& volume) {
    validate_pvr_modifier_volume(volume);
    write_pvr_enum(writer, volume.list);
    if (volume.triangles.size() > maximum_pvr_state_vertices / 3u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Modifierdreiecke.");
    writer.u32(
        static_cast<std::uint32_t>(volume.triangles.size()));
    for (const auto& triangle : volume.triangles)
        for (const auto& vertex : triangle)
            encode_pvr_vertex(writer, vertex);
    writer.u8(volume.depth_mode);
    writer.u8(volume.culling);
    writer.u8(volume.user_clip_mode);
    writer.u16(volume.user_clip_start_x);
    writer.u16(volume.user_clip_start_y);
    writer.u16(volume.user_clip_end_x);
    writer.u16(volume.user_clip_end_y);
    writer.boolean(volume.volume_last);
}

[[nodiscard]] PvrModifierVolume decode_pvr_modifier_volume(
    PvrStateReader& reader) {
    PvrModifierVolume volume;
    volume.list = read_pvr_enum<PvrListType>(reader);
    const auto count = reader.u32();
    if (count > maximum_pvr_state_vertices / 3u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Modifierdreiecke.");
    volume.triangles.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        std::array<PvrVertex, 3u> triangle;
        for (auto& vertex : triangle)
            vertex = decode_pvr_vertex(reader);
        volume.triangles.push_back(std::move(triangle));
    }
    volume.depth_mode = reader.u8();
    volume.culling = reader.u8();
    volume.user_clip_mode = reader.u8();
    volume.user_clip_start_x = reader.u16();
    volume.user_clip_start_y = reader.u16();
    volume.user_clip_end_x = reader.u16();
    volume.user_clip_end_y = reader.u16();
    volume.volume_last = reader.boolean();
    validate_pvr_modifier_volume(volume);
    return volume;
}

void encode_tile_accelerator(PvrStateWriter& writer,
                             const TileAcceleratorSnapshot& state) {
    if (state.primitives.size() > maximum_pvr_state_primitives ||
        state.current_strip.size() > maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viel TA-Geometrie.");
    writer.u32(static_cast<std::uint32_t>(state.primitives.size()));
    for (const auto& primitive : state.primitives)
        encode_pvr_primitive(writer, primitive);
    writer.u32(static_cast<std::uint32_t>(state.current_strip.size()));
    for (const auto& vertex : state.current_strip)
        encode_pvr_vertex(writer, vertex);
    write_pvr_enum(writer, state.current_list);
    encode_pvr_material(writer, state.current_material);
    writer.u8(state.highest_list_rank);
    writer.boolean(state.frame_has_list);
    writer.boolean(state.list_open);
}

[[nodiscard]] TileAcceleratorSnapshot decode_tile_accelerator(
    PvrStateReader& reader) {
    TileAcceleratorSnapshot state;
    const auto primitive_count = reader.u32();
    if (primitive_count > maximum_pvr_state_primitives)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Primitive.");
    state.primitives.reserve(primitive_count);
    for (std::uint32_t index = 0u; index < primitive_count; ++index)
        state.primitives.push_back(decode_pvr_primitive(reader));
    const auto strip_count = reader.u32();
    if (strip_count > maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen TA-Strip.");
    state.current_strip.reserve(strip_count);
    for (std::uint32_t index = 0u; index < strip_count; ++index)
        state.current_strip.push_back(decode_pvr_vertex(reader));
    state.current_list = read_pvr_enum<PvrListType>(reader);
    state.current_material = decode_pvr_material(reader);
    state.highest_list_rank = reader.u8();
    state.frame_has_list = reader.boolean();
    state.list_open = reader.boolean();
    TileAccelerator validator;
    validator.validate_state_restore(state);
    return state;
}

void encode_ta_metrics(PvrStateWriter& writer,
                       const PvrTaMetrics& metrics) {
    writer.u64(metrics.packets);
    for (const auto count : metrics.normalized_packets)
        writer.u64(count);
    writer.u64(metrics.polygon_headers);
    writer.u64(metrics.vertices);
    writer.u64(metrics.list_completions);
    writer.u64(metrics.frames);
    writer.u64(metrics.continuations);
    writer.u64(metrics.rejected_packets);
}

[[nodiscard]] PvrTaMetrics decode_ta_metrics(PvrStateReader& reader) {
    PvrTaMetrics metrics;
    metrics.packets = reader.u64();
    for (auto& count : metrics.normalized_packets)
        count = reader.u64();
    metrics.polygon_headers = reader.u64();
    metrics.vertices = reader.u64();
    metrics.list_completions = reader.u64();
    metrics.frames = reader.u64();
    metrics.continuations = reader.u64();
    metrics.rejected_packets = reader.u64();
    return metrics;
}

void encode_ta_fifo(PvrStateWriter& writer,
                    const PvrTaFifoSnapshot& state) {
    PvrTaFifo validator;
    validator.validate_state_restore(state);
    encode_tile_accelerator(writer, state.accelerator);
    write_pvr_enum(writer, state.active_list);
    writer.boolean(state.active_textured);
    writer.boolean(state.active_uv16);
    writer.u8(state.active_color_type);
    writer.boolean(state.active_sprite);
    writer.boolean(state.active_two_volume);
    writer.u32(state.active_header_argb);
    writer.u32(state.active_header_oargb);
    writer.u32(state.active_volume_header_argb);
    writer.boolean(state.intensity_face_color_valid);
    encode_pvr_material(writer, state.active_material);
    writer.u16(state.user_clip_start_x);
    writer.u16(state.user_clip_start_y);
    writer.u16(state.user_clip_end_x);
    writer.u16(state.user_clip_end_y);
    writer.boolean(state.pending_sprite_vertex.has_value());
    if (state.pending_sprite_vertex)
        writer.raw(*state.pending_sprite_vertex);
    writer.boolean(state.pending_extended_vertex.has_value());
    if (state.pending_extended_vertex)
        encode_pvr_vertex(writer, *state.pending_extended_vertex);
    writer.boolean(state.pending_intensity_header);
    writer.boolean(state.pending_extended_end_of_strip);
    if (state.modifier_volumes.size() >
        maximum_pvr_state_modifier_volumes)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Modifier-Volumes.");
    writer.u32(
        static_cast<std::uint32_t>(state.modifier_volumes.size()));
    for (const auto& volume : state.modifier_volumes)
        encode_pvr_modifier_volume(writer, volume);
    writer.boolean(state.active_modifier_volume.has_value());
    if (state.active_modifier_volume) {
        if (*state.active_modifier_volume >
            std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt einen zu grossen Modifierindex.");
        writer.u32(static_cast<std::uint32_t>(
            *state.active_modifier_volume));
    }
    writer.boolean(
        state.pending_modifier_vertex_packet.has_value());
    if (state.pending_modifier_vertex_packet)
        writer.raw(*state.pending_modifier_vertex_packet);
    encode_ta_metrics(writer, state.metrics);
    writer.u64(state.frame_packets);
    writer.boolean(state.first_input_error.has_value());
    if (state.first_input_error) {
        write_pvr_enum(writer, state.first_input_error->reason);
        writer.u64(state.first_input_error->packet);
        writer.string(state.first_input_error->detail);
    }
}

[[nodiscard]] PvrTaFifoSnapshot decode_ta_fifo(
    PvrStateReader& reader) {
    PvrTaFifoSnapshot state;
    state.accelerator = decode_tile_accelerator(reader);
    state.active_list = read_pvr_enum<PvrListType>(reader);
    state.active_textured = reader.boolean();
    state.active_uv16 = reader.boolean();
    state.active_color_type = reader.u8();
    state.active_sprite = reader.boolean();
    state.active_two_volume = reader.boolean();
    state.active_header_argb = reader.u32();
    state.active_header_oargb = reader.u32();
    state.active_volume_header_argb = reader.u32();
    state.intensity_face_color_valid = reader.boolean();
    state.active_material = decode_pvr_material(reader);
    state.user_clip_start_x = reader.u16();
    state.user_clip_start_y = reader.u16();
    state.user_clip_end_x = reader.u16();
    state.user_clip_end_y = reader.u16();
    if (reader.boolean()) {
        std::array<std::uint8_t, 32u> packet{};
        reader.raw(packet);
        state.pending_sprite_vertex = packet;
    }
    if (reader.boolean())
        state.pending_extended_vertex = decode_pvr_vertex(reader);
    state.pending_intensity_header = reader.boolean();
    state.pending_extended_end_of_strip = reader.boolean();
    const auto volume_count = reader.u32();
    if (volume_count > maximum_pvr_state_modifier_volumes)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Modifier-Volumes.");
    state.modifier_volumes.reserve(volume_count);
    for (std::uint32_t index = 0u; index < volume_count; ++index)
        state.modifier_volumes.push_back(
            decode_pvr_modifier_volume(reader));
    if (reader.boolean())
        state.active_modifier_volume = reader.u32();
    if (reader.boolean()) {
        std::array<std::uint8_t, 32u> packet{};
        reader.raw(packet);
        state.pending_modifier_vertex_packet = packet;
    }
    state.metrics = decode_ta_metrics(reader);
    state.frame_packets = reader.u64();
    if (reader.boolean()) {
        state.first_input_error = PvrTaInputError{
            read_pvr_enum<PvrTaInputErrorReason>(reader),
            reader.u64(),
            reader.string(),
        };
    }
    PvrTaFifo validator;
    validator.validate_state_restore(state);
    return state;
}

void encode_pvr_registers(PvrStateWriter& writer,
                          const PvrRegisterSnapshot& state) {
    if (!state.render_event_ids.empty() || state.vblank_in_event ||
        state.vblank_out_event || state.hblank_event)
        throw std::invalid_argument(
            "PVR-Handoff-Payload darf keine SchedulerEventIds enthalten.");
    for (const auto value : state.registers) writer.u32(value);
    writer.u32(state.framebuffer_read_control);
    writer.u32(state.framebuffer_read_size);
    writer.u32(state.framebuffer_read_sof1);
    writer.u32(state.framebuffer_read_sof2);
    writer.u32(state.framebuffer_write_control);
    writer.u32(state.framebuffer_write_sof1);
    writer.u32(state.framebuffer_write_sof2);
    writer.u32(state.video_control);
    writer.u64(state.render_requests);
    writer.u64(state.render_completions);
    writer.u64(state.render_failures);
    writer.u64(state.render_overruns);
    writer.u64(state.vblank_in);
    writer.u64(state.vblank_out);
    writer.u64(state.hblank);
    writer.u64(state.resets);
    writer.boolean(state.vblank_in_event_rehydration_pending);
    writer.boolean(state.vblank_out_event_rehydration_pending);
    writer.boolean(state.hblank_event_rehydration_pending);
    writer.boolean(state.hblank_event_line.has_value());
    if (state.hblank_event_line) writer.u32(*state.hblank_event_line);
    writer.u64(state.scan_frame_cycles);
    writer.u64(state.scan_epoch_cycle);
    writer.u64(state.timing.render_latency);
    writer.u64(state.timing.guest_clock_hz);
    writer.u64(state.timing.pixel_clock_hz);
    writer.boolean(state.in_vblank);
    writer.u32(state.field);
    writer.u64(state.next_render_generation);
    writer.u64(state.active_render_request);
    writer.u64(state.active_render_generation);
    writer.u64(state.active_render_start_cycle);
    writer.u64(state.active_render_payload_digest);
    writer.boolean(state.last_render_start_error.has_value());
    if (state.last_render_start_error)
        write_pvr_enum(writer, *state.last_render_start_error);
    writer.boolean(state.last_render_failure.has_value());
    if (state.last_render_failure) {
        const auto& failure = *state.last_render_failure;
        writer.u64(failure.request);
        writer.u64(failure.generation);
        write_pvr_enum(writer, failure.error);
        writer.string(failure.ta_packet_class);
        writer.u64(failure.register_digest);
        writer.u64(failure.guest_cycle);
        writer.string(failure.detail);
    }
}

[[nodiscard]] PvrRegisterSnapshot decode_pvr_registers(
    PvrStateReader& reader) {
    PvrRegisterSnapshot state;
    for (auto& value : state.registers) value = reader.u32();
    state.framebuffer_read_control = reader.u32();
    state.framebuffer_read_size = reader.u32();
    state.framebuffer_read_sof1 = reader.u32();
    state.framebuffer_read_sof2 = reader.u32();
    state.framebuffer_write_control = reader.u32();
    state.framebuffer_write_sof1 = reader.u32();
    state.framebuffer_write_sof2 = reader.u32();
    state.video_control = reader.u32();
    state.render_requests = reader.u64();
    state.render_completions = reader.u64();
    state.render_failures = reader.u64();
    state.render_overruns = reader.u64();
    state.vblank_in = reader.u64();
    state.vblank_out = reader.u64();
    state.hblank = reader.u64();
    state.resets = reader.u64();
    state.vblank_in_event_rehydration_pending = reader.boolean();
    state.vblank_out_event_rehydration_pending = reader.boolean();
    state.hblank_event_rehydration_pending = reader.boolean();
    if (reader.boolean()) state.hblank_event_line = reader.u32();
    state.scan_frame_cycles = reader.u64();
    state.scan_epoch_cycle = reader.u64();
    state.timing.render_latency = reader.u64();
    state.timing.guest_clock_hz = reader.u64();
    state.timing.pixel_clock_hz = reader.u64();
    state.in_vblank = reader.boolean();
    state.field = reader.u32();
    state.next_render_generation = reader.u64();
    state.active_render_request = reader.u64();
    state.active_render_generation = reader.u64();
    state.active_render_start_cycle = reader.u64();
    state.active_render_payload_digest = reader.u64();
    if (reader.boolean())
        state.last_render_start_error =
            read_pvr_enum<PvrRenderStartError>(reader);
    if (reader.boolean()) {
        PvrRenderFailure failure;
        failure.request = reader.u64();
        failure.generation = reader.u64();
        failure.error = read_pvr_enum<PvrRenderError>(reader);
        failure.ta_packet_class = reader.string();
        failure.register_digest = reader.u64();
        failure.guest_cycle = reader.u64();
        failure.detail = reader.string();
        state.last_render_failure = std::move(failure);
    }
    return state;
}

void encode_ta_aperture(
    PvrStateWriter& writer,
    const PvrTaFifoMemoryDevice::Snapshot& state) {
    writer.raw(state.packet);
    writer.u32(state.packet_base);
    writer.u32(state.written_mask);
    writer.boolean(state.packet_active);
}

[[nodiscard]] PvrTaFifoMemoryDevice::Snapshot decode_ta_aperture(
    PvrStateReader& reader) {
    PvrTaFifoMemoryDevice::Snapshot state;
    reader.raw(state.packet);
    state.packet_base = reader.u32();
    state.written_mask = reader.u32();
    state.packet_active = reader.boolean();
    return state;
}

void encode_yuv(PvrStateWriter& writer,
                const PvrYuvConverterMemoryDevice::Snapshot& state) {
    if (state.input.size() > 512u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen YUV-Puffer.");
    writer.u32(static_cast<std::uint32_t>(state.input.size()));
    writer.raw(state.input);
    writer.u32(state.configuration);
    writer.u32(state.destination);
    writer.u32(state.frame_macroblock);
    writer.u64(state.converted_macroblocks);
    writer.boolean(state.guest_memory_access_bound);
}

[[nodiscard]] PvrYuvConverterMemoryDevice::Snapshot decode_yuv(
    PvrStateReader& reader) {
    PvrYuvConverterMemoryDevice::Snapshot state;
    state.input = reader.bytes(reader.u32(), 512u);
    state.configuration = reader.u32();
    state.destination = reader.u32();
    state.frame_macroblock = reader.u32();
    state.converted_macroblocks = reader.u64();
    state.guest_memory_access_bound = reader.boolean();
    return state;
}

void encode_render_metrics(PvrStateWriter& writer,
                           const PvrSoftwareRenderMetrics& metrics) {
    writer.u64(metrics.frames);
    writer.u64(metrics.triangles);
    writer.u64(metrics.pixels);
    writer.u64(metrics.pixel_writes);
    writer.u64(metrics.changed_pixels);
    writer.u64(metrics.proven_guest_frames);
    writer.u64(metrics.direct_scanout_frames);
    writer.u64(metrics.direct_scanout_changed_pixels);
    writer.u64(metrics.last_frame_pixel_writes);
    writer.u64(metrics.last_frame_changed_pixels);
    writer.u64(metrics.dropped_render_evidence_generations);
    writer.u64(metrics.render_evidence_pixels_examined);
    writer.u64(metrics.render_evidence_range_rejections);
    writer.u64(metrics.render_evidence_scan_budget_exhaustions);
}

[[nodiscard]] PvrSoftwareRenderMetrics decode_render_metrics(
    PvrStateReader& reader) {
    PvrSoftwareRenderMetrics metrics;
    metrics.frames = reader.u64();
    metrics.triangles = reader.u64();
    metrics.pixels = reader.u64();
    metrics.pixel_writes = reader.u64();
    metrics.changed_pixels = reader.u64();
    metrics.proven_guest_frames = reader.u64();
    metrics.direct_scanout_frames = reader.u64();
    metrics.direct_scanout_changed_pixels = reader.u64();
    metrics.last_frame_pixel_writes = reader.u64();
    metrics.last_frame_changed_pixels = reader.u64();
    metrics.dropped_render_evidence_generations = reader.u64();
    metrics.render_evidence_pixels_examined = reader.u64();
    metrics.render_evidence_range_rejections = reader.u64();
    metrics.render_evidence_scan_budget_exhaustions = reader.u64();
    return metrics;
}

void encode_render_evidence(
    PvrStateWriter& writer,
    const PvrRenderGenerationEvidence& evidence) {
    writer.u64(evidence.generation);
    writer.u32(evidence.write_base);
    writer.u32(evidence.stride_bytes);
    writer.u32(evidence.width);
    writer.u32(evidence.height);
    writer.u8(evidence.pixel_bytes);
    writer.boolean(evidence.render_to_texture);
    writer.u64(evidence.pixel_writes);
    writer.u64(evidence.changed_pixels);
    writer.u64(evidence.validation_cursor);
    if (evidence.changed_pixel_values.size() >
        maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Evidenzpixel.");
    writer.u32(static_cast<std::uint32_t>(
        evidence.changed_pixel_values.size()));
    for (const auto& pixel : evidence.changed_pixel_values) {
        writer.u32(pixel.offset);
        writer.u32(pixel.packed_value);
        writer.u32(pixel.changed_byte_mask);
    }
}

[[nodiscard]] PvrRenderGenerationEvidence decode_render_evidence(
    PvrStateReader& reader) {
    PvrRenderGenerationEvidence evidence;
    evidence.generation = reader.u64();
    evidence.write_base = reader.u32();
    evidence.stride_bytes = reader.u32();
    evidence.width = reader.u32();
    evidence.height = reader.u32();
    evidence.pixel_bytes = reader.u8();
    evidence.render_to_texture = reader.boolean();
    evidence.pixel_writes = reader.u64();
    evidence.changed_pixels = reader.u64();
    const auto cursor = reader.u64();
    if (cursor > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen Evidenzcursor.");
    evidence.validation_cursor = static_cast<std::size_t>(cursor);
    const auto count = reader.u32();
    if (count > maximum_pvr_state_vertices)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Evidenzpixel.");
    evidence.changed_pixel_values.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        evidence.changed_pixel_values.push_back({
            reader.u32(),
            reader.u32(),
            reader.u32(),
        });
    }
    return evidence;
}

void encode_scanout(PvrStateWriter& writer,
                    const PvrScanoutDescriptor& scanout) {
    writer.u32(scanout.width);
    writer.u32(scanout.height);
    writer.u32(scanout.source_width);
    writer.u32(scanout.source_height);
    writer.u32(scanout.stride_bytes);
    writer.u64(scanout.base_offset);
    writer.u64(scanout.second_base_offset);
    write_pvr_enum(writer, scanout.format);
    writer.u8(scanout.concat);
    writer.boolean(scanout.line_double);
    writer.boolean(scanout.interlaced);
    writer.boolean(scanout.weave_fields);
    writer.boolean(scanout.horizontal_scale);
    writer.u16(scanout.vertical_scale_factor);
    writer.boolean(scanout.video_blank);
    writer.raw(scanout.border_rgba);
}

[[nodiscard]] PvrScanoutDescriptor decode_scanout(
    PvrStateReader& reader) {
    PvrScanoutDescriptor scanout;
    scanout.width = reader.u32();
    scanout.height = reader.u32();
    scanout.source_width = reader.u32();
    scanout.source_height = reader.u32();
    scanout.stride_bytes = reader.u32();
    const auto base = reader.u64();
    const auto second = reader.u64();
    if (base > std::numeric_limits<std::size_t>::max() ||
        second > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen Scanoutoffset.");
    scanout.base_offset = static_cast<std::size_t>(base);
    scanout.second_base_offset = static_cast<std::size_t>(second);
    scanout.format = read_pvr_enum<PvrFramebufferFormat>(reader);
    if (scanout.format != PvrFramebufferFormat::Rgb565 &&
        scanout.format != PvrFramebufferFormat::Rgb0555 &&
        scanout.format != PvrFramebufferFormat::Rgb888 &&
        scanout.format != PvrFramebufferFormat::Rgb0888)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt ein ungueltiges Scanoutformat.");
    scanout.concat = reader.u8();
    scanout.line_double = reader.boolean();
    scanout.interlaced = reader.boolean();
    scanout.weave_fields = reader.boolean();
    scanout.horizontal_scale = reader.boolean();
    scanout.vertical_scale_factor = reader.u16();
    scanout.video_blank = reader.boolean();
    reader.raw(scanout.border_rgba);
    return scanout;
}

void encode_guest_frame_proof(PvrStateWriter& writer,
                              const PvrGuestFrameProof& proof) {
    writer.u64(proof.render_generation);
    writer.u64(proof.changed_pixels);
    writer.u32(proof.scanout_field);
    encode_scanout(writer, proof.scanout);
    writer.u32(proof.frame.width);
    writer.u32(proof.frame.height);
    if (proof.frame.rgba.size() > 16u * dreamcast_vram_size)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen Framebeweis.");
    writer.u32(static_cast<std::uint32_t>(proof.frame.rgba.size()));
    writer.raw(proof.frame.rgba);
    write_pvr_enum(writer, proof.source);
    writer.u64(proof.write_generation_first);
    writer.u64(proof.write_generation_last);
}

[[nodiscard]] PvrGuestFrameProof decode_guest_frame_proof(
    PvrStateReader& reader) {
    PvrGuestFrameProof proof;
    proof.render_generation = reader.u64();
    proof.changed_pixels = reader.u64();
    proof.scanout_field = reader.u32();
    proof.scanout = decode_scanout(reader);
    proof.frame.width = reader.u32();
    proof.frame.height = reader.u32();
    proof.frame.rgba =
        reader.bytes(reader.u32(), 16u * dreamcast_vram_size);
    proof.source =
        read_pvr_enum<PvrGuestFrameProofSource>(reader);
    proof.write_generation_first = reader.u64();
    proof.write_generation_last = reader.u64();
    return proof;
}

void encode_renderer(PvrStateWriter& writer,
                     const PvrSoftwareRendererSnapshot& state) {
    encode_render_metrics(writer, state.metrics);
    writer.u64(state.next_render_generation);
    writer.u64(state.last_render_generation);
    if (state.pending_render_evidence.size() >
        PvrSoftwareRenderer::render_evidence_capacity)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Renderevidenzen.");
    writer.u32(static_cast<std::uint32_t>(
        state.pending_render_evidence.size()));
    for (const auto& evidence : state.pending_render_evidence)
        encode_render_evidence(writer, evidence);
    writer.u64(state.pending_render_evidence_bytes);
    writer.u64(state.next_evidence_scan_generation);
    writer.u64(state.next_direct_write_generation);
    writer.u64(state.pending_direct_write_generation);
    writer.u64(state.pending_direct_first_write_generation);
    writer.u64(state.pending_direct_last_write_generation);
    if (state.direct_dirty_words.size() >
        (dreamcast_vram_size + 63u) / 64u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt eine zu grosse Dirty-Bitmap.");
    writer.u32(
        static_cast<std::uint32_t>(state.direct_dirty_words.size()));
    for (const auto word : state.direct_dirty_words) writer.u64(word);
    writer.u64(state.direct_dirty_byte_count);
    if (state.direct_vram_shadow.size() > dreamcast_vram_size)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt ein zu grosses VRAM-Schattenabbild.");
    writer.u32(
        static_cast<std::uint32_t>(state.direct_vram_shadow.size()));
    writer.raw(state.direct_vram_shadow);
    writer.boolean(state.guest_memory_access_bound);
    writer.boolean(state.direct_vram_shadow_valid);
    writer.boolean(state.queued_guest_frame_proof.has_value());
    if (state.queued_guest_frame_proof)
        encode_guest_frame_proof(
            writer, *state.queued_guest_frame_proof);
    writer.boolean(state.first_error.has_value());
    if (state.first_error) {
        write_pvr_enum(writer, state.first_error->error);
        writer.u64(state.first_error->render_request);
        writer.string(state.first_error->detail);
    }
}

[[nodiscard]] PvrSoftwareRendererSnapshot decode_renderer(
    PvrStateReader& reader) {
    PvrSoftwareRendererSnapshot state;
    state.metrics = decode_render_metrics(reader);
    state.next_render_generation = reader.u64();
    state.last_render_generation = reader.u64();
    const auto evidence_count = reader.u32();
    if (evidence_count >
        PvrSoftwareRenderer::render_evidence_capacity)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt zu viele Renderevidenzen.");
    for (std::uint32_t index = 0u; index < evidence_count; ++index)
        state.pending_render_evidence.push_back(
            decode_render_evidence(reader));
    const auto evidence_bytes = reader.u64();
    if (evidence_bytes > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen Evidenzzaehler.");
    state.pending_render_evidence_bytes =
        static_cast<std::size_t>(evidence_bytes);
    state.next_evidence_scan_generation = reader.u64();
    state.next_direct_write_generation = reader.u64();
    state.pending_direct_write_generation = reader.u64();
    state.pending_direct_first_write_generation = reader.u64();
    state.pending_direct_last_write_generation = reader.u64();
    const auto dirty_count = reader.u32();
    if (dirty_count > (dreamcast_vram_size + 63u) / 64u)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt eine zu grosse Dirty-Bitmap.");
    state.direct_dirty_words.reserve(dirty_count);
    for (std::uint32_t index = 0u; index < dirty_count; ++index)
        state.direct_dirty_words.push_back(reader.u64());
    const auto dirty_bytes = reader.u64();
    if (dirty_bytes > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen zu grossen Dirty-Zaehler.");
    state.direct_dirty_byte_count =
        static_cast<std::size_t>(dirty_bytes);
    state.direct_vram_shadow =
        reader.bytes(reader.u32(), dreamcast_vram_size);
    state.guest_memory_access_bound = reader.boolean();
    state.direct_vram_shadow_valid = reader.boolean();
    if (reader.boolean())
        state.queued_guest_frame_proof =
            decode_guest_frame_proof(reader);
    if (reader.boolean()) {
        state.first_error = PvrRenderFirstError{
            read_pvr_enum<PvrRenderError>(reader),
            reader.u64(),
            reader.string(),
        };
    }
    return state;
}

void validate_pvr_payload_shape(
    const DreamcastPvrStateSnapshot& state) {
    if (state.contract_version != dreamcast_pvr_state_contract_version)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen inkompatiblen Vertrag.");

    EventScheduler scheduler;
    PvrRegisterFile register_validator(
        scheduler, state.registers.timing);
    register_validator.validate_state_restore(state.registers);
    if (state.registers.vblank_in_event ||
        state.registers.vblank_out_event ||
        state.registers.hblank_event ||
        !state.registers.render_event_ids.empty())
        throw std::invalid_argument(
            "PVR-Handoff-Payload darf keine SchedulerEventIds enthalten.");

    PvrTaFifo fifo_validator;
    fifo_validator.validate_state_restore(state.ta_fifo);
    auto aperture_fifo = std::make_shared<PvrTaFifo>();
    PvrTaFifoMemoryDevice aperture_validator(aperture_fifo);
    aperture_validator.validate_state_restore(state.ta_aperture);

    const auto& yuv = state.yuv;
    const auto unconfigured =
        yuv.configuration == std::numeric_limits<std::uint32_t>::max() &&
        yuv.destination == std::numeric_limits<std::uint32_t>::max();
    if (unconfigured) {
        if (!yuv.input.empty() || yuv.frame_macroblock != 0u)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt YUV-Daten ohne Konfiguration.");
    } else {
        if (yuv.configuration ==
                std::numeric_limits<std::uint32_t>::max() ||
            yuv.destination ==
                std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt eine unvollstaendige YUV-Konfiguration.");
        const auto macroblock_size =
            (yuv.configuration & 0x01000000u) != 0u ? 512u : 384u;
        const auto blocks_x =
            static_cast<std::uint64_t>(yuv.configuration & 0x3Fu) + 1u;
        const auto blocks_y =
            static_cast<std::uint64_t>(
                (yuv.configuration >> 8u) & 0x3Fu) +
            1u;
        const auto total_blocks = blocks_x * blocks_y;
        const auto frame_bytes =
            blocks_x * 16u * blocks_y * 16u * 2u;
        if (yuv.input.size() >= macroblock_size ||
            yuv.frame_macroblock > total_blocks ||
            yuv.destination >= dreamcast_vram_size ||
            frame_bytes > dreamcast_vram_size - yuv.destination)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt ungueltigen YUV-Zustand.");
    }

    PvrSoftwareRenderer renderer_validator;
    Memory memory_marker(1u);
    if (state.renderer.guest_memory_access_bound)
        renderer_validator.set_guest_memory_access_memory(&memory_marker);
    renderer_validator.validate_state_restore(state.renderer);
    if (state.renderer.queued_guest_frame_proof) {
        const auto format =
            state.renderer.queued_guest_frame_proof->scanout.format;
        if (format != PvrFramebufferFormat::Rgb565 &&
            format != PvrFramebufferFormat::Rgb0555 &&
            format != PvrFramebufferFormat::Rgb888 &&
            format != PvrFramebufferFormat::Rgb0888)
            throw std::invalid_argument(
                "PVR-Handoff-Payload besitzt ein ungueltiges Scanoutformat.");
    }
}

} // namespace

std::vector<std::uint8_t> encode_dreamcast_pvr_state(
    const DreamcastPvrStateSnapshot& state) {
    validate_pvr_payload_shape(state);
    PvrStateWriter writer;
    writer.raw(pvr_state_magic);
    writer.u32(dreamcast_pvr_state_contract_version);
    encode_pvr_registers(writer, state.registers);
    encode_ta_fifo(writer, state.ta_fifo);
    encode_ta_aperture(writer, state.ta_aperture);
    encode_yuv(writer, state.yuv);
    encode_renderer(writer, state.renderer);
    return std::move(writer).finish();
}

DreamcastPvrStateSnapshot decode_dreamcast_pvr_state(
    const std::span<const std::uint8_t> bytes) {
    PvrStateReader reader(bytes);
    if (!reader.matches(pvr_state_magic))
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt keine gueltige Signatur.");
    if (reader.u32() != dreamcast_pvr_state_contract_version)
        throw std::invalid_argument(
            "PVR-Handoff-Payload besitzt einen inkompatiblen Vertrag.");
    DreamcastPvrStateSnapshot state;
    state.registers = decode_pvr_registers(reader);
    state.ta_fifo = decode_ta_fifo(reader);
    state.ta_aperture = decode_ta_aperture(reader);
    state.yuv = decode_yuv(reader);
    state.renderer = decode_renderer(reader);
    reader.expect_end();
    validate_pvr_payload_shape(state);
    return state;
}

PvrTexture decode_pvr_texture(const std::span<const std::uint8_t> source,
                              const std::uint32_t width,
                              const std::uint32_t height,
                              const PvrTextureFormat format) {
    if (width == 0u || height == 0u) {
        throw std::invalid_argument("PVR-Texturen brauchen eine von null verschiedene Geometrie.");
    }
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (static_cast<std::size_t>(height) > maximum / width) {
        throw std::out_of_range("PVR-Texturgeometrie ist zu gross.");
    }
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (pixels > maximum / 4u || source.size() < pixels * 2u) {
        throw std::out_of_range("PVR-Textur liegt ausserhalb der Quelldaten.");
    }
    PvrTexture texture{width, height, std::vector<std::uint8_t>(pixels * 4u)};
    for (std::size_t index = 0u; index < pixels; ++index) {
        const auto pixel = static_cast<std::uint16_t>(source[index * 2u]) |
                           static_cast<std::uint16_t>(source[index * 2u + 1u] << 8u);
        const auto destination = index * 4u;
        if (format == PvrTextureFormat::Rgb565) {
            texture.rgba[destination] = expand5(static_cast<std::uint16_t>((pixel >> 11u) & 0x1Fu));
            texture.rgba[destination + 1u] =
                expand6(static_cast<std::uint16_t>((pixel >> 5u) & 0x3Fu));
            texture.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
            texture.rgba[destination + 3u] = 0xFFu;
        } else if (format == PvrTextureFormat::Argb1555) {
            texture.rgba[destination] = expand5(static_cast<std::uint16_t>((pixel >> 10u) & 0x1Fu));
            texture.rgba[destination + 1u] =
                expand5(static_cast<std::uint16_t>((pixel >> 5u) & 0x1Fu));
            texture.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
            texture.rgba[destination + 3u] = (pixel & 0x8000u) != 0u ? 0xFFu : 0u;
        } else {
            const auto expand4 = [](const std::uint16_t value) {
                return static_cast<std::uint8_t>((value << 4u) | value);
            };
            texture.rgba[destination] = expand4(static_cast<std::uint16_t>((pixel >> 8u) & 0xFu));
            texture.rgba[destination + 1u] =
                expand4(static_cast<std::uint16_t>((pixel >> 4u) & 0xFu));
            texture.rgba[destination + 2u] = expand4(static_cast<std::uint16_t>(pixel & 0xFu));
            texture.rgba[destination + 3u] =
                expand4(static_cast<std::uint16_t>((pixel >> 12u) & 0xFu));
        }
    }
    return texture;
}

void RecordingPvrRenderBackend::render(const PvrTaFrame& frame,
                                       const std::span<const PvrTexture> textures) {
    last_frame_ = frame;
    last_textures_.assign(textures.begin(), textures.end());
    ++submitted_frames_;
}

std::uint64_t RecordingPvrRenderBackend::submitted_frames() const noexcept {
    return submitted_frames_;
}
const PvrTaFrame& RecordingPvrRenderBackend::last_frame() const noexcept {
    return last_frame_;
}
const std::vector<PvrTexture>& RecordingPvrRenderBackend::last_textures() const noexcept {
    return last_textures_;
}

std::shared_ptr<PvrRegisterFile> map_pvr_registers(Memory& memory,
                                                   EventScheduler& scheduler,
                                                   std::function<void()> render_observer,
                                                   const PvrTiming timing,
                                                   std::function<void(bool)> vblank_observer) {
    auto registers = std::make_shared<PvrRegisterFile>(
        scheduler, timing, std::move(render_observer), std::move(vblank_observer));
    auto device = std::make_shared<MmioMemoryDevice>(
        pvr_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            return registers->read(offset);
        },
        [registers](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            registers->write(offset, value);
        });
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + pvr_register_physical_base;
        memory.map_region("dreamcast-pvr-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime
