#include "katana/runtime/runtime_probe.hpp"

#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/pvr.hpp"
#include "katana/runtime/system_asic.hpp"
#include "katana/runtime/system_replay.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

enum class HashDomain : std::uint8_t {
    Cpu = 1u,
    Scheduler = 2u,
    Memory = 3u,
    Persistent = 4u,
    Devices = 5u,
    Replay = 6u,
    GuestState = 7u,
    Combined = 8u,
    DevicePayload = 9u,
};

constexpr std::array<std::uint8_t, 20u> probe_domain = {
    'K', 'A', 'T', 'A', 'N', 'A', '-', 'R', 'U', 'N',
    'T', 'I', 'M', 'E', '-', 'P', 'R', 'O', 'B', 'E',
};

void begin_hash(RuntimeProbeFnv1a64LeV1& hash, const HashDomain domain) noexcept {
    hash.append_bytes(probe_domain);
    hash.append_u8(0u);
    hash.append_u32(runtime_probe_schema_version);
    hash.append_u8(static_cast<std::uint8_t>(domain));
}

void append_optional(RuntimeProbeFnv1a64LeV1& hash,
                     const std::optional<std::uint64_t> value) noexcept {
    hash.append_bool(value.has_value());
    if (value.has_value()) hash.append_u64(*value);
}

const char* status_name(const RuntimeProbeStatus status) noexcept {
    switch (status) {
    case RuntimeProbeStatus::Complete:
        return "complete";
    case RuntimeProbeStatus::Incomplete:
        return "incomplete";
    case RuntimeProbeStatus::Failed:
        return "failed";
    }
    return "unknown";
}

const char* termination_name(const RuntimeProbeTermination termination) noexcept {
    switch (termination) {
    case RuntimeProbeTermination::Unknown:
        return "unknown";
    case RuntimeProbeTermination::Completed:
        return "completed";
    case RuntimeProbeTermination::GuestLifecycle:
        return "guest-lifecycle";
    case RuntimeProbeTermination::BudgetReached:
        return "budget-reached";
    case RuntimeProbeTermination::HostShutdown:
        return "host-shutdown";
    case RuntimeProbeTermination::Failed:
        return "failed";
    case RuntimeProbeTermination::Hang:
        return "hang";
    case RuntimeProbeTermination::GuestException:
        return "guest-exception";
    case RuntimeProbeTermination::DispatchMiss:
        return "dispatch-miss";
    }
    return "unknown";
}

bool known_termination(const RuntimeProbeTermination termination) noexcept {
    switch (termination) {
    case RuntimeProbeTermination::Unknown:
    case RuntimeProbeTermination::Completed:
    case RuntimeProbeTermination::GuestLifecycle:
    case RuntimeProbeTermination::BudgetReached:
    case RuntimeProbeTermination::HostShutdown:
    case RuntimeProbeTermination::Failed:
    case RuntimeProbeTermination::Hang:
    case RuntimeProbeTermination::GuestException:
    case RuntimeProbeTermination::DispatchMiss:
        return true;
    }
    return false;
}

bool fault_termination(const RuntimeProbeTermination termination) noexcept {
    switch (termination) {
    case RuntimeProbeTermination::Failed:
    case RuntimeProbeTermination::Hang:
    case RuntimeProbeTermination::GuestException:
    case RuntimeProbeTermination::DispatchMiss:
        return true;
    case RuntimeProbeTermination::Unknown:
    case RuntimeProbeTermination::Completed:
    case RuntimeProbeTermination::GuestLifecycle:
    case RuntimeProbeTermination::BudgetReached:
    case RuntimeProbeTermination::HostShutdown:
        return false;
    }
    return false;
}

const char* checkpoint_name(const RuntimeProbeCheckpoint checkpoint) noexcept {
    switch (checkpoint) {
    case RuntimeProbeCheckpoint::None:
        return "none";
    case RuntimeProbeCheckpoint::RuntimeStarted:
        return "runtime-started";
    case RuntimeProbeCheckpoint::GuestProgramEntered:
        return "guest-program-entered";
    case RuntimeProbeCheckpoint::FirstGuestFrame:
        return "first-guest-frame";
    case RuntimeProbeCheckpoint::GuestInputInteractive:
        return "guest-input-interactive";
    case RuntimeProbeCheckpoint::ControlledRetailScene:
        return "controlled-retail-scene";
    }
    return "none";
}

bool known_checkpoint(const RuntimeProbeCheckpoint checkpoint) noexcept {
    switch (checkpoint) {
    case RuntimeProbeCheckpoint::None:
    case RuntimeProbeCheckpoint::RuntimeStarted:
    case RuntimeProbeCheckpoint::GuestProgramEntered:
    case RuntimeProbeCheckpoint::FirstGuestFrame:
    case RuntimeProbeCheckpoint::GuestInputInteractive:
    case RuntimeProbeCheckpoint::ControlledRetailScene:
        return true;
    }
    return false;
}

void append_hex64(std::ostringstream& output, const std::uint64_t value) {
    output << '"' << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
           << value << std::dec << '"';
}

std::uint64_t checked_memory_byte_count(
    const std::span<const RuntimeProbeMemoryRange> ranges) {
    std::uint64_t total = 0u;
    for (const auto& range : ranges) {
        const auto size = static_cast<std::uint64_t>(range.bytes.size());
        if (size > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("Runtime-Probe-Speichermenge laeuft ueber.");
        total += size;
    }
    return total;
}

std::uint64_t checked_device_field_count(
    const std::span<const RuntimeProbeDeviceSnapshot> devices) {
    std::uint64_t total = 0u;
    for (const auto& device : devices) {
        const auto fields = static_cast<std::uint64_t>(device.fields.size());
        if (fields > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("Runtime-Probe-Geraetefeldmenge laeuft ueber.");
        total += fields;
    }
    return total;
}

std::vector<const RuntimeProbeMemoryRange*>
canonical_memory_ranges(const std::span<const RuntimeProbeMemoryRange> ranges,
                        const bool persistent) {
    constexpr std::uint64_t address_space_size = 0x100000000ull;
    std::vector<const RuntimeProbeMemoryRange*> ordered;
    ordered.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.region == RuntimeProbeMemoryRegion::Unknown || range.bytes.empty())
            throw std::invalid_argument(
                "Runtime-Probe-Speicherbereiche brauchen Region und Bytes.");
        const bool persistent_region = range.region == RuntimeProbeMemoryRegion::Flash ||
                                       range.region == RuntimeProbeMemoryRegion::Vmu;
        if (persistent != persistent_region)
            throw std::invalid_argument(
                "Runtime-Probe trennt fluechtige und persistente Speicherregionen.");
        const auto size = static_cast<std::uint64_t>(range.bytes.size());
        if (size > address_space_size - static_cast<std::uint64_t>(range.offset))
            throw std::invalid_argument(
                "Runtime-Probe-Speicherbereich ueberschreitet seinen 32-Bit-Adressraum.");
        ordered.push_back(&range);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return std::tuple{static_cast<std::uint32_t>(left->region),
                          left->offset,
                          left->bytes.size()} <
               std::tuple{static_cast<std::uint32_t>(right->region),
                          right->offset,
                          right->bytes.size()};
    });
    for (std::size_t index = 1u; index < ordered.size(); ++index) {
        const auto& previous = *ordered[index - 1u];
        const auto& current = *ordered[index];
        if (previous.region != current.region) continue;
        const auto previous_end =
            static_cast<std::uint64_t>(previous.offset) + previous.bytes.size();
        if (previous_end > current.offset)
            throw std::invalid_argument(
                "Runtime-Probe-Speicherbereiche derselben Region duerfen nicht ueberlappen.");
    }
    return ordered;
}

std::vector<const RuntimeProbeDeviceSnapshot*>
canonical_devices(const std::span<const RuntimeProbeDeviceSnapshot> devices) {
    std::vector<const RuntimeProbeDeviceSnapshot*> ordered;
    ordered.reserve(devices.size());
    for (const auto& device : devices) {
        if (device.kind == RuntimeProbeDeviceKind::Unknown)
            throw std::invalid_argument(
                "Runtime-Probe-Geraetesnapshot braucht eine stabile Geraeteklasse.");
        ordered.push_back(&device);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return std::tuple{static_cast<std::uint32_t>(left->kind), left->instance} <
               std::tuple{static_cast<std::uint32_t>(right->kind), right->instance};
    });
    for (std::size_t index = 1u; index < ordered.size(); ++index) {
        const auto& previous = *ordered[index - 1u];
        const auto& current = *ordered[index];
        if (previous.kind == current.kind && previous.instance == current.instance)
            throw std::invalid_argument(
                "Runtime-Probe-Geraeteklasse und Instanz muessen eindeutig sein.");
    }
    return ordered;
}

std::vector<const RuntimeProbeDeviceField*>
canonical_device_fields(const RuntimeProbeDeviceSnapshot& device) {
    std::vector<const RuntimeProbeDeviceField*> ordered;
    ordered.reserve(device.fields.size());
    for (const auto& field : device.fields) {
        if (field.id == 0u)
            throw std::invalid_argument(
                "Runtime-Probe-Geraetefelder brauchen eine stabile Feld-ID.");
        ordered.push_back(&field);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    for (std::size_t index = 1u; index < ordered.size(); ++index) {
        if (ordered[index - 1u]->id == ordered[index]->id)
            throw std::invalid_argument(
                "Runtime-Probe-Geraetefeld-IDs muessen pro Instanz eindeutig sein.");
    }
    return ordered;
}

void validate_scheduler_profile(const RuntimeProbeSchedulerSnapshot& scheduler) {
    if (scheduler.advance_in_progress ||
        scheduler.pending_event_count != scheduler.pending_events.size() ||
        scheduler.next_event_cycle.has_value() != !scheduler.pending_events.empty() ||
        (scheduler.next_event_cycle &&
         *scheduler.next_event_cycle != scheduler.pending_events.front().guest_cycle) ||
        scheduler.next_event_id == 0u || scheduler.next_reset_observer_id == 0u ||
        !scheduler.guest_cycle_budget.has_value() || *scheduler.guest_cycle_budget == 0u ||
        scheduler.current_cycle > *scheduler.guest_cycle_budget) {
        throw std::invalid_argument(
            "Deterministic-v1 braucht einen konsistenten terminalen Schedulerzustand.");
    }
    std::optional<std::pair<std::uint64_t, SchedulerEventId>> previous_event;
    for (const auto& event : scheduler.pending_events) {
        const std::pair key{event.guest_cycle, event.event_id};
        if (event.event_id == 0u || event.event_id >= scheduler.next_event_id ||
            event.kind == SchedulerEventKind::Unknown ||
            (previous_event && *previous_event >= key)) {
            throw std::invalid_argument(
                "Deterministic-v1 braucht eine kanonische typisierte Schedulerqueue.");
        }
        previous_event = key;
    }
    SchedulerResetObserverId previous_observer = 0u;
    for (const auto observer_id : scheduler.reset_observer_ids) {
        if (observer_id == 0u || observer_id >= scheduler.next_reset_observer_id ||
            observer_id <= previous_observer) {
            throw std::invalid_argument(
                "Deterministic-v1 braucht kanonische Scheduler-Resetbeobachter.");
        }
        previous_observer = observer_id;
    }
}

void require_exact_memory_profile(
    const std::span<const RuntimeProbeMemoryRange> ranges,
    const std::span<const std::pair<RuntimeProbeMemoryRegion, std::size_t>> expected,
    const bool persistent) {
    const auto ordered = canonical_memory_ranges(ranges, persistent);
    if (ordered.size() != expected.size()) {
        throw std::invalid_argument(
            "Deterministic-v1 besitzt nicht die vollstaendige Speichermenge.");
    }
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        if (ordered[index]->region != expected[index].first ||
            ordered[index]->offset != 0u ||
            ordered[index]->bytes.size() != expected[index].second) {
            throw std::invalid_argument(
                "Deterministic-v1 besitzt eine falsche Speicherregion oder Groesse.");
        }
    }
}

class DeviceFieldBuilder final {
  public:
    DeviceFieldBuilder(const RuntimeProbeDeviceKind kind, const std::uint32_t instance)
        : kind_(kind), instance_(instance) {
        scalar(runtime_probe_device_schema_version);
    }

    void scalar(const std::uint64_t value) {
        fields_.push_back({next_field_id_++, value});
    }

    template <typename Value>
    void optional_scalar(const std::optional<Value>& value) {
        scalar(value.has_value());
        scalar(value.has_value() ? static_cast<std::uint64_t>(*value) : 0u);
    }

    template <typename Serialize>
    void payload(const std::uint64_t element_count, Serialize&& serialize) {
        const auto payload_tag = next_field_id_;
        scalar(element_count);
        RuntimeProbeFnv1a64LeV1 hash;
        begin_hash(hash, HashDomain::DevicePayload);
        hash.append_u32(static_cast<std::uint32_t>(kind_));
        hash.append_u32(instance_);
        hash.append_u32(payload_tag);
        hash.append_u64(element_count);
        std::invoke(std::forward<Serialize>(serialize), hash);
        scalar(hash.value());
    }

    template <typename Value, std::size_t Size>
    void byte_payload(const std::array<Value, Size>& values) {
        static_assert(sizeof(Value) == sizeof(std::uint8_t));
        payload(Size, [&values](RuntimeProbeFnv1a64LeV1& hash) {
            for (const auto value : values)
                hash.append_u8(static_cast<std::uint8_t>(value));
        });
    }

    void byte_payload(const std::span<const std::uint8_t> values) {
        payload(static_cast<std::uint64_t>(values.size()),
                [values](RuntimeProbeFnv1a64LeV1& hash) { hash.append_bytes(values); });
    }

    [[nodiscard]] RuntimeProbeDeviceSnapshot finish() && {
        return {kind_, instance_, std::move(fields_)};
    }

  private:
    RuntimeProbeDeviceKind kind_ = RuntimeProbeDeviceKind::Unknown;
    std::uint32_t instance_ = 0u;
    std::uint32_t next_field_id_ = 1u;
    std::vector<RuntimeProbeDeviceField> fields_;
};

void append_string(RuntimeProbeFnv1a64LeV1& hash, const std::string_view value) noexcept {
    hash.append_u64(static_cast<std::uint64_t>(value.size()));
    for (const auto character : value)
        hash.append_u8(static_cast<std::uint8_t>(
            static_cast<unsigned char>(character)));
}

void append_vertex(RuntimeProbeFnv1a64LeV1& hash, const PvrVertex& vertex) noexcept {
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.x));
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.y));
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.z));
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.u));
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.v));
    hash.append_u32(vertex.argb);
    hash.append_u32(vertex.oargb);
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.volume_u));
    hash.append_u32(std::bit_cast<std::uint32_t>(vertex.volume_v));
    hash.append_u32(vertex.volume_argb);
    hash.append_u32(vertex.volume_oargb);
}

void append_material(RuntimeProbeFnv1a64LeV1& hash,
                     const PvrMaterial& material,
                     std::vector<const PvrMaterial*>& path) {
    hash.append_bool(material.gouraud);
    hash.append_bool(material.textured);
    hash.append_bool(material.texture_twiddled);
    hash.append_bool(material.texture_vq);
    hash.append_bool(material.texture_mipmapped);
    hash.append_bool(material.texture_x32_stride);
    hash.append_bool(material.texture_alpha_disabled);
    hash.append_bool(material.vertex_alpha_enabled);
    hash.append_bool(material.offset_color_enabled);
    hash.append_bool(material.color_clamp_enabled);
    hash.append_bool(material.texture_supersampling);
    hash.append_bool(material.shadow_enabled);
    hash.append_bool(material.blend_destination_accumulation);
    hash.append_bool(material.blend_source_accumulation);
    hash.append_bool(material.clamp_u);
    hash.append_bool(material.clamp_v);
    hash.append_bool(material.flip_u);
    hash.append_bool(material.flip_v);
    hash.append_bool(material.depth_write);
    hash.append_u8(material.depth_compare);
    hash.append_u8(material.culling);
    hash.append_u8(material.texture_format);
    hash.append_u8(material.texture_shading);
    hash.append_u8(material.texture_filter);
    hash.append_u8(material.texture_mipmap_bias);
    hash.append_u8(material.fog_mode);
    hash.append_u8(material.source_blend);
    hash.append_u8(material.destination_blend);
    hash.append_u8(material.palette_bank);
    hash.append_u8(material.user_clip_mode);
    hash.append_u16(material.user_clip_start_x);
    hash.append_u16(material.user_clip_start_y);
    hash.append_u16(material.user_clip_end_x);
    hash.append_u16(material.user_clip_end_y);
    hash.append_u32(material.texture_width);
    hash.append_u32(material.texture_height);
    hash.append_u32(material.texture_base);
    hash.append_u32(material.texture_stride_width);
    hash.append_bool(static_cast<bool>(material.volume_material));
    if (!material.volume_material) return;

    const auto found = std::find(path.begin(), path.end(), material.volume_material.get());
    hash.append_bool(found != path.end());
    if (found != path.end()) {
        hash.append_u64(static_cast<std::uint64_t>(std::distance(path.begin(), found)));
        return;
    }
    path.push_back(material.volume_material.get());
    append_material(hash, *material.volume_material, path);
    path.pop_back();
}

void append_material(RuntimeProbeFnv1a64LeV1& hash, const PvrMaterial& material) {
    std::vector<const PvrMaterial*> path = {&material};
    append_material(hash, material, path);
}

void append_material_fields(DeviceFieldBuilder& fields, const PvrMaterial& material) {
    fields.scalar(material.gouraud);
    fields.scalar(material.textured);
    fields.scalar(material.texture_twiddled);
    fields.scalar(material.texture_vq);
    fields.scalar(material.texture_mipmapped);
    fields.scalar(material.texture_x32_stride);
    fields.scalar(material.texture_alpha_disabled);
    fields.scalar(material.vertex_alpha_enabled);
    fields.scalar(material.offset_color_enabled);
    fields.scalar(material.color_clamp_enabled);
    fields.scalar(material.texture_supersampling);
    fields.scalar(material.shadow_enabled);
    fields.scalar(material.blend_destination_accumulation);
    fields.scalar(material.blend_source_accumulation);
    fields.scalar(material.clamp_u);
    fields.scalar(material.clamp_v);
    fields.scalar(material.flip_u);
    fields.scalar(material.flip_v);
    fields.scalar(material.depth_write);
    fields.scalar(material.depth_compare);
    fields.scalar(material.culling);
    fields.scalar(material.texture_format);
    fields.scalar(material.texture_shading);
    fields.scalar(material.texture_filter);
    fields.scalar(material.texture_mipmap_bias);
    fields.scalar(material.fog_mode);
    fields.scalar(material.source_blend);
    fields.scalar(material.destination_blend);
    fields.scalar(material.palette_bank);
    fields.scalar(material.user_clip_mode);
    fields.scalar(material.user_clip_start_x);
    fields.scalar(material.user_clip_start_y);
    fields.scalar(material.user_clip_end_x);
    fields.scalar(material.user_clip_end_y);
    fields.scalar(material.texture_width);
    fields.scalar(material.texture_height);
    fields.scalar(material.texture_base);
    fields.scalar(material.texture_stride_width);
    fields.scalar(static_cast<bool>(material.volume_material));
    fields.payload(material.volume_material ? 1u : 0u,
                   [&material](RuntimeProbeFnv1a64LeV1& hash) {
                       if (material.volume_material)
                           append_material(hash, *material.volume_material);
                   });
}

void append_primitive(RuntimeProbeFnv1a64LeV1& hash, const PvrPrimitive& primitive) {
    hash.append_u8(static_cast<std::uint8_t>(primitive.list));
    hash.append_u64(static_cast<std::uint64_t>(primitive.vertices.size()));
    for (const auto& vertex : primitive.vertices)
        append_vertex(hash, vertex);
    append_material(hash, primitive.material);
}

void append_modifier_volume(RuntimeProbeFnv1a64LeV1& hash,
                            const PvrModifierVolume& volume) {
    hash.append_u8(static_cast<std::uint8_t>(volume.list));
    hash.append_u64(static_cast<std::uint64_t>(volume.triangles.size()));
    for (const auto& triangle : volume.triangles)
        for (const auto& vertex : triangle)
            append_vertex(hash, vertex);
    hash.append_u8(volume.depth_mode);
    hash.append_u8(volume.culling);
    hash.append_u8(volume.user_clip_mode);
    hash.append_u16(volume.user_clip_start_x);
    hash.append_u16(volume.user_clip_start_y);
    hash.append_u16(volume.user_clip_end_x);
    hash.append_u16(volume.user_clip_end_y);
    hash.append_bool(volume.volume_last);
}

void append_response(RuntimeProbeFnv1a64LeV1& hash, const GdRomResponse& response) noexcept {
    hash.append_u8(static_cast<std::uint8_t>(response.status));
    hash.append_u32(response.transferred_sectors);
    hash.append_u64(static_cast<std::uint64_t>(response.data.size()));
    hash.append_bytes(response.data);
}

void append_render_evidence(RuntimeProbeFnv1a64LeV1& hash,
                            const PvrRenderGenerationEvidence& evidence) noexcept {
    hash.append_u64(evidence.generation);
    hash.append_u32(evidence.write_base);
    hash.append_u32(evidence.stride_bytes);
    hash.append_u32(evidence.width);
    hash.append_u32(evidence.height);
    hash.append_u8(evidence.pixel_bytes);
    hash.append_bool(evidence.render_to_texture);
    hash.append_u64(evidence.pixel_writes);
    hash.append_u64(evidence.changed_pixels);
    hash.append_u64(static_cast<std::uint64_t>(evidence.validation_cursor));
    hash.append_u64(static_cast<std::uint64_t>(evidence.changed_pixel_values.size()));
    for (const auto& pixel : evidence.changed_pixel_values) {
        hash.append_u32(pixel.offset);
        hash.append_u32(pixel.packed_value);
        hash.append_u32(pixel.changed_byte_mask);
    }
}

void append_scanout(RuntimeProbeFnv1a64LeV1& hash,
                    const PvrScanoutDescriptor& scanout) noexcept {
    hash.append_u32(scanout.width);
    hash.append_u32(scanout.height);
    hash.append_u32(scanout.source_width);
    hash.append_u32(scanout.source_height);
    hash.append_u32(scanout.stride_bytes);
    hash.append_u64(static_cast<std::uint64_t>(scanout.base_offset));
    hash.append_u64(static_cast<std::uint64_t>(scanout.second_base_offset));
    hash.append_u8(static_cast<std::uint8_t>(scanout.format));
    hash.append_u8(scanout.concat);
    hash.append_bool(scanout.line_double);
    hash.append_bool(scanout.interlaced);
    hash.append_bool(scanout.weave_fields);
    hash.append_bool(scanout.horizontal_scale);
    hash.append_u16(scanout.vertical_scale_factor);
    hash.append_bool(scanout.video_blank);
    for (const auto component : scanout.border_rgba)
        hash.append_u8(component);
}

void append_guest_frame_proof(RuntimeProbeFnv1a64LeV1& hash,
                              const PvrGuestFrameProof& proof) noexcept {
    hash.append_u64(proof.render_generation);
    hash.append_u64(proof.changed_pixels);
    hash.append_u32(proof.scanout_field);
    append_scanout(hash, proof.scanout);
    hash.append_u32(proof.frame.width);
    hash.append_u32(proof.frame.height);
    hash.append_u64(static_cast<std::uint64_t>(proof.frame.rgba.size()));
    hash.append_bytes(proof.frame.rgba);
    hash.append_u8(static_cast<std::uint8_t>(proof.source));
}

} // namespace

RuntimeProbeBudgetReached::RuntimeProbeBudgetReached(
    const std::optional<std::uint64_t> final_guest_cycle)
    : std::runtime_error("runtime-probe-budget-reached"),
      final_guest_cycle_(final_guest_cycle) {}

std::optional<std::uint64_t>
RuntimeProbeBudgetReached::final_guest_cycle() const noexcept {
    return final_guest_cycle_;
}

bool RuntimeProbeObservationState::observe_checkpoint(
    const RuntimeProbeCheckpoint checkpoint,
    const RuntimeProbeCpuSnapshot& cpu) noexcept {
    if (first_fault_.has_value() || checkpoint == RuntimeProbeCheckpoint::None ||
        !known_checkpoint(checkpoint)) {
        return false;
    }
    if (last_stable_checkpoint_.has_value()) {
        if (static_cast<std::uint8_t>(checkpoint) <=
                static_cast<std::uint8_t>(
                    last_stable_checkpoint_->checkpoint) ||
            cpu.retired_guest_instructions <
                last_stable_checkpoint_->cpu.retired_guest_instructions) {
            return false;
        }
    }
    last_stable_checkpoint_ = RuntimeProbeCheckpointObservation{checkpoint, cpu};
    return true;
}

bool RuntimeProbeObservationState::latch_fault(
    const RuntimeProbeTermination termination,
    const RuntimeProbeCpuSnapshot& cpu) noexcept {
    if (first_fault_.has_value() || !fault_termination(termination)) return false;
    first_fault_ = RuntimeProbeFaultObservation{termination, cpu};
    return true;
}

const std::optional<RuntimeProbeCheckpointObservation>&
RuntimeProbeObservationState::last_stable_checkpoint() const noexcept {
    return last_stable_checkpoint_;
}

const std::optional<RuntimeProbeFaultObservation>&
RuntimeProbeObservationState::first_fault() const noexcept {
    return first_fault_;
}

RuntimeProbeFaultEnvelope RuntimeProbeObservationState::fault_envelope(
    const RuntimeProbeTermination termination) const noexcept {
    RuntimeProbeFaultEnvelope envelope;
    envelope.termination =
        first_fault_.has_value() ? first_fault_->termination : termination;
    envelope.first_fault = first_fault_;
    envelope.last_stable_checkpoint = last_stable_checkpoint_;
    return envelope;
}

void RuntimeProbeFnv1a64LeV1::append_u8(const std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= runtime_probe_fnv1a64_prime;
}

void RuntimeProbeFnv1a64LeV1::append_u16(const std::uint16_t value) noexcept {
    for (unsigned shift = 0u; shift < 16u; shift += 8u)
        append_u8(static_cast<std::uint8_t>(value >> shift));
}

void RuntimeProbeFnv1a64LeV1::append_u32(const std::uint32_t value) noexcept {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        append_u8(static_cast<std::uint8_t>(value >> shift));
}

void RuntimeProbeFnv1a64LeV1::append_u64(const std::uint64_t value) noexcept {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        append_u8(static_cast<std::uint8_t>(value >> shift));
}

void RuntimeProbeFnv1a64LeV1::append_bool(const bool value) noexcept {
    append_u8(value ? 1u : 0u);
}

void RuntimeProbeFnv1a64LeV1::append_bytes(
    const std::span<const std::uint8_t> bytes) noexcept {
    for (const auto value : bytes)
        append_u8(value);
}

std::uint64_t RuntimeProbeFnv1a64LeV1::value() const noexcept {
    return value_;
}

RuntimeProbeCpuSnapshot capture_runtime_probe_cpu(const CpuState& cpu) noexcept {
    RuntimeProbeCpuSnapshot snapshot;
    snapshot.r = cpu.r;
    snapshot.r_bank = cpu.r_bank;
    snapshot.fr = cpu.fr;
    snapshot.xf = cpu.xf;
    snapshot.pc = cpu.pc;
    snapshot.pr = cpu.pr;
    snapshot.gbr = cpu.gbr;
    snapshot.vbr = cpu.vbr;
    snapshot.ssr = cpu.ssr;
    snapshot.spc = cpu.spc;
    snapshot.sgr = cpu.sgr;
    snapshot.dbr = cpu.dbr;
    snapshot.tra = cpu.tra;
    snapshot.tea = cpu.tea;
    snapshot.expevt = cpu.expevt;
    snapshot.intevt = cpu.intevt;
    snapshot.pteh = cpu.pteh;
    snapshot.ptel = cpu.ptel;
    snapshot.ptea = cpu.ptea;
    snapshot.ttb = cpu.ttb;
    snapshot.mmucr = cpu.mmucr;
    for (std::size_t index = 0u; index < snapshot.utlb.size(); ++index) {
        snapshot.utlb[index] = {cpu.utlb[index].pteh,
                                cpu.utlb[index].ptel,
                                cpu.utlb[index].ptea};
    }
    snapshot.tlb_load_count = cpu.tlb_load_count;
    snapshot.mach = cpu.mach;
    snapshot.macl = cpu.macl;
    snapshot.fpul = cpu.fpul;
    snapshot.fpscr = cpu.read_fpscr();
    snapshot.sr = cpu.read_sr();
    snapshot.t = cpu.t;
    snapshot.s = cpu.s;
    snapshot.q = cpu.q;
    snapshot.m = cpu.m;
    snapshot.trap_pending = cpu.trap_pending;
    snapshot.last_exception_cause = cpu.last_exception_cause;
    snapshot.exception_in_delay_slot = cpu.exception_in_delay_slot;
    snapshot.sleeping = cpu.sleeping;
    snapshot.last_prefetch_address = cpu.last_prefetch_address;
    snapshot.prefetch_count = cpu.prefetch_count;
    snapshot.retired_guest_instructions = cpu.retired_guest_instructions;
    snapshot.last_prefetch_was_store_queue = cpu.last_prefetch_was_store_queue;
    return snapshot;
}

RuntimeProbeSchedulerSnapshot
capture_runtime_probe_scheduler(const EventScheduler& scheduler) {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    const auto scheduler_snapshot = scheduler.snapshot();
    RuntimeProbeSchedulerSnapshot snapshot;
    snapshot.current_cycle = scheduler_snapshot.current_cycle;
    if (!scheduler_snapshot.pending_events.empty())
        snapshot.next_event_cycle = scheduler_snapshot.pending_events.front().guest_cycle;
    snapshot.pending_event_count =
        static_cast<std::uint64_t>(scheduler_snapshot.pending_events.size());
    snapshot.next_event_id = scheduler_snapshot.next_event_id;
    snapshot.next_reset_observer_id = scheduler_snapshot.next_reset_observer_id;
    snapshot.processed_event_count = scheduler_snapshot.processed_event_count;
    snapshot.reset_generation = scheduler_snapshot.reset_generation;
    snapshot.guest_cycle_budget = scheduler_snapshot.guest_cycle_budget;
    snapshot.advance_in_progress = scheduler_snapshot.advance_in_progress;
    snapshot.pending_events = scheduler_snapshot.pending_events;
    snapshot.reset_observer_ids = scheduler_snapshot.reset_observer_ids;
    return snapshot;
}

RuntimeProbeReplaySnapshot
capture_runtime_probe_replay(const SystemReplayLog& replay) {
    RuntimeProbeReplaySnapshot snapshot;
    snapshot.replay_schema_version = system_replay_schema_version;
    snapshot.storage_mode =
        static_cast<std::uint32_t>(replay.config().storage_mode);
    snapshot.retention_capacity =
        static_cast<std::uint64_t>(replay.config().capacity);
    snapshot.event_count = replay.event_count();
    snapshot.retained_event_count =
        static_cast<std::uint64_t>(replay.events().size());
    snapshot.summarized_event_count = replay.summarized_event_count();
    snapshot.dropped_events = replay.dropped_events();
    snapshot.event_hash = replay.event_hash();
    snapshot.ordering_digest = replay.ordering_digest();
    snapshot.enabled_coverage = replay.enabled_coverage();
    snapshot.observed_coverage = replay.observed_coverage();
    snapshot.required_coverage = replay.required_coverage();
    snapshot.event_counts = replay.event_counts();
    snapshot.coverage_complete = replay.coverage_complete();
    snapshot.complete = snapshot.dropped_events == 0u && snapshot.coverage_complete;
    snapshot.exact_event_stream = replay.exact_event_stream_available();
    snapshot.sealed = replay.sealed();
    if (snapshot.sealed)
        snapshot.final_guest_state_hash = replay.final_guest_state_hash();
    return snapshot;
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrRegisterSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Pvr, instance);
    fields.payload(snapshot.registers.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto value : snapshot.registers)
                           hash.append_u32(value);
                   });
    fields.scalar(snapshot.framebuffer_read_control);
    fields.scalar(snapshot.framebuffer_read_size);
    fields.scalar(snapshot.framebuffer_read_sof1);
    fields.scalar(snapshot.framebuffer_read_sof2);
    fields.scalar(snapshot.framebuffer_write_control);
    fields.scalar(snapshot.framebuffer_write_sof1);
    fields.scalar(snapshot.framebuffer_write_sof2);
    fields.scalar(snapshot.video_control);
    fields.scalar(snapshot.render_requests);
    fields.scalar(snapshot.render_completions);
    fields.scalar(snapshot.render_failures);
    fields.scalar(snapshot.vblank_in);
    fields.scalar(snapshot.vblank_out);
    fields.scalar(snapshot.hblank);
    fields.scalar(snapshot.resets);
    fields.payload(snapshot.render_event_ids.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto event : snapshot.render_event_ids)
                           hash.append_u64(event);
                   });
    fields.optional_scalar(snapshot.vblank_in_event);
    fields.optional_scalar(snapshot.vblank_out_event);
    fields.optional_scalar(snapshot.hblank_event);
    fields.scalar(snapshot.scan_frame_cycles);
    fields.scalar(snapshot.scan_epoch_cycle);
    fields.scalar(snapshot.timing.render_latency);
    fields.scalar(snapshot.timing.guest_clock_hz);
    fields.scalar(snapshot.timing.pixel_clock_hz);
    fields.scalar(snapshot.in_vblank);
    fields.scalar(snapshot.field);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastSystemBusSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::SystemBus, instance);
    fields.payload(snapshot.registers.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto value : snapshot.registers)
                           hash.append_u32(value);
                   });
    fields.scalar(snapshot.channel2_destination);
    fields.scalar(snapshot.channel2_length);
    fields.scalar(snapshot.channel2_start);
    fields.scalar(snapshot.sort_start_address);
    fields.scalar(snapshot.sort_base_address);
    fields.scalar(snapshot.sort_link_width);
    fields.scalar(snapshot.sort_address_shift);
    fields.scalar(snapshot.sort_start);
    fields.scalar(snapshot.dbreq_mask);
    fields.scalar(snapshot.bavl_wait_count);
    fields.scalar(snapshot.channel2_priority);
    fields.scalar(snapshot.channel2_max_burst);
    fields.scalar(snapshot.sort_divider);
    fields.scalar(snapshot.ta_fifo_remaining);
    fields.scalar(snapshot.texture_memory_mode0);
    fields.scalar(snapshot.texture_memory_mode1);
    fields.scalar(snapshot.fifo_status);
    fields.scalar(snapshot.revision);
    fields.scalar(snapshot.root_bus_split);
    fields.scalar(snapshot.system_reset_requests);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastSystemAsicSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::SystemAsic, instance);
    for (const auto value : snapshot.pending)
        fields.scalar(value);
    for (const auto& level : snapshot.masks)
        for (const auto value : level)
            fields.scalar(value);
    for (const auto& trigger : snapshot.dma_trigger_masks)
        for (const auto value : trigger)
            fields.scalar(value);
    fields.payload(snapshot.events.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& event : snapshot.events) {
                           hash.append_u64(event.guest_cycle);
                           hash.append_u64(event.sequence);
                           hash.append_u16(static_cast<std::uint16_t>(event.event));
                       }
                   });
    fields.scalar(snapshot.next_sequence);
    fields.scalar(snapshot.last_guest_cycle);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaRegisterSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Aica, instance);
    fields.byte_payload(snapshot.registers);
    fields.payload(snapshot.channels.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& channel : snapshot.channels) {
                           hash.append_u64(channel.phase);
                           hash.append_u32(channel.adpcm_position);
                           hash.append_u32(static_cast<std::uint32_t>(
                               channel.adpcm_predictor));
                           hash.append_u32(static_cast<std::uint32_t>(
                               channel.adpcm_step));
                           hash.append_bool(channel.active);
                       }
                   });
    fields.scalar(snapshot.writes);
    fields.scalar(snapshot.rendered_buffers);
    fields.scalar(snapshot.rendered_frames);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaRtcSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::AicaRtc, instance);
    fields.scalar(snapshot.scheduler_cycle);
    fields.scalar(snapshot.guest_clock_hz);
    fields.scalar(snapshot.base_cycle);
    fields.scalar(snapshot.initial_seconds);
    fields.scalar(snapshot.base_seconds);
    fields.scalar(snapshot.counter);
    fields.scalar(snapshot.write_latch);
    fields.scalar(snapshot.write_enabled);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const AicaExecutionController::Snapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::AicaExecution, instance);
    fields.scalar(static_cast<std::uint64_t>(snapshot.mode));
    fields.scalar(snapshot.arm7_reset_asserted);
    for (const auto& timer : snapshot.timers) {
        fields.scalar(timer.remainder);
        fields.scalar(timer.divisor);
        fields.scalar(timer.counter);
        fields.scalar(timer.enabled);
    }
    fields.scalar(snapshot.interrupts.enabled);
    fields.scalar(snapshot.interrupts.pending);
    fields.scalar(snapshot.interrupts.asserted);
    fields.optional_scalar(snapshot.tick_event);
    fields.scalar(snapshot.guest_cycles_per_tick);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastGdRomSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::GdRom, instance);
    fields.scalar(snapshot.reader.scheduler_cycle);
    fields.scalar(snapshot.reader.timing.command_latency);
    fields.scalar(snapshot.reader.timing.cycles_per_sector);
    fields.scalar(snapshot.reader.next_request_id);
    fields.payload(snapshot.reader.pending.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& pending : snapshot.reader.pending) {
                           hash.append_u64(pending.request_id);
                           hash.append_u64(pending.ready_cycle);
                           hash.append_u8(
                               static_cast<std::uint8_t>(pending.request.command));
                           hash.append_u32(pending.request.lba);
                           hash.append_u32(pending.request.sector_count);
                           hash.append_u64(pending.event_id);
                       }
                   });
    fields.payload(snapshot.reader.completed.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& completion : snapshot.reader.completed) {
                           hash.append_u64(completion.request_id);
                           hash.append_u64(completion.ready_cycle);
                           append_response(hash, completion.response);
                       }
                   });
    fields.byte_payload(snapshot.packet);
    fields.byte_payload(snapshot.data);
    fields.scalar(static_cast<std::uint64_t>(snapshot.data_cursor));
    fields.scalar(snapshot.taskfile_phase_remaining);
    fields.scalar(snapshot.taskfile_host_byte_limit);
    fields.scalar(snapshot.taskfile_phase);
    fields.scalar(snapshot.drive_owner);
    fields.scalar(snapshot.command_irq_asserted);
    fields.scalar(snapshot.command_irq_reassert_pending);
    fields.scalar(snapshot.taskfile_command_failed);
    fields.scalar(snapshot.clear_sense_after_data);
    fields.scalar(snapshot.set_mode_offset);
    fields.byte_payload(snapshot.drive_mode);
    fields.scalar(snapshot.sense_key);
    fields.scalar(snapshot.sense_asc);
    fields.scalar(snapshot.sense_ascq);
    fields.scalar(snapshot.status);
    fields.scalar(snapshot.error);
    fields.scalar(snapshot.interrupt_reason);
    fields.scalar(snapshot.features);
    fields.scalar(snapshot.sector_count_register);
    fields.scalar(snapshot.sector_number);
    fields.scalar(snapshot.drive_select);
    fields.scalar(snapshot.byte_count);
    fields.scalar(snapshot.current_fad);
    fields.scalar(snapshot.expecting_packet);
    fields.payload(snapshot.bios_requests.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& request : snapshot.bios_requests) {
                           hash.append_u32(request.id);
                           hash.append_u32(request.command);
                           for (const auto value : request.parameters)
                               hash.append_u32(value);
                           hash.append_u64(request.async_id);
                           hash.append_u32(request.destination);
                           hash.append_u8(
                               static_cast<std::uint8_t>(request.write_source));
                           hash.append_u8(static_cast<std::uint8_t>(request.state));
                           for (const auto value : request.status)
                               hash.append_u32(value);
                           append_response(hash, request.response);
                           hash.append_bool(request.streaming_dma);
                           hash.append_u32(request.stream_lba);
                           hash.append_u32(request.stream_sector_count);
                           hash.append_u64(request.stream_total_bytes);
                           hash.append_u64(request.stream_consumed_bytes);
                           hash.append_u32(request.cached_stream_sector);
                           hash.append_u64(
                               static_cast<std::uint64_t>(
                                   request.stream_sector_cache.size()));
                           hash.append_bytes(request.stream_sector_cache);
                           hash.append_u8(
                               static_cast<std::uint8_t>(request.transfer_kind));
                           hash.append_u32(request.transfer_destination);
                           hash.append_u32(request.transfer_size);
                           hash.append_u32(request.transfer_transferred);
                           hash.append_bool(request.transfer_active);
                       }
                   });
    fields.scalar(snapshot.next_bios_request);
    fields.scalar(snapshot.last_bios_request.id);
    fields.scalar(snapshot.last_bios_request.command);
    fields.scalar(static_cast<std::uint64_t>(snapshot.last_bios_request.state));
    for (const auto value : snapshot.last_bios_request.status)
        fields.scalar(value);
    fields.payload(snapshot.bios_call_events.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& event : snapshot.bios_call_events) {
                           hash.append_u64(event.sequence);
                           hash.append_u64(event.guest_cycle);
                           hash.append_u32(event.callsite);
                           hash.append_u32(event.return_address);
                           hash.append_u32(event.selector);
                           hash.append_u32(event.super_selector);
                           for (const auto value : event.arguments)
                               hash.append_u32(value);
                           hash.append_u32(event.request_id);
                           hash.append_u8(
                               static_cast<std::uint8_t>(event.state_before));
                           hash.append_u8(
                               static_cast<std::uint8_t>(event.state_after));
                           hash.append_u32(event.result);
                           for (const auto value : event.status)
                               hash.append_u32(value);
                       }
                   });
    fields.scalar(snapshot.next_bios_call_sequence);
    fields.scalar(snapshot.dropped_bios_call_events);
    fields.scalar(snapshot.completed_commands);
    fields.scalar(snapshot.completed_dma);
    for (const auto value : snapshot.sector_mode)
        fields.scalar(value);
    fields.scalar(snapshot.dma_callback);
    fields.scalar(snapshot.dma_callback_argument);
    fields.scalar(snapshot.pio_callback);
    fields.scalar(snapshot.pio_callback_argument);
    fields.scalar(snapshot.dma_completion_pending);
    fields.scalar(snapshot.pio_completion_pending);
    fields.scalar(snapshot.dma_completion_request);
    fields.scalar(snapshot.pio_completion_request);
    fields.payload(snapshot.pending_guest_callbacks.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& callback : snapshot.pending_guest_callbacks) {
                           hash.append_u8(
                               static_cast<std::uint8_t>(callback.kind));
                           hash.append_u32(callback.address);
                           hash.append_u32(callback.argument);
                           hash.append_u32(callback.request_id);
                       }
                   });
    fields.optional_scalar(snapshot.packet_event);
    fields.scalar(snapshot.g1_bus_bound);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const MapleBusSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Maple, instance);
    for (const auto attached : snapshot.attached)
        fields.scalar(attached);
    fields.payload(snapshot.history.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& record : snapshot.history) {
                           hash.append_u64(record.sequence);
                           hash.append_u8(record.port);
                           hash.append_u8(record.unit);
                           hash.append_u8(
                               static_cast<std::uint8_t>(record.command));
                           hash.append_u8(
                               static_cast<std::uint8_t>(record.response));
                       }
                   });
    fields.scalar(snapshot.next_sequence);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastMapleControllerSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::MapleController, instance);
    fields.scalar(snapshot.timing.cycles_per_word);
    fields.optional_scalar(snapshot.completion_event);
    fields.payload(snapshot.pending_responses.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& response : snapshot.pending_responses) {
                           hash.append_u32(response.destination);
                           hash.append_u64(
                               static_cast<std::uint64_t>(response.words.size()));
                           for (const auto word : response.words)
                               hash.append_u32(word);
                       }
                   });
    fields.scalar(snapshot.command_table);
    fields.scalar(snapshot.trigger_select);
    fields.scalar(snapshot.enabled);
    fields.scalar(snapshot.active);
    fields.scalar(snapshot.system_control);
    fields.scalar(snapshot.address_protect);
    fields.scalar(snapshot.msb_select);
    fields.scalar(snapshot.tx_address);
    fields.scalar(snapshot.rx_address);
    fields.scalar(snapshot.rx_base);
    fields.scalar(snapshot.completed_dma_count);
    fields.scalar(snapshot.transferred_word_count);
    fields.scalar(snapshot.hard_trigger_failed);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4TmuSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Tmu, instance);
    fields.scalar(snapshot.scheduler_cycle);
    fields.scalar(snapshot.guest_cycles_per_peripheral_cycle);
    for (const auto& channel : snapshot.channels) {
        fields.scalar(channel.constant);
        fields.scalar(channel.stored_counter);
        fields.scalar(channel.effective_counter);
        fields.scalar(channel.control);
        fields.scalar(channel.anchor_cycle);
        fields.scalar(channel.anchor_counter);
        fields.scalar(channel.underflows);
        fields.optional_scalar(channel.event);
        fields.optional_scalar(channel.event_deadline);
        fields.scalar(channel.running);
    }
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4RtcClockDomain::Snapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::RtcClock, instance);
    fields.scalar(snapshot.guest_cycles_per_second);
    fields.scalar(snapshot.epoch_cycle);
    fields.scalar(snapshot.next_observer_id);
    fields.payload(snapshot.observer_ids.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto id : snapshot.observer_ids)
                           hash.append_u64(id);
                   });
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4RtcSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Rtc, instance);
    fields.scalar(snapshot.date_time.year);
    fields.scalar(snapshot.date_time.month);
    fields.scalar(snapshot.date_time.day);
    fields.scalar(snapshot.date_time.day_of_week);
    fields.scalar(snapshot.date_time.hour);
    fields.scalar(snapshot.date_time.minute);
    fields.scalar(snapshot.date_time.second);
    fields.scalar(snapshot.clock.guest_cycles_per_second);
    fields.scalar(snapshot.clock.epoch_cycle);
    fields.scalar(snapshot.clock.next_observer_id);
    fields.payload(snapshot.clock.observer_ids.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto id : snapshot.clock.observer_ids)
                           hash.append_u64(id);
                   });
    fields.optional_scalar(snapshot.event);
    fields.scalar(static_cast<std::uint64_t>(snapshot.periodic_rate));
    fields.scalar(snapshot.divider_256hz_phase);
    fields.scalar(snapshot.counter_64hz);
    fields.scalar(snapshot.periodic_phase_ticks);
    fields.scalar(snapshot.ticks);
    fields.scalar(snapshot.periodic_events);
    fields.scalar(snapshot.calendar_running);
    fields.scalar(snapshot.rtc_enabled);
    fields.scalar(snapshot.periodic_pending);
    fields.scalar(snapshot.carry_flag);
    fields.scalar(snapshot.carry_enabled);
    for (const auto value : snapshot.alarm_registers)
        fields.scalar(value);
    fields.scalar(snapshot.alarm_pending);
    fields.scalar(snapshot.alarm_enabled);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4ScifSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Scif, instance);
    fields.optional_scalar(snapshot.transmit_event);
    fields.byte_payload(snapshot.transmit_fifo);
    fields.byte_payload(snapshot.receive_fifo);
    fields.byte_payload(snapshot.transmitted_bytes);
    fields.scalar(snapshot.mode);
    fields.scalar(snapshot.bit_rate);
    fields.scalar(snapshot.control);
    fields.scalar(snapshot.status);
    fields.scalar(snapshot.status_last_read);
    fields.scalar(snapshot.fifo_control);
    fields.scalar(snapshot.port);
    fields.scalar(snapshot.line_status);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrTaFifoSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::PvrTa, instance);
    fields.payload(snapshot.accelerator.primitives.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& primitive : snapshot.accelerator.primitives)
                           append_primitive(hash, primitive);
                   });
    fields.payload(snapshot.accelerator.current_strip.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& vertex : snapshot.accelerator.current_strip)
                           append_vertex(hash, vertex);
                   });
    fields.scalar(static_cast<std::uint64_t>(snapshot.accelerator.current_list));
    append_material_fields(fields, snapshot.accelerator.current_material);
    fields.scalar(snapshot.accelerator.highest_list_rank);
    fields.scalar(snapshot.accelerator.frame_has_list);
    fields.scalar(snapshot.accelerator.list_open);
    fields.scalar(static_cast<std::uint64_t>(snapshot.active_list));
    fields.scalar(snapshot.active_textured);
    fields.scalar(snapshot.active_uv16);
    fields.scalar(snapshot.active_color_type);
    fields.scalar(snapshot.active_sprite);
    fields.scalar(snapshot.active_two_volume);
    fields.scalar(snapshot.active_header_argb);
    fields.scalar(snapshot.active_header_oargb);
    fields.scalar(snapshot.active_volume_header_argb);
    fields.scalar(snapshot.intensity_face_color_valid);
    append_material_fields(fields, snapshot.active_material);
    fields.scalar(snapshot.user_clip_start_x);
    fields.scalar(snapshot.user_clip_start_y);
    fields.scalar(snapshot.user_clip_end_x);
    fields.scalar(snapshot.user_clip_end_y);
    fields.scalar(snapshot.pending_sprite_vertex.has_value());
    fields.payload(snapshot.pending_sprite_vertex ? snapshot.pending_sprite_vertex->size() : 0u,
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       if (snapshot.pending_sprite_vertex)
                           hash.append_bytes(*snapshot.pending_sprite_vertex);
                   });
    fields.scalar(snapshot.pending_extended_vertex.has_value());
    const auto pending_vertex =
        snapshot.pending_extended_vertex.value_or(PvrVertex{});
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.x));
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.y));
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.z));
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.u));
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.v));
    fields.scalar(pending_vertex.argb);
    fields.scalar(pending_vertex.oargb);
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.volume_u));
    fields.scalar(std::bit_cast<std::uint32_t>(pending_vertex.volume_v));
    fields.scalar(pending_vertex.volume_argb);
    fields.scalar(pending_vertex.volume_oargb);
    fields.scalar(snapshot.pending_intensity_header);
    fields.scalar(snapshot.pending_extended_end_of_strip);
    fields.payload(snapshot.modifier_volumes.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& volume : snapshot.modifier_volumes)
                           append_modifier_volume(hash, volume);
                   });
    fields.optional_scalar(snapshot.active_modifier_volume);
    fields.scalar(snapshot.pending_modifier_vertex_packet.has_value());
    fields.payload(snapshot.pending_modifier_vertex_packet
                       ? snapshot.pending_modifier_vertex_packet->size()
                       : 0u,
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       if (snapshot.pending_modifier_vertex_packet)
                           hash.append_bytes(
                               *snapshot.pending_modifier_vertex_packet);
                   });
    fields.scalar(snapshot.metrics.packets);
    for (const auto count : snapshot.metrics.normalized_packets)
        fields.scalar(count);
    fields.scalar(snapshot.metrics.polygon_headers);
    fields.scalar(snapshot.metrics.vertices);
    fields.scalar(snapshot.metrics.list_completions);
    fields.scalar(snapshot.metrics.frames);
    fields.scalar(snapshot.metrics.continuations);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrTaFifoMemoryDevice::Snapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::PvrTaAperture, instance);
    fields.byte_payload(snapshot.packet);
    fields.scalar(snapshot.packet_base);
    fields.scalar(snapshot.written_mask);
    fields.scalar(snapshot.packet_active);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(
    const PvrYuvConverterMemoryDevice::Snapshot& snapshot,
    const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::PvrYuv, instance);
    fields.byte_payload(snapshot.input);
    fields.scalar(snapshot.configuration);
    fields.scalar(snapshot.destination);
    fields.scalar(snapshot.frame_macroblock);
    fields.scalar(snapshot.converted_macroblocks);
    fields.scalar(snapshot.guest_memory_access_bound);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PvrSoftwareRendererSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Renderer, instance);
    fields.scalar(snapshot.metrics.frames);
    fields.scalar(snapshot.metrics.triangles);
    fields.scalar(snapshot.metrics.pixels);
    fields.scalar(snapshot.metrics.pixel_writes);
    fields.scalar(snapshot.metrics.changed_pixels);
    fields.scalar(snapshot.metrics.proven_guest_frames);
    fields.scalar(snapshot.metrics.direct_scanout_frames);
    fields.scalar(snapshot.metrics.direct_scanout_changed_pixels);
    fields.scalar(snapshot.metrics.last_frame_pixel_writes);
    fields.scalar(snapshot.metrics.last_frame_changed_pixels);
    fields.scalar(snapshot.metrics.dropped_render_evidence_generations);
    fields.scalar(snapshot.metrics.render_evidence_pixels_examined);
    fields.scalar(snapshot.metrics.render_evidence_range_rejections);
    fields.scalar(snapshot.metrics.render_evidence_scan_budget_exhaustions);
    fields.scalar(snapshot.next_render_generation);
    fields.scalar(snapshot.last_render_generation);
    fields.payload(snapshot.pending_render_evidence.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& evidence : snapshot.pending_render_evidence)
                           append_render_evidence(hash, evidence);
                   });
    fields.scalar(static_cast<std::uint64_t>(snapshot.pending_render_evidence_bytes));
    fields.scalar(snapshot.next_evidence_scan_generation);
    fields.scalar(snapshot.next_direct_write_generation);
    fields.scalar(snapshot.pending_direct_write_generation);
    fields.payload(snapshot.direct_dirty_words.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto value : snapshot.direct_dirty_words)
                           hash.append_u64(value);
                   });
    fields.scalar(static_cast<std::uint64_t>(snapshot.direct_dirty_byte_count));
    fields.byte_payload(snapshot.direct_vram_shadow);
    fields.scalar(snapshot.guest_memory_access_bound);
    fields.scalar(snapshot.direct_vram_shadow_valid);
    fields.scalar(snapshot.queued_guest_frame_proof.has_value());
    fields.payload(snapshot.queued_guest_frame_proof ? 1u : 0u,
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       if (snapshot.queued_guest_frame_proof)
                           append_guest_frame_proof(
                               hash, *snapshot.queued_guest_frame_proof);
                   });
    fields.scalar(snapshot.first_error.has_value());
    fields.scalar(snapshot.first_error
                      ? static_cast<std::uint64_t>(snapshot.first_error->error)
                      : 0u);
    fields.scalar(snapshot.first_error ? snapshot.first_error->render_request : 0u);
    const std::string_view error_detail =
        snapshot.first_error ? std::string_view(snapshot.first_error->detail)
                             : std::string_view{};
    fields.payload(error_detail.size(),
                   [error_detail](RuntimeProbeFnv1a64LeV1& hash) {
                       append_string(hash, error_detail);
                   });
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4IoPortSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::IoPort, instance);
    fields.scalar(snapshot.inputs.port_a);
    fields.scalar(snapshot.inputs.port_b);
    fields.scalar(snapshot.control_a);
    fields.scalar(snapshot.data_a_latch);
    fields.scalar(snapshot.effective_data_a);
    fields.scalar(snapshot.control_b);
    fields.scalar(snapshot.data_b_latch);
    fields.scalar(snapshot.effective_data_b);
    fields.scalar(snapshot.gpio_interrupt_control);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4DmacSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Dmac, instance);
    for (const auto& channel : snapshot.channels) {
        fields.scalar(channel.source);
        fields.scalar(channel.destination);
        fields.scalar(channel.count);
        fields.scalar(channel.control);
        fields.scalar(channel.pending_requests);
        fields.scalar(channel.pending_on_demand_requests);
        fields.scalar(channel.completed_units);
        fields.scalar(
            static_cast<std::uint64_t>(channel.external_destination_progression));
        fields.scalar(channel.interrupt_pending);
    }
    fields.scalar(snapshot.operation);
    fields.scalar(snapshot.timing.guest_cycles_per_byte);
    fields.scalar(static_cast<std::uint64_t>(snapshot.timing.maximum_batch_units));
    fields.scalar(static_cast<std::uint64_t>(snapshot.execution_mode));
    fields.optional_scalar(snapshot.event_id);
    fields.optional_scalar(snapshot.scheduled_channel);
    fields.scalar(static_cast<std::uint64_t>(snapshot.scheduled_units));
    fields.scalar(snapshot.last_fault.has_value());
    fields.scalar(snapshot.last_fault
                      ? static_cast<std::uint64_t>(snapshot.last_fault->reason)
                      : 0u);
    fields.scalar(snapshot.last_fault ? snapshot.last_fault->channel : 0u);
    fields.scalar(snapshot.last_fault ? snapshot.last_fault->source : 0u);
    fields.scalar(snapshot.last_fault ? snapshot.last_fault->destination : 0u);
    fields.scalar(snapshot.last_fault ? snapshot.last_fault->transfer_size : 0u);
    fields.optional_scalar(snapshot.last_on_demand_channel);
    fields.scalar(static_cast<std::uint64_t>(snapshot.round_robin_cursor));
    fields.scalar(snapshot.performance_counters.scheduler_callbacks);
    fields.scalar(snapshot.performance_counters.completed_batches);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const InterruptControllerSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::InterruptController, instance);
    fields.payload(snapshot.pending.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& interrupt : snapshot.pending) {
                           hash.append_u32(interrupt.source);
                           hash.append_u8(interrupt.level);
                           hash.append_u32(interrupt.event_code);
                       }
                   });
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const PlatformInterruptRouterSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::InterruptRouter, instance);
    for (const auto value : snapshot.tmu_levels) fields.scalar(value);
    fields.scalar(snapshot.rtc_level);
    fields.scalar(snapshot.dma_level);
    fields.scalar(snapshot.scif_level);
    for (const auto value : snapshot.scif_pending) fields.scalar(value);
    for (const auto value : snapshot.external_pending) fields.scalar(value);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4InterruptRegistersSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::InterruptRegisters, instance);
    fields.scalar(snapshot.interrupt_control);
    fields.scalar(snapshot.priority_a);
    fields.scalar(snapshot.priority_b);
    fields.scalar(snapshot.priority_c);
    fields.scalar(snapshot.priority_d);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4CacheControlSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::CacheControl, instance);
    fields.scalar(snapshot.value);
    fields.scalar(snapshot.instruction_invalidations);
    fields.payload(snapshot.instruction_addresses.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto value : snapshot.instruction_addresses)
                           hash.append_u32(value);
                   });
    fields.payload(snapshot.operand_addresses.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto value : snapshot.operand_addresses)
                           hash.append_u32(value);
                   });
    fields.byte_payload(snapshot.instruction_data);
    fields.byte_payload(snapshot.operand_data);
    fields.byte_payload(snapshot.on_chip_ram);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const RuntimeAddressSpaceSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::AddressSpace, instance);
    fields.scalar(static_cast<std::uint64_t>(snapshot.mode));
    fields.scalar(snapshot.mmucr);
    fields.scalar(snapshot.asid);
    fields.scalar(snapshot.address_space_generation);
    fields.scalar(snapshot.mmu_generation);
    fields.scalar(snapshot.watchpoint_generation);
    fields.payload(snapshot.mappings.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& mapping : snapshot.mappings) {
                           hash.append_u32(mapping.virtual_page);
                           hash.append_u32(mapping.physical_page);
                           hash.append_u32(mapping.page_size);
                           hash.append_u8(mapping.asid);
                           hash.append_u8(mapping.slot);
                           hash.append_bool(mapping.valid);
                           hash.append_bool(mapping.readable);
                           hash.append_bool(mapping.writable);
                           hash.append_bool(mapping.executable);
                           hash.append_bool(mapping.user_access);
                           hash.append_bool(mapping.dirty);
                           hash.append_bool(mapping.shared);
                       }
                   });
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const RuntimeBlockTableSnapshot& snapshot,
                                   const std::uint32_t instance) {
    const auto append_variant = [](RuntimeProbeFnv1a64LeV1& hash,
                                   const BlockVariantKey& variant) {
        hash.append_u64(variant.address_space_generation);
        hash.append_u64(variant.mmu_generation);
        hash.append_u64(variant.watchpoint_generation);
        hash.append_u32(variant.fpscr_mode);
        hash.append_u64(variant.runtime_generation);
    };
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::BlockTable, instance);
    fields.payload(snapshot.records.size(),
                   [&snapshot, &append_variant](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& record : snapshot.records) {
                           hash.append_u64(record.handle.id);
                           hash.append_u64(record.handle.generation);
                           hash.append_u32(record.virtual_start);
                           hash.append_u32(record.physical_origin);
                           hash.append_u32(record.size);
                           hash.append_u8(static_cast<std::uint8_t>(record.end_kind));
                           append_variant(hash, record.variant);
                           append_string(hash, record.identity);
                           append_string(hash, record.provenance);
                           hash.append_bool(record.runtime_registered);
                           hash.append_bool(record.active);
                           hash.append_bool(record.static_block);
                           hash.append_bool(record.aot_template.has_value());
                           if (record.aot_template) {
                               hash.append_u32(record.aot_template->mapping.source_start);
                               hash.append_u32(record.aot_template->mapping.runtime_start);
                               hash.append_u32(record.aot_template->mapping.extent);
                               hash.append_u32(record.aot_template->validation_extent);
                           }
                       }
                   });
    fields.payload(snapshot.rejected.size(),
                   [&snapshot, &append_variant](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& rejected : snapshot.rejected) {
                           hash.append_u32(rejected.virtual_address);
                           append_variant(hash, rejected.variant);
                           hash.append_u64(rejected.generation);
                       }
                   });
    fields.scalar(snapshot.next_id);
    fields.scalar(static_cast<std::uint64_t>(snapshot.active_count));
    fields.scalar(snapshot.static_sealed);
    fields.scalar(snapshot.code_tracker_bound);
    fields.scalar(static_cast<std::uint64_t>(snapshot.lookup_mode));
    fields.scalar(snapshot.lookup_counters.direct_probes);
    fields.scalar(snapshot.lookup_counters.reference_probes);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const ExecutableCodeTrackerSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::CodeTracker, instance);
    fields.payload(snapshot.blocks.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& tracked : snapshot.blocks) {
                           append_string(hash, tracked.block.identity);
                           hash.append_u32(tracked.block.physical_start);
                           hash.append_u32(tracked.block.size);
                           append_string(hash, tracked.block.provenance);
                           hash.append_u64(tracked.block.incoming_links.size());
                           for (const auto& link : tracked.block.incoming_links)
                               append_string(hash, link);
                           hash.append_u8(
                               static_cast<std::uint8_t>(tracked.block.origin));
                           hash.append_bool(tracked.valid);
                       }
                   });
    const auto append_pages = [&fields](const auto& pages) {
        fields.payload(pages.size(), [&pages](RuntimeProbeFnv1a64LeV1& hash) {
            for (const auto& page : pages) {
                hash.append_u32(page.physical_page);
                hash.append_u64(page.generation);
            }
        });
    };
    append_pages(snapshot.page_generations);
    append_pages(snapshot.hotspots);
    fields.scalar(snapshot.invalidation_count);
    fields.payload(snapshot.invalidation_events.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& event : snapshot.invalidation_events) {
                           hash.append_u64(event.sequence);
                           hash.append_u32(event.virtual_address);
                           hash.append_u32(event.physical_address);
                           hash.append_u64(event.size);
                           hash.append_u8(static_cast<std::uint8_t>(event.source));
                           hash.append_bool(event.byte_identical);
                           hash.append_u64(event.pages.size());
                           for (const auto& page : event.pages) {
                               hash.append_u32(page.physical_page);
                               hash.append_u64(page.generation);
                           }
                           hash.append_u64(event.invalidated_blocks.size());
                           for (const auto& id : event.invalidated_blocks)
                               append_string(hash, id);
                           hash.append_u64(event.unlinked_sources.size());
                           for (const auto& id : event.unlinked_sources)
                               append_string(hash, id);
                       }
                   });
    fields.scalar(snapshot.provenance_capacity);
    fields.scalar(snapshot.next_provenance_sequence);
    fields.scalar(snapshot.dropped_provenance_events);
    fields.scalar(static_cast<std::uint64_t>(snapshot.lookup_mode));
    fields.scalar(snapshot.performance_counters.indexed_candidates);
    fields.scalar(snapshot.performance_counters.reference_candidates);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const ExecutableModuleCatalogSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::ModuleCatalog, instance);
    fields.payload(snapshot.modules.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& module : snapshot.modules) {
                           append_string(hash, module.id);
                           append_string(hash, module.source_identity);
                           hash.append_u32(module.guest_start);
                           hash.append_u64(module.bytes.size());
                           hash.append_bytes(module.bytes);
                           hash.append_u64(module.relocations.size());
                           for (const auto& relocation : module.relocations) {
                               hash.append_u32(relocation.offset);
                               hash.append_u32(relocation.type);
                               hash.append_u32(static_cast<std::uint32_t>(
                                   relocation.addend));
                           }
                           hash.append_u64(module.range_roles.size());
                           for (const auto& range : module.range_roles) {
                               hash.append_u32(range.offset);
                               hash.append_u32(range.size);
                               hash.append_u8(static_cast<std::uint8_t>(range.role));
                           }
                           hash.append_u64(module.active_extents.size());
                           for (const auto& extent : module.active_extents) {
                               hash.append_u32(extent.offset);
                               hash.append_u32(extent.size);
                           }
                           hash.append_u8(static_cast<std::uint8_t>(module.kind));
                           hash.append_u64(module.generation);
                           hash.append_u64(module.relocation_generation);
                           hash.append_bool(module.executable_permission);
                           hash.append_bool(module.control_transfer_promotion_allowed);
                           hash.append_bool(module.writable);
                           hash.append_bool(module.active);
                       }
                   });
    fields.payload(snapshot.runtime_write_pages.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& page : snapshot.runtime_write_pages) {
                           hash.append_u32(page.physical_page);
                           for (const auto word : page.written) hash.append_u64(word);
                       }
                   });
    fields.payload(snapshot.active_extent_page_refcounts.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& page : snapshot.active_extent_page_refcounts) {
                           hash.append_u32(page.physical_page);
                           hash.append_u64(page.refcount);
                       }
                   });
    fields.scalar(snapshot.next_runtime_write_module);
    fields.scalar(snapshot.metrics.loads);
    fields.scalar(snapshot.metrics.unloads);
    fields.scalar(snapshot.metrics.replacements);
    fields.scalar(snapshot.metrics.invalidated_blocks);
    fields.scalar(snapshot.metrics.write_index_rejections);
    fields.scalar(snapshot.metrics.write_index_scans);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const FirmwareHandoffSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::FirmwareHandoff, instance);
    fields.payload(snapshot.mappings.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& mapping : snapshot.mappings) {
                           append_string(hash, mapping.name);
                           hash.append_u8(static_cast<std::uint8_t>(mapping.kind));
                           hash.append_u32(mapping.virtual_start);
                           hash.append_u32(mapping.physical_start);
                           hash.append_u32(mapping.size);
                       }
                   });
    fields.payload(snapshot.copies.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& copy : snapshot.copies) {
                           hash.append_u32(copy.source_physical);
                           hash.append_u32(copy.destination_physical);
                           hash.append_u32(copy.size);
                           append_string(hash, copy.provenance);
                           hash.append_bool(copy.byte_verified);
                           hash.append_bool(copy.changed_after_copy);
                       }
                   });
    fields.payload(snapshot.runtime_symbols.size(),
                   [&snapshot](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& symbol : snapshot.runtime_symbols) {
                           append_string(hash, symbol.name);
                           hash.append_u32(symbol.virtual_address);
                           hash.append_u32(symbol.physical_address);
                           append_string(hash, symbol.provenance);
                           hash.append_u64(symbol.guest_cycle);
                       }
                   });
    fields.scalar(snapshot.canonical_origin_count);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const Sh4StoreQueueSnapshot& snapshot,
                                   const std::span<const StoreQueueTransfer> transfer_history,
                                   const std::uint64_t dropped_transfers,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::StoreQueue, instance);
    for (const auto& queue : snapshot.queues) fields.byte_payload(queue);
    for (const auto value : snapshot.qacr) fields.scalar(value);
    fields.byte_payload(snapshot.operand_cache_ram);
    fields.scalar(static_cast<std::uint64_t>(snapshot.operand_cache_ram_profile));
    fields.scalar(snapshot.operand_cache_ram_enabled);
    fields.scalar(snapshot.external_sink_bound);
    fields.scalar(snapshot.address_translator_bound);
    fields.scalar(snapshot.code_tracker_bound);
    fields.scalar(snapshot.transfer_count);
    fields.payload(static_cast<std::uint64_t>(transfer_history.size()),
                   [transfer_history](RuntimeProbeFnv1a64LeV1& hash) {
                       for (const auto& transfer : transfer_history) {
                           hash.append_u8(transfer.queue);
                           hash.append_u32(transfer.source_address);
                           hash.append_u32(transfer.target_address);
                           hash.append_u8(static_cast<std::uint8_t>(transfer.target));
                           hash.append_u32(transfer.instruction.source_pc);
                           hash.append_u32(transfer.instruction.runtime_pc);
                           hash.append_bool(transfer.instruction.valid);
                           hash.append_u64(transfer.retired_guest_instructions);
                           hash.append_u64(
                               static_cast<std::uint64_t>(transfer.bytes.size()));
                           for (const auto byte : transfer.bytes)
                               hash.append_u8(byte);
                       }
                   });
    fields.scalar(dropped_transfers);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const FlashMemorySnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Flash, instance);
    fields.scalar(snapshot.size);
    fields.scalar(static_cast<std::uint64_t>(snapshot.command_state));
    fields.scalar(snapshot.write_protected);
    fields.scalar(snapshot.working_copy_dirty);
    fields.scalar(snapshot.persistent_working_copy);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const MapleVmuSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::Vmu, instance);
    fields.scalar(snapshot.size);
    fields.scalar(snapshot.write_protected);
    fields.scalar(snapshot.working_copy_dirty);
    fields.scalar(snapshot.persistent_working_copy);
    return std::move(fields).finish();
}

namespace {
void append_holly_channel_fields(DeviceFieldBuilder& fields,
                                 const HollyDmaChannelState& channel) {
    fields.scalar(channel.peripheral_address);
    fields.scalar(channel.system_address);
    fields.scalar(channel.length);
    fields.scalar(channel.direction);
    fields.scalar(channel.trigger_select);
    fields.scalar(channel.enabled);
    fields.scalar(channel.active);
    fields.scalar(channel.suspend);
    fields.scalar(channel.peripheral_counter);
    fields.scalar(channel.system_counter);
    fields.scalar(channel.remaining);
    fields.scalar(channel.completion_cycle);
    fields.scalar(channel.remaining_cycles);
    fields.optional_scalar(channel.completion_event);
    fields.scalar(static_cast<std::uint64_t>(channel.fault));
    fields.scalar(channel.fault_count);
}

void append_holly_fault_fields(DeviceFieldBuilder& fields,
                               const std::optional<HollyDmaFault>& fault) {
    fields.scalar(fault.has_value());
    fields.scalar(fault ? static_cast<std::uint64_t>(fault->reason) : 0u);
    fields.scalar(fault && fault->event.has_value());
    fields.scalar(fault && fault->event
                      ? static_cast<std::uint64_t>(*fault->event)
                      : 0u);
    fields.scalar(fault ? fault->channel : 0u);
    fields.scalar(fault ? fault->peripheral_address : 0u);
    fields.scalar(fault ? fault->system_address : 0u);
    fields.scalar(fault ? fault->remaining : 0u);
}
} // namespace

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastG1DmaSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::HollyDma, instance);
    append_holly_channel_fields(fields, snapshot.channel);
    fields.scalar(snapshot.timing.cycles_per_byte);
    fields.scalar(snapshot.bios_handoff_live_address);
    fields.scalar(snapshot.system_mode);
    fields.scalar(snapshot.gdrom_read_access_timing);
    fields.scalar(snapshot.address_protect);
    append_holly_fault_fields(fields, snapshot.last_fault);
    fields.scalar(snapshot.last_g1_fault.has_value());
    fields.scalar(snapshot.last_g1_fault
                      ? static_cast<std::uint64_t>(snapshot.last_g1_fault->reason)
                      : 0u);
    fields.scalar(snapshot.last_g1_fault ? snapshot.last_g1_fault->fault_address : 0u);
    fields.scalar(snapshot.last_g1_fault ? snapshot.last_g1_fault->transferred_bytes : 0u);
    fields.scalar(snapshot.last_g1_fault ? snapshot.last_g1_fault->residue : 0u);
    fields.scalar(snapshot.last_g1_fault
                      ? static_cast<std::uint64_t>(snapshot.last_g1_fault->phase)
                      : 0u);
    fields.scalar(snapshot.reset_observer);
    fields.scalar(snapshot.transfer_handler_bound);
    fields.scalar(snapshot.completion_observer_bound);
    fields.scalar(snapshot.range_validator_bound);
    fields.scalar(snapshot.fault_observer_bound);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastPvrDmaSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::HollyDma, instance);
    append_holly_channel_fields(fields, snapshot.channel);
    fields.scalar(snapshot.timing.cycles_per_byte);
    fields.scalar(snapshot.address_protect);
    append_holly_fault_fields(fields, snapshot.last_fault);
    fields.scalar(snapshot.reset_observer);
    fields.scalar(snapshot.dmac_channel);
    fields.scalar(snapshot.dmac_bound);
    fields.scalar(snapshot.dmac_contract_required);
    fields.scalar(snapshot.completion_observer_bound);
    return std::move(fields).finish();
}

RuntimeProbeDeviceSnapshot
make_runtime_probe_device_snapshot(const DreamcastG2DmaSnapshot& snapshot,
                                   const std::uint32_t instance) {
    DeviceFieldBuilder fields(RuntimeProbeDeviceKind::HollyDma, instance);
    for (const auto& channel : snapshot.channels)
        append_holly_channel_fields(fields, channel);
    fields.scalar(snapshot.timing.cycles_per_byte);
    fields.scalar(snapshot.address_protect);
    fields.scalar(snapshot.ds_timeout);
    fields.scalar(snapshot.tr_timeout);
    fields.scalar(snapshot.modem_timeout);
    fields.scalar(snapshot.modem_wait);
    fields.scalar(snapshot.completed_dma_count);
    append_holly_fault_fields(fields, snapshot.last_fault);
    fields.scalar(snapshot.reset_observer);
    fields.scalar(snapshot.completion_observer_bound);
    return std::move(fields).finish();
}

RuntimeProbeDreamcastSnapshot
capture_runtime_probe_dreamcast(const DreamcastRuntimeState& state,
                                const std::uint64_t audio_buffers,
                                const std::uint64_t audio_frames,
                                const std::uint64_t audio_hash) {
    if (!state.flash || !state.vmu || !state.pvr_registers ||
        !state.system_bus_control || !state.pvr_renderer || !state.pvr_ta_fifo ||
        !state.pvr_ta_aperture || !state.pvr_yuv_converter || !state.gdrom || !state.aica ||
        !state.aica_registers || !state.aica_rtc || !state.dmac ||
        !state.holly_dma.g1 || !state.holly_dma.g2 || !state.holly_dma.pvr ||
        !state.interrupt_controller || !state.store_queues || !state.maple ||
        !state.maple_controller || !state.tmu || !state.rtc || !state.scif ||
        !state.rtc_clock || !state.system_asic || !state.io_ports ||
        !state.interrupt_router || !state.interrupt_registers ||
        !state.cache_control || !state.address_space || !state.runtime_blocks ||
        !state.code_tracker || !state.module_catalog || !state.firmware_handoff ||
        !state.store_queue_transfers || !state.dropped_store_queue_transfers) {
        throw std::invalid_argument(
            "Runtime-Probe braucht einen vollstaendigen Dreamcast-Produktzustand.");
    }

    RuntimeProbeDreamcastSnapshot result;
    result.flash.resize(state.flash->size());
    for (std::size_t offset = 0u; offset < result.flash.size(); ++offset)
        result.flash[offset] = state.flash->read_u8(static_cast<std::uint32_t>(offset));
    result.vmu.resize(vmu_storage_size);
    for (std::size_t offset = 0u; offset < result.vmu.size(); ++offset)
        result.vmu[offset] = state.vmu->read_byte(offset);

    const auto append_simple = [&result](const RuntimeProbeDeviceKind kind,
                                         const std::uint32_t instance,
                                         const auto& populate) {
        DeviceFieldBuilder fields(kind, instance);
        std::invoke(populate, fields);
        result.devices.push_back(std::move(fields).finish());
    };
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.pvr_registers->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.system_bus_control->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.system_asic->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.pvr_ta_fifo->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.pvr_ta_aperture->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.pvr_yuv_converter->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.pvr_renderer->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.gdrom->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.aica_registers->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.aica_rtc->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.aica->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.maple->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.maple_controller->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.tmu->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.rtc_clock->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.rtc->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.scif->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.io_ports->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.dmac->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.interrupt_controller->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.interrupt_router->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.interrupt_registers->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.cache_control->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.address_space->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.runtime_blocks->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.code_tracker->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.module_catalog->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.firmware_handoff->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(
            state.store_queues->snapshot(),
            *state.store_queue_transfers,
            *state.dropped_store_queue_transfers));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.flash->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.vmu->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.holly_dma.g1->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.holly_dma.pvr->snapshot()));
    result.devices.push_back(
        make_runtime_probe_device_snapshot(state.holly_dma.g2->snapshot()));
    append_simple(RuntimeProbeDeviceKind::HostAudio,
                  0u,
                  [&](DeviceFieldBuilder& fields) {
                      fields.scalar(audio_buffers);
                      fields.scalar(audio_frames);
                      fields.scalar(audio_hash);
                  });

    return result;
}

std::uint64_t
hash_runtime_probe_cpu(const RuntimeProbeCpuSnapshot& snapshot) noexcept {
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Cpu);
    hash.append_u32(static_cast<std::uint32_t>(snapshot.r.size()));
    for (const auto value : snapshot.r)
        hash.append_u32(value);
    hash.append_u32(static_cast<std::uint32_t>(snapshot.r_bank.size()));
    for (const auto value : snapshot.r_bank)
        hash.append_u32(value);
    hash.append_u32(static_cast<std::uint32_t>(snapshot.fr.size()));
    for (const auto value : snapshot.fr)
        hash.append_u32(value);
    hash.append_u32(static_cast<std::uint32_t>(snapshot.xf.size()));
    for (const auto value : snapshot.xf)
        hash.append_u32(value);
    for (const auto value : {snapshot.pc,
                             snapshot.pr,
                             snapshot.gbr,
                             snapshot.vbr,
                             snapshot.ssr,
                             snapshot.spc,
                             snapshot.sgr,
                             snapshot.dbr,
                             snapshot.tra,
                             snapshot.tea,
                             snapshot.expevt,
                             snapshot.intevt,
                             snapshot.pteh,
                             snapshot.ptel,
                             snapshot.ptea,
                             snapshot.ttb,
                             snapshot.mmucr})
        hash.append_u32(value);
    hash.append_u32(static_cast<std::uint32_t>(snapshot.utlb.size()));
    for (const auto& entry : snapshot.utlb) {
        hash.append_u32(entry.pteh);
        hash.append_u32(entry.ptel);
        hash.append_u32(entry.ptea);
    }
    hash.append_u64(snapshot.tlb_load_count);
    for (const auto value : {
             snapshot.mach, snapshot.macl, snapshot.fpul, snapshot.fpscr, snapshot.sr})
        hash.append_u32(value);
    hash.append_bool(snapshot.t);
    hash.append_bool(snapshot.s);
    hash.append_bool(snapshot.q);
    hash.append_bool(snapshot.m);
    hash.append_bool(snapshot.trap_pending);
    hash.append_u8(static_cast<std::uint8_t>(snapshot.last_exception_cause));
    hash.append_bool(snapshot.exception_in_delay_slot);
    hash.append_bool(snapshot.sleeping);
    hash.append_u32(snapshot.last_prefetch_address);
    hash.append_u64(snapshot.prefetch_count);
    hash.append_u64(snapshot.retired_guest_instructions);
    hash.append_bool(snapshot.last_prefetch_was_store_queue);
    return hash.value();
}

std::uint64_t hash_runtime_probe_scheduler(
    const RuntimeProbeSchedulerSnapshot& snapshot) noexcept {
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Scheduler);
    hash.append_u64(snapshot.current_cycle);
    append_optional(hash, snapshot.next_event_cycle);
    hash.append_u64(snapshot.pending_event_count);
    hash.append_u64(snapshot.next_event_id);
    hash.append_u64(snapshot.next_reset_observer_id);
    hash.append_u64(snapshot.processed_event_count);
    hash.append_u64(snapshot.reset_generation);
    append_optional(hash, snapshot.guest_cycle_budget);
    hash.append_bool(snapshot.advance_in_progress);
    hash.append_u64(static_cast<std::uint64_t>(snapshot.pending_events.size()));
    for (const auto& event : snapshot.pending_events) {
        hash.append_u64(event.guest_cycle);
        hash.append_u64(event.event_id);
        hash.append_u32(static_cast<std::uint32_t>(event.kind));
    }
    hash.append_u64(static_cast<std::uint64_t>(snapshot.reset_observer_ids.size()));
    for (const auto observer_id : snapshot.reset_observer_ids)
        hash.append_u64(observer_id);
    return hash.value();
}

std::uint64_t
hash_runtime_probe_memory(const std::span<const RuntimeProbeMemoryRange> ranges) {
    const auto ordered = canonical_memory_ranges(ranges, false);
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Memory);
    hash.append_u64(static_cast<std::uint64_t>(ordered.size()));
    for (const auto* range : ordered) {
        hash.append_u32(static_cast<std::uint32_t>(range->region));
        hash.append_u32(range->offset);
        hash.append_u64(static_cast<std::uint64_t>(range->bytes.size()));
        hash.append_bytes(range->bytes);
    }
    return hash.value();
}

std::uint64_t
hash_runtime_probe_persistent(const std::span<const RuntimeProbeMemoryRange> ranges) {
    const auto ordered = canonical_memory_ranges(ranges, true);
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Persistent);
    hash.append_u64(static_cast<std::uint64_t>(ordered.size()));
    for (const auto* range : ordered) {
        hash.append_u32(static_cast<std::uint32_t>(range->region));
        hash.append_u32(range->offset);
        hash.append_u64(static_cast<std::uint64_t>(range->bytes.size()));
        hash.append_bytes(range->bytes);
    }
    return hash.value();
}

std::uint64_t
hash_runtime_probe_devices(const std::span<const RuntimeProbeDeviceSnapshot> devices) {
    const auto ordered = canonical_devices(devices);
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Devices);
    hash.append_u64(static_cast<std::uint64_t>(ordered.size()));
    for (const auto* device : ordered) {
        const auto fields = canonical_device_fields(*device);
        hash.append_u32(static_cast<std::uint32_t>(device->kind));
        hash.append_u32(device->instance);
        hash.append_u64(static_cast<std::uint64_t>(fields.size()));
        for (const auto* field : fields) {
            hash.append_u32(field->id);
            hash.append_u64(field->value);
        }
    }
    return hash.value();
}

std::uint64_t
hash_runtime_probe_replay(const RuntimeProbeReplaySnapshot& snapshot) noexcept {
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Replay);
    hash.append_u32(snapshot.replay_schema_version);
    hash.append_u32(snapshot.storage_mode);
    hash.append_u64(snapshot.retention_capacity);
    hash.append_u64(snapshot.event_count);
    hash.append_u64(snapshot.retained_event_count);
    hash.append_u64(snapshot.summarized_event_count);
    hash.append_u64(snapshot.dropped_events);
    hash.append_u64(snapshot.event_hash);
    hash.append_u64(snapshot.ordering_digest);
    hash.append_u32(snapshot.enabled_coverage);
    hash.append_u32(snapshot.observed_coverage);
    hash.append_u32(snapshot.required_coverage);
    for (const auto count : snapshot.event_counts)
        hash.append_u64(count);
    hash.append_bool(snapshot.coverage_complete);
    hash.append_bool(snapshot.complete);
    hash.append_bool(snapshot.exact_event_stream);
    hash.append_bool(snapshot.sealed);
    append_optional(hash, snapshot.final_guest_state_hash);
    return hash.value();
}

std::uint64_t combine_runtime_probe_guest_state_hashes(
    const std::uint64_t cpu_hash,
    const std::uint64_t scheduler_hash,
    const std::uint64_t memory_hash,
    const std::uint64_t persistent_hash,
    const std::uint64_t device_hash) noexcept {
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::GuestState);
    hash.append_u64(cpu_hash);
    hash.append_u64(scheduler_hash);
    hash.append_u64(memory_hash);
    hash.append_u64(persistent_hash);
    hash.append_u64(device_hash);
    return hash.value();
}

std::uint64_t combine_runtime_probe_hashes(const std::uint64_t guest_state_hash,
                                           const std::uint64_t replay_hash) noexcept {
    RuntimeProbeFnv1a64LeV1 hash;
    begin_hash(hash, HashDomain::Combined);
    hash.append_u64(guest_state_hash);
    hash.append_u64(replay_hash);
    return hash.value();
}

void validate_runtime_probe_deterministic_v1(
    const RuntimeProbeSchedulerSnapshot& scheduler,
    const std::span<const RuntimeProbeMemoryRange> memory,
    const std::span<const RuntimeProbeMemoryRange> persistent,
    const std::span<const RuntimeProbeDeviceSnapshot> devices,
    const RuntimeProbeReplaySnapshot& replay) {
    validate_scheduler_profile(scheduler);
    constexpr std::array volatile_memory = {
        std::pair{RuntimeProbeMemoryRegion::MainRam, dreamcast_main_ram_size},
        std::pair{RuntimeProbeMemoryRegion::VideoRam, dreamcast_vram_size},
        std::pair{RuntimeProbeMemoryRegion::AicaRam, dreamcast_aica_ram_size},
    };
    constexpr std::array persistent_memory = {
        std::pair{RuntimeProbeMemoryRegion::Flash, dreamcast_flash_size},
        std::pair{RuntimeProbeMemoryRegion::Vmu, vmu_storage_size},
    };
    require_exact_memory_profile(memory, volatile_memory, false);
    require_exact_memory_profile(persistent, persistent_memory, true);
    const auto ordered_devices = canonical_devices(devices);
    if (ordered_devices.size() !=
        runtime_probe_deterministic_v1_device_schemas.size()) {
        throw std::invalid_argument(
            "Deterministic-v1 braucht vollstaendige Geraetesnapshots.");
    }
    for (std::size_t index = 0u; index < ordered_devices.size(); ++index) {
        const auto* device = ordered_devices[index];
        const auto& schema =
            runtime_probe_deterministic_v1_device_schemas[index];
        const auto fields = canonical_device_fields(*device);
        if (device->kind != schema.kind || device->instance != schema.instance ||
            fields.size() != schema.field_count ||
            fields.empty() ||
            fields.front()->value != runtime_probe_device_schema_version) {
            throw std::invalid_argument(
                "Deterministic-v1-Geraetesnapshot verletzt Instanz oder Schema.");
        }
        for (std::size_t field = 0u; field < fields.size(); ++field) {
            if (fields[field]->id != field + 1u) {
                throw std::invalid_argument(
                    "Deterministic-v1-Geraetefeldschema besitzt eine Luecke.");
            }
        }
    }
    const auto required_replay_coverage =
        system_replay_required_coverage(SystemReplayProfile::DeterministicV1);
    const auto exact_replay_storage =
        static_cast<std::uint32_t>(SystemReplayStorageMode::ExactEvents);
    const auto digest_replay_storage =
        static_cast<std::uint32_t>(SystemReplayStorageMode::DigestStream);
    const auto known_replay_storage =
        replay.storage_mode == exact_replay_storage ||
        replay.storage_mode == digest_replay_storage;
    const auto retention_sum_overflows =
        replay.summarized_event_count >
        std::numeric_limits<std::uint64_t>::max() -
            replay.retained_event_count;
    const auto retention_sum =
        retention_sum_overflows
            ? 0u
            : replay.retained_event_count + replay.summarized_event_count;
    if (replay.replay_schema_version != system_replay_schema_version ||
        !known_replay_storage ||
        replay.retention_capacity == 0u ||
        replay.retention_capacity > SystemReplayConfig::maximum_capacity ||
        replay.retained_event_count > replay.retention_capacity ||
        retention_sum_overflows ||
        retention_sum != replay.event_count ||
        (replay.summarized_event_count != 0u &&
         replay.retained_event_count != replay.retention_capacity) ||
        replay.exact_event_stream !=
            (replay.summarized_event_count == 0u &&
             replay.dropped_events == 0u) ||
        (replay.storage_mode == exact_replay_storage &&
         replay.summarized_event_count != 0u) ||
        replay.required_coverage != required_replay_coverage ||
        replay.enabled_coverage != required_replay_coverage ||
        !replay.coverage_complete ||
        replay.ordering_digest != replay.event_hash ||
        replay.complete !=
            (replay.dropped_events == 0u && replay.coverage_complete) ||
        (replay.observed_coverage & ~replay.enabled_coverage) != 0u ||
        replay.sealed != replay.final_guest_state_hash.has_value()) {
        throw std::invalid_argument(
            "Deterministic-v1 besitzt keinen vollstaendigen Replay-Coverage-Vertrag.");
    }
    std::uint64_t covered_events = 0u;
    std::uint32_t counted_coverage = 0u;
    for (std::size_t index = 0u; index < replay.event_counts.size(); ++index) {
        if (replay.event_counts[index] >
            std::numeric_limits<std::uint64_t>::max() - covered_events) {
            throw std::overflow_error(
                "Deterministic-v1-Replay-Ereigniszaehler laufen ueber.");
        }
        covered_events += replay.event_counts[index];
        if (replay.event_counts[index] != 0u)
            counted_coverage |= std::uint32_t{1u} << index;
    }
    if (covered_events > replay.event_count ||
        counted_coverage != replay.observed_coverage) {
        throw std::invalid_argument(
            "Deterministic-v1-Replay-Coverage und Ereigniszaehler widersprechen sich.");
    }
}

RuntimeProbeReport
make_runtime_probe_report(const RuntimeProbeCpuSnapshot& cpu,
                          const RuntimeProbeSchedulerSnapshot& scheduler,
                          const std::span<const RuntimeProbeMemoryRange> memory,
                          const std::span<const RuntimeProbeMemoryRange> persistent,
                          const std::span<const RuntimeProbeDeviceSnapshot> devices,
                          const RuntimeProbeReplaySnapshot& replay) {
    validate_runtime_probe_deterministic_v1(
        scheduler, memory, persistent, devices, replay);
    RuntimeProbeReport report;
    report.status = replay.complete && replay.sealed ? RuntimeProbeStatus::Complete
                                                     : RuntimeProbeStatus::Incomplete;
    report.guest_cycle_budget = scheduler.guest_cycle_budget;
    report.guest_cycle = scheduler.current_cycle;
    report.retired_guest_instructions = cpu.retired_guest_instructions;
    report.memory_byte_count = checked_memory_byte_count(memory);
    report.memory_range_count = static_cast<std::uint64_t>(memory.size());
    report.persistent_byte_count = checked_memory_byte_count(persistent);
    report.persistent_range_count = static_cast<std::uint64_t>(persistent.size());
    report.device_count = static_cast<std::uint64_t>(devices.size());
    report.device_field_count = checked_device_field_count(devices);
    report.replay = replay;
    report.hashes.cpu = hash_runtime_probe_cpu(cpu);
    report.hashes.scheduler = hash_runtime_probe_scheduler(scheduler);
    report.hashes.memory = hash_runtime_probe_memory(memory);
    report.hashes.persistent = hash_runtime_probe_persistent(persistent);
    report.hashes.devices = hash_runtime_probe_devices(devices);
    report.hashes.replay = hash_runtime_probe_replay(replay);
    report.hashes.guest_state =
        combine_runtime_probe_guest_state_hashes(report.hashes.cpu,
                                                 report.hashes.scheduler,
                                                 report.hashes.memory,
                                                 report.hashes.persistent,
                                                 report.hashes.devices);
    if (replay.sealed &&
        (!replay.final_guest_state_hash.has_value() ||
         *replay.final_guest_state_hash != report.hashes.guest_state)) {
        throw std::invalid_argument(
            "Versiegelter Runtime-Probe-Replay passt nicht zum Gastzustand.");
    }
    report.hashes.combined =
        combine_runtime_probe_hashes(report.hashes.guest_state, report.hashes.replay);
    return report;
}

std::string serialize_runtime_probe_report_json(const RuntimeProbeReport& report) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schema\":\"katana.runtime-probe\",\"probe_version\":"
           << report.schema_version
           << ",\"profile\":\"deterministic-v1\",\"hash_contract\":\""
           << runtime_probe_hash_contract << "\",\"status\":\""
           << status_name(report.status) << "\",\"termination\":\""
           << termination_name(report.termination)
           << "\",\"diagnostics_enabled\":"
           << (report.diagnostics_enabled ? "true" : "false")
           << ",\"guest_cycle_budget\":";
    if (report.guest_cycle_budget.has_value())
        output << *report.guest_cycle_budget;
    else
        output << "null";
    output << ",\"guest_cycle\":" << report.guest_cycle
           << ",\"retired_guest_instructions\":"
           << report.retired_guest_instructions
           << ",\"memory_byte_count\":" << report.memory_byte_count
           << ",\"memory_range_count\":" << report.memory_range_count
           << ",\"persistent_byte_count\":" << report.persistent_byte_count
           << ",\"persistent_range_count\":" << report.persistent_range_count
           << ",\"device_count\":" << report.device_count
           << ",\"device_field_count\":" << report.device_field_count
           << ",\"replay\":{\"storage_mode\":\""
           << system_replay_storage_mode_name(
                  static_cast<SystemReplayStorageMode>(
                      report.replay.storage_mode))
           << "\",\"retention_capacity\":"
           << report.replay.retention_capacity
           << ",\"event_count\":" << report.replay.event_count
           << ",\"retained_event_count\":"
           << report.replay.retained_event_count
           << ",\"summarized_event_count\":"
           << report.replay.summarized_event_count
           << ",\"dropped_events\":" << report.replay.dropped_events
           << ",\"complete\":" << (report.replay.complete ? "true" : "false")
           << ",\"exact_event_stream\":"
           << (report.replay.exact_event_stream ? "true" : "false")
           << ",\"sealed\":" << (report.replay.sealed ? "true" : "false")
           << "},\"hashes\":{\"cpu\":";
    append_hex64(output, report.hashes.cpu);
    output << ",\"scheduler\":";
    append_hex64(output, report.hashes.scheduler);
    output << ",\"memory\":";
    append_hex64(output, report.hashes.memory);
    output << ",\"persistent\":";
    append_hex64(output, report.hashes.persistent);
    output << ",\"devices\":";
    append_hex64(output, report.hashes.devices);
    output << ",\"guest_state\":";
    append_hex64(output, report.hashes.guest_state);
    output << ",\"replay\":";
    append_hex64(output, report.hashes.replay);
    output << ",\"combined\":";
    append_hex64(output, report.hashes.combined);
    output << "}}";
    return output.str();
}

std::string serialize_runtime_probe_fault_envelope_json(
    const RuntimeProbeFaultEnvelope& envelope) {
    if (envelope.report_version != runtime_probe_fault_report_version)
        throw std::invalid_argument(
            "Runtime-Probe-Fehlerpaket besitzt eine unbekannte Version.");
    if (!fault_termination(envelope.termination))
        throw std::invalid_argument(
            "Runtime-Probe-Fehlerpaket besitzt keine stabile Fehlerklasse.");
    if (envelope.first_fault.has_value()) {
        if (!fault_termination(envelope.first_fault->termination) ||
            envelope.termination != envelope.first_fault->termination) {
            throw std::invalid_argument(
                "Runtime-Probe-Fehlerpaket verletzt den First-Fault-Vertrag.");
        }
    } else {
        throw std::invalid_argument(
            "Runtime-Probe-Fehlerklasse besitzt keinen gelatchten CPU-Zustand.");
    }
    if (envelope.last_stable_checkpoint.has_value() &&
        (envelope.last_stable_checkpoint->checkpoint ==
             RuntimeProbeCheckpoint::None ||
         !known_checkpoint(envelope.last_stable_checkpoint->checkpoint))) {
        throw std::invalid_argument(
            "Runtime-Probe-Fehlerpaket besitzt keinen stabilen Checkpoint.");
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"schema\":\"katana.runtime-probe-fault\",\"report_version\":"
           << envelope.report_version << ",\"termination\":\""
           << termination_name(envelope.termination)
           << "\",\"first_fault_present\":"
           << (envelope.first_fault.has_value() ? "true" : "false")
           << ",\"first_fault\":";
    if (envelope.first_fault.has_value())
        output << '"' << termination_name(envelope.first_fault->termination) << '"';
    else
        output << "null";
    output << ",\"last_checkpoint_present\":"
           << (envelope.last_stable_checkpoint.has_value() ? "true" : "false")
           << ",\"last_checkpoint\":";
    if (envelope.last_stable_checkpoint.has_value())
        output << '"'
               << checkpoint_name(envelope.last_stable_checkpoint->checkpoint)
               << '"';
    else
        output << "null";
    output << '}';
    return output.str();
}

} // namespace katana::runtime
