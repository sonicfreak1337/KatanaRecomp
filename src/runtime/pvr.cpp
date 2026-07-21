#include "katana/runtime/pvr.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {

PvrRegisterFile::PvrRegisterFile(EventScheduler& scheduler,
                                 const PvrTiming timing,
                                 std::function<void()> render_observer,
                                 std::function<void(bool)> vblank_observer)
    : scheduler_(scheduler), timing_(timing), scheduler_lifetime_(scheduler.lifetime_token()),
      render_observer_(std::move(render_observer)),
      vblank_observer_(std::move(vblank_observer)) {
    if (timing_.guest_clock_hz == 0u || timing_.pixel_clock_hz == 0u)
        throw std::invalid_argument("PVR-SPG braucht positive Gast- und Pixeltakte.");
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

PvrRegisterFile::~PvrRegisterFile() {
    if (scheduler_lifetime_.expired()) return;
    for (const auto event : render_events_)
        static_cast<void>(scheduler_.cancel(event));
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
        const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
        if (scan_frame_cycles_ == 0u || vertical <= 1u) return 0u;

        const auto elapsed = scheduler_.current_cycle() - scan_epoch_cycle_;
        const auto frame_cycle = elapsed % scan_frame_cycles_;
        const auto scanline = std::min<std::uint64_t>(
            vertical - 1u, frame_cycle * vertical / scan_frame_cycles_);
        const auto vblank = registers_[index(pvr_register::SpgVblank)];
        const auto blank_start = static_cast<std::uint64_t>(vblank & 0x3FFu) % vertical;
        const auto blank_end = static_cast<std::uint64_t>((vblank >> 16u) & 0x3FFu) % vertical;
        const auto blank = blank_start <= blank_end
                               ? scanline >= blank_start && scanline < blank_end
                               : scanline >= blank_start || scanline < blank_end;
        return static_cast<std::uint32_t>(scanline) | (field_ << 10u) |
               (blank ? 1u << 11u : 0u);
    }
    return registers_[index(offset)];
}

void PvrRegisterFile::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == pvr_register::Id || offset == pvr_register::Revision ||
        offset == pvr_register::SpgStatus || offset == pvr_register::TaNextOpb ||
        offset == pvr_register::TaIspCurrent) {
        throw std::runtime_error("Read-only-PVR-Register ist nicht beschreibbar.");
    }
    if (offset == pvr_register::SoftReset) {
        if ((value & 0x7u) != 0u) {
            reset();
        }
        return;
    }
    if (offset == pvr_register::StartRender) {
        ++render_requests_;
        const auto event = scheduler_.schedule_after(
            timing_.render_latency,
            [this](const auto event_id, const auto) { complete_render(event_id); });
        render_events_.insert(event);
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
    registers_[index(offset)] = value;
    if (offset == pvr_register::SpgControl || offset == pvr_register::SpgLoad ||
        offset == pvr_register::SpgVblank || offset == pvr_register::VideoControl)
        reschedule_scanout();
}

void PvrRegisterFile::reset() noexcept {
    for (const auto event : render_events_)
        static_cast<void>(scheduler_.cancel(event));
    render_events_.clear();
    cancel_scan_events();
    registers_.fill(0u);
    scan_frame_cycles_ = 0u;
    scan_epoch_cycle_ = 0u;
    in_vblank_ = false;
    field_ = 0u;
    ++resets_;
    if (ta_reset_observer_) ta_reset_observer_();
}

void PvrRegisterFile::complete_render(const SchedulerEventId event_id) {
    if (render_events_.erase(event_id) == 0u)
        throw std::logic_error("PVR-Rendercompletion besitzt keinen Request.");
    ++render_completions_;
    if (render_observer_) render_observer_();
}

void PvrRegisterFile::handle_scheduler_reset() noexcept {
    render_events_.clear();
    vblank_in_event_.reset();
    vblank_out_event_.reset();
    scan_frame_cycles_ = 0u;
    scan_epoch_cycle_ = 0u;
    in_vblank_ = false;
}

std::uint64_t PvrRegisterFile::render_request_count() const noexcept {
    return render_requests_;
}
std::uint64_t PvrRegisterFile::render_completion_count() const noexcept {
    return render_completions_;
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
bool PvrRegisterFile::in_vblank() const noexcept {
    return in_vblank_;
}
std::uint32_t PvrRegisterFile::field() const noexcept {
    return field_;
}
void PvrRegisterFile::set_render_observer(std::function<void()> observer) {
    render_observer_ = std::move(observer);
}
void PvrRegisterFile::set_vblank_observer(std::function<void(bool)> observer) {
    vblank_observer_ = std::move(observer);
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
    vblank_in_event_.reset();
    vblank_out_event_.reset();
}

void PvrRegisterFile::reschedule_scanout() {
    cancel_scan_events();
    const auto load = registers_[index(pvr_register::SpgLoad)];
    const auto horizontal = static_cast<std::uint64_t>(load & 0x3FFu) + 1u;
    const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
    scan_frame_cycles_ = 0u;
    scan_epoch_cycle_ = scheduler_.current_cycle();
    in_vblank_ = false;
    if (horizontal <= 1u || vertical <= 1u) return;
    const auto pixels = horizontal * vertical;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / timing_.guest_clock_hz)
        throw std::out_of_range("PVR-SPG-Frameperiode laeuft ueber.");
    scan_frame_cycles_ = std::max<std::uint64_t>(
        1u, (pixels * timing_.guest_clock_hz + timing_.pixel_clock_hz - 1u) /
                timing_.pixel_clock_hz);
    const auto vblank = registers_[index(pvr_register::SpgVblank)];
    const auto start = vblank & 0x3FFu;
    const auto end = (vblank >> 16u) & 0x3FFu;
    schedule_scan_event(start % static_cast<std::uint32_t>(vertical), true);
    schedule_scan_event(end % static_cast<std::uint32_t>(vertical), false);
}

void PvrRegisterFile::schedule_scan_event(const std::uint32_t line, const bool entering) {
    const auto load = registers_[index(pvr_register::SpgLoad)];
    const auto vertical = static_cast<std::uint64_t>((load >> 16u) & 0x3FFu) + 1u;
    const auto delay = std::max<std::uint64_t>(1u, scan_frame_cycles_ * line / vertical);
    const auto event = scheduler_.schedule_after(
        delay, [this, entering](const auto id, const auto) { handle_scan_event(id, entering); });
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
        field_ ^= 1u;
    } else {
        ++vblank_out_count_;
    }
    if (vblank_observer_) vblank_observer_(entering);
    if (scan_frame_cycles_ != 0u) {
        const auto next = scheduler_.schedule_after(
            scan_frame_cycles_,
            [this, entering](const auto id, const auto) { handle_scan_event(id, entering); });
        slot = next;
    }
}

void configure_dreamcast_video(PvrRegisterFile& registers, const DreamcastVideoMode mode) {
    struct Profile {
        std::uint32_t load;
        std::uint32_t hblank;
        std::uint32_t vblank;
        std::uint32_t width;
        std::uint32_t control;
        std::uint32_t start_x;
        std::uint32_t start_y;
    };
    constexpr std::array profiles{
        Profile{0x01060359u, 0x007E0345u, 0x00120102u, 0x03F1933Fu,
                0x00000140u, 0x000000A4u, 0x00120011u},
        Profile{0x020C0359u, 0x007E0345u, 0x00240204u, 0x07D6C63Fu,
                0x00000150u, 0x000000A4u, 0x00120012u},
        Profile{0x0138035Fu, 0x008D034Bu, 0x002C026Cu, 0x07F1F53Fu,
                0x00000180u, 0x000000AEu, 0x002E002Eu},
        Profile{0x0270035Fu, 0x008D034Bu, 0x002C026Cu, 0x07D6A53Fu,
                0x00000190u, 0x000000AEu, 0x002E002Du},
        Profile{0x020C0359u, 0x007E0345u, 0x00280208u, 0x03F1933Fu,
                0x00000100u, 0x000000A8u, 0x00280028u},
    };
    const auto selected = static_cast<std::size_t>(mode);
    if (selected >= profiles.size()) throw std::invalid_argument("Unbekannter Dreamcast-Videomodus.");
    const auto& profile = profiles[selected];
    registers.write(pvr_register::SpgHblank, profile.hblank);
    registers.write(pvr_register::SpgVblank, profile.vblank);
    registers.write(pvr_register::SpgWidth, profile.width);
    registers.write(pvr_register::VideoStartX, profile.start_x);
    registers.write(pvr_register::VideoStartY, profile.start_y);
    registers.write(pvr_register::VideoControl, 0x00160000u);
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

void write_render_pixel(LinearMemoryDevice& vram,
                        const std::uint32_t offset,
                        const std::uint32_t pack_mode,
                        const std::uint8_t alpha,
                        const std::uint8_t red,
                        const std::uint8_t green,
                        const std::uint8_t blue) {
    switch (pack_mode) {
    case 0u:
        vram.write_u16(offset,
                       static_cast<std::uint16_t>(((red >> 3u) << 10u) |
                                                  ((green >> 3u) << 5u) | (blue >> 3u)));
        return;
    case 1u:
        vram.write_u16(offset,
                       static_cast<std::uint16_t>(((red >> 3u) << 11u) |
                                                  ((green >> 2u) << 5u) | (blue >> 3u)));
        return;
    case 2u:
        vram.write_u16(offset,
                       static_cast<std::uint16_t>(((alpha >> 4u) << 12u) |
                                                  ((red >> 4u) << 8u) |
                                                  ((green >> 4u) << 4u) | (blue >> 4u)));
        return;
    case 3u:
        vram.write_u16(offset,
                       static_cast<std::uint16_t>(((alpha >= 0x80u) ? 0x8000u : 0u) |
                                                  ((red >> 3u) << 10u) |
                                                  ((green >> 3u) << 5u) | (blue >> 3u)));
        return;
    case 5u:
        vram.write_u32(offset, static_cast<std::uint32_t>(red) << 16u |
                                   static_cast<std::uint32_t>(green) << 8u | blue);
        return;
    case 6u:
        vram.write_u32(offset, static_cast<std::uint32_t>(alpha) << 24u |
                                   static_cast<std::uint32_t>(red) << 16u |
                                   static_cast<std::uint32_t>(green) << 8u | blue);
        return;
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

Rgba8 read_render_pixel(const LinearMemoryDevice& vram,
                        const std::uint32_t offset,
                        const std::uint32_t pack_mode) {
    if (pack_mode <= 3u) {
        const auto pixel = vram.read_u16(offset);
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
    const auto pixel = vram.read_u32(offset);
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

Rgba8 decode_palette_color(const PvrRegisterFile& registers, const std::uint32_t index) {
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

std::uint8_t table_fog_coefficient(const PvrRegisterFile& registers, const float depth) {
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

Rgba8 clamp_fragment_color(const Rgba8 source, const PvrRegisterFile& registers) {
    const auto minimum = decode_register_color(registers.read(pvr_register::ColorClampMinimum));
    const auto maximum = decode_register_color(registers.read(pvr_register::ColorClampMaximum));
    if (minimum.r > maximum.r || minimum.g > maximum.g || minimum.b > maximum.b)
        throw std::runtime_error("PVR-Farbclamp besitzt vertauschte RGB-Grenzen.");
    return {std::clamp(source.r, minimum.r, maximum.r),
            std::clamp(source.g, minimum.g, maximum.g),
            std::clamp(source.b, minimum.b, maximum.b),
            source.a};
}

Rgba8 sample_texture_nearest(const LinearMemoryDevice& vram,
                             const PvrRegisterFile& registers,
                             const PvrMaterial& material,
                             float u,
                             float v) {
    if (!material.textured) return {};
    if (material.texture_width == 0u || material.texture_height == 0u ||
        material.texture_width > 1024u || material.texture_height > 1024u)
        throw std::runtime_error("PVR-Texturgeometrie liegt ausserhalb des Hardwarevertrags.");
    if (material.texture_vq && material.texture_format > 2u)
        throw std::runtime_error("PVR-VQ ist nur fuer 16-Bit-Farbtexturen definiert.");
    if (material.texture_format == 4u)
        throw std::runtime_error("PVR-Bumpmapping braucht den Modifier-/Lichtvektorpfad.");
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
        color = decode_16bit_color(vram.read_u16(static_cast<std::uint32_t>(texel_offset)),
                                   material.texture_format);
    } else if (material.texture_format <= 2u) {
        const auto byte_offset = static_cast<std::uint64_t>(material.texture_base) +
                                 level_offset + index * 2u;
        if (byte_offset + 2u > vram.size())
            throw std::out_of_range("PVR-Texturzugriff liegt ausserhalb des VRAM.");
        color = decode_16bit_color(vram.read_u16(static_cast<std::uint32_t>(byte_offset)),
                                   material.texture_format);
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
    if (material.texture_alpha_disabled) color.a = 0xFFu;
    return color;
}

Rgba8 sample_texture(const LinearMemoryDevice& vram,
                     const PvrRegisterFile& registers,
                     const PvrMaterial& material,
                     const float u,
                     const float v) {
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
    const auto format = depth == 0u   ? PvrFramebufferFormat::Argb1555
                        : depth == 1u ? PvrFramebufferFormat::Rgb565
                        : depth == 2u ? PvrFramebufferFormat::Rgb888
                                      : PvrFramebufferFormat::Rgb0888;
    const auto size = registers.read(pvr_register::FramebufferReadSize);
    const auto line_words = static_cast<std::size_t>(size & 0x3FFu) + 1u;
    const auto line_bytes =
        checked_multiply(line_words, 4u, "PVR-Scanout-Zeilenbreite ist zu gross.");
    const auto pixel_bytes = bytes_per_pixel(format);
    if ((line_bytes % pixel_bytes) != 0u) {
        throw std::invalid_argument("PVR-Scanout-Zeilenbreite passt nicht zum Pixelformat.");
    }
    const auto width = line_bytes / pixel_bytes;
    const auto field_height = static_cast<std::size_t>((size >> 10u) & 0x3FFu) + 1u;
    const auto modulus_units = static_cast<std::size_t>((size >> 20u) & 0x3FFu);
    const auto modulus_bytes = checked_multiply(
        modulus_units == 0u ? 0u : modulus_units - 1u,
        4u,
        "PVR-Scanout-Modulus ist zu gross.");
    const auto stride = checked_add(line_bytes, modulus_bytes, "PVR-Scanout-Stride ist zu gross.");
    const auto base =
        static_cast<std::size_t>(registers.read(pvr_register::FramebufferReadSof1) & 0x007FFFFCu);
    const auto second_base =
        static_cast<std::size_t>(registers.read(pvr_register::FramebufferReadSof2) & 0x007FFFFCu);
    const auto interlaced = (registers.read(pvr_register::SpgControl) & 0x10u) != 0u;
    const auto line_double = (control & 2u) != 0u;
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
    auto height = interlaced ? checked_multiply(field_height, 2u, "PVR-Scanout-Hoehe ist zu gross.")
                             : field_height;
    if (line_double)
        height = checked_multiply(height, 2u, "PVR-Line-Doubling-Hoehe ist zu gross.");
    return PvrScanoutDescriptor{static_cast<std::uint32_t>(width),
                                static_cast<std::uint32_t>(height),
                                static_cast<std::uint32_t>(stride),
                                base,
                                second_base,
                                format,
                                line_double,
                                interlaced};
}

void PvrFramebuffer::configure(const std::uint32_t width,
                               const std::uint32_t height,
                               const std::uint32_t stride_bytes,
                               const PvrFramebufferFormat format,
                               const bool line_double,
                               const bool interlaced) {
    if (width == 0u || height == 0u) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    const auto pixel_bytes = bytes_per_pixel(format);
    const auto minimum_stride = checked_multiply(
        static_cast<std::size_t>(width), pixel_bytes, "PVR-Framebuffer-Zeilenbreite ist zu gross.");
    if (static_cast<std::size_t>(stride_bytes) < minimum_stride) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    width_ = width;
    height_ = height;
    stride_ = stride_bytes;
    format_ = format;
    line_double_ = line_double;
    interlaced_ = interlaced;
}

PvrFrame PvrFramebuffer::capture(const std::span<const std::uint8_t> vram,
                                 const std::size_t base_offset,
                                 const std::optional<std::size_t> second_base_offset) {
    if (width_ == 0u || height_ == 0u) {
        throw std::logic_error("PVR-Framebuffer wurde nicht konfiguriert.");
    }
    const auto pixel_count = checked_multiply(static_cast<std::size_t>(width_),
                                              static_cast<std::size_t>(height_),
                                              "PVR-Framebuffer-Pixelzahl ist zu gross.");
    const auto rgba_size =
        checked_multiply(pixel_count, 4u, "PVR-Framebuffer-RGBA-Ausgabe ist zu gross.");
    if (interlaced_ && !second_base_offset)
        throw std::invalid_argument("Interlaced PVR-Scanout braucht beide Feldadressen.");
    const auto source_height = line_double_ ? (static_cast<std::size_t>(height_) + 1u) / 2u
                                            : static_cast<std::size_t>(height_);
    const auto field_rows = interlaced_ ? (source_height + 1u) / 2u : source_height;
    const auto line_bytes = checked_multiply(static_cast<std::size_t>(width_),
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
    if (required > vram.size() || (second_base_offset && second_required > vram.size())) {
        throw std::out_of_range("PVR-Framebuffer liegt ausserhalb des VRAM-Abbilds.");
    }
    PvrFrame frame{width_, height_, std::vector<std::uint8_t>(rgba_size)};
    for (std::uint32_t y = 0u; y < height_; ++y) {
        const auto source_line = line_double_ ? y / 2u : y;
        const auto source_base = interlaced_ && (source_line & 1u) != 0u
                                     ? *second_base_offset
                                     : base_offset;
        const auto source_row = interlaced_ ? source_line / 2u : source_line;
        for (std::uint32_t x = 0u; x < width_; ++x) {
            const auto source =
                source_base + static_cast<std::size_t>(source_row) * stride_ +
                x * bytes_per_pixel(format_);
            const auto destination = (static_cast<std::size_t>(y) * width_ + x) * 4u;
            if (format_ == PvrFramebufferFormat::Rgb888) {
                frame.rgba[destination] = vram[source + 2u];
                frame.rgba[destination + 1u] = vram[source + 1u];
                frame.rgba[destination + 2u] = vram[source];
                frame.rgba[destination + 3u] = 0xFFu;
                continue;
            }
            if (format_ == PvrFramebufferFormat::Rgb0888) {
                frame.rgba[destination] = vram[source + 2u];
                frame.rgba[destination + 1u] = vram[source + 1u];
                frame.rgba[destination + 2u] = vram[source];
                frame.rgba[destination + 3u] = 0xFFu;
                continue;
            }
            const auto pixel = static_cast<std::uint16_t>(vram[source]) |
                               static_cast<std::uint16_t>(vram[source + 1u] << 8u);
            if (format_ == PvrFramebufferFormat::Rgb565) {
                frame.rgba[destination] =
                    expand5(static_cast<std::uint16_t>((pixel >> 11u) & 0x1Fu));
                frame.rgba[destination + 1u] =
                    expand6(static_cast<std::uint16_t>((pixel >> 5u) & 0x3Fu));
                frame.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
                frame.rgba[destination + 3u] = 0xFFu;
            } else {
                frame.rgba[destination] =
                    expand5(static_cast<std::uint16_t>((pixel >> 10u) & 0x1Fu));
                frame.rgba[destination + 1u] =
                    expand5(static_cast<std::uint16_t>((pixel >> 5u) & 0x1Fu));
                frame.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
                frame.rgba[destination + 3u] = (pixel & 0x8000u) != 0u ? 0xFFu : 0u;
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
        throw std::logic_error("Eine PVR-Primitivliste ist bereits offen.");
    }
    if (frame_has_list_ && list_rank(type) < highest_list_rank_) {
        throw std::logic_error("PVR-Primitivlisten wurden in rueckwaertiger Reihenfolge begonnen.");
    }
    current_list_ = type;
    highest_list_rank_ = list_rank(type);
    frame_has_list_ = true;
    current_strip_.clear();
    list_open_ = true;
}

void TileAccelerator::set_material(PvrMaterial material) {
    if (!list_open_)
        throw std::logic_error("PVR-Material ohne offene Primitivliste.");
    if (!current_strip_.empty())
        throw std::logic_error("PVR-Material wechselt innerhalb eines Triangle-Strips.");
    current_material_ = std::move(material);
}

void TileAccelerator::submit_vertex(const PvrVertex& vertex, const bool end_of_strip) {
    if (!list_open_) {
        throw std::logic_error("PVR-Vertex ohne offene Primitivliste.");
    }
    current_strip_.push_back(vertex);
    if (!end_of_strip) {
        return;
    }
    if (current_strip_.size() < 3u) {
        throw std::invalid_argument("Ein PVR-Triangle-Strip braucht mindestens drei Vertices.");
    }
    primitives_.push_back(
        PvrPrimitive{current_list_, std::move(current_strip_), current_material_});
    current_strip_.clear();
}

void TileAccelerator::end_list() {
    if (!list_open_) {
        throw std::logic_error("Keine PVR-Primitivliste ist offen.");
    }
    if (!current_strip_.empty()) {
        throw std::logic_error("PVR-Primitivliste endet mit einem unvollstaendigen Strip.");
    }
    list_open_ = false;
}

PvrTaFrame TileAccelerator::finish_frame() {
    if (list_open_) {
        throw std::logic_error("PVR-Frame endet mit einer offenen Primitivliste.");
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

namespace {

std::uint32_t ta_u32(const std::span<const std::uint8_t> packet, const std::size_t offset) {
    if (offset > packet.size() || packet.size() - offset < 4u)
        throw std::out_of_range("TA-Paket ist abgeschnitten.");
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
        throw std::invalid_argument("TA-Objektliste wird vom allgemeinen Polygonpfad abgewiesen.");
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
            throw std::invalid_argument("TA-Floatfarbe ist nicht endlich.");
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
    if (packet.size() != 32u) throw std::invalid_argument("TA-FIFO erwartet 32-Byte-Parameter.");
    ++metrics_.packets;
    if (pending_intensity_header_) {
        active_header_argb_ = decode_ta_float_color(packet, 0u);
        active_header_oargb_ = decode_ta_float_color(packet, 16u);
        intensity_face_color_valid_ = true;
        pending_intensity_header_ = false;
        return;
    }
    if (pending_modifier_vertex_packet_) {
        pending_modifier_vertex_packet_ = false;
        modifier_volumes_present_ = true;
        ++metrics_.vertices;
        return;
    }
    if (pending_extended_vertex_) {
        auto vertex = *pending_extended_vertex_;
        vertex.argb = decode_ta_float_color(packet, 0u);
        vertex.oargb = decode_ta_float_color(packet, 16u);
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
        if (!accelerator_.list_open())
            throw std::logic_error("TA-End-of-List ohne offene Objektliste.");
        accelerator_.end_list();
        ++metrics_.list_completions;
        if (list_observer_) list_observer_(active_list_);
        return;
    }
    if (parameter_type == 1u) {
        const auto start_x = ta_u32(packet, 16u);
        const auto start_y = ta_u32(packet, 20u);
        const auto end_x = ta_u32(packet, 24u);
        const auto end_y = ta_u32(packet, 28u);
        if (start_x > end_x || start_y > end_y || end_x >= 64u || end_y >= 32u)
            throw std::invalid_argument("TA-Userclip liegt ausserhalb des 32-Pixel-Tilebereichs.");
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
            throw std::logic_error("TA-Objektlistenauswahl wechselt eine offene Liste.");
        }
        return;
    }
    if (parameter_type == 4u) {
        const auto selected = decode_list_type(pcw);
        if (!accelerator_.list_open()) {
            accelerator_.begin_list(selected);
        } else if (selected != active_list_) {
            throw std::logic_error("TA-Polygonheader wechselt eine offene Objektliste.");
        }
        active_list_ = selected;
        active_uv16_ = (pcw & 0x1u) != 0u;
        active_textured_ = (pcw & 0x8u) != 0u;
        active_color_type_ = static_cast<std::uint8_t>((pcw >> 4u) & 3u);
        active_sprite_ = false;
        const auto mode1 = ta_u32(packet, 4u);
        const auto mode2 = ta_u32(packet, 8u);
        const auto mode3 = ta_u32(packet, 12u);
        active_material_ = {};
        active_material_.gouraud = (pcw & 2u) != 0u;
        active_material_.textured = active_textured_;
        active_material_.user_clip_mode = static_cast<std::uint8_t>((pcw >> 16u) & 3u);
        active_material_.user_clip_start_x = user_clip_start_x_;
        active_material_.user_clip_start_y = user_clip_start_y_;
        active_material_.user_clip_end_x = user_clip_end_x_;
        active_material_.user_clip_end_y = user_clip_end_y_;
        active_material_.depth_compare = static_cast<std::uint8_t>((mode1 >> 29u) & 7u);
        active_material_.culling = static_cast<std::uint8_t>((mode1 >> 27u) & 3u);
        active_material_.depth_write = (mode1 & 0x04000000u) == 0u;
        active_material_.texture_height = 8u << (mode2 & 7u);
        active_material_.texture_width = 8u << ((mode2 >> 3u) & 7u);
        active_material_.texture_shading = static_cast<std::uint8_t>((mode2 >> 6u) & 3u);
        active_material_.texture_mipmap_bias = static_cast<std::uint8_t>((mode2 >> 8u) & 0xFu);
        active_material_.texture_supersampling = (mode2 & 0x00001000u) != 0u;
        active_material_.texture_filter = static_cast<std::uint8_t>((mode2 >> 13u) & 3u);
        active_material_.clamp_v = (mode2 & 0x00008000u) != 0u;
        active_material_.clamp_u = (mode2 & 0x00010000u) != 0u;
        active_material_.flip_v = (mode2 & 0x00020000u) != 0u;
        active_material_.flip_u = (mode2 & 0x00040000u) != 0u;
        active_material_.texture_alpha_disabled = (mode2 & 0x00080000u) != 0u;
        active_material_.vertex_alpha_enabled = (mode2 & 0x00100000u) != 0u;
        active_material_.color_clamp_enabled = (mode2 & 0x00200000u) != 0u;
        active_material_.offset_color_enabled = (pcw & 0x4u) != 0u;
        active_material_.fog_mode = static_cast<std::uint8_t>((mode2 >> 22u) & 3u);
        active_material_.blend_destination_accumulation = (mode2 & 0x01000000u) != 0u;
        active_material_.blend_source_accumulation = (mode2 & 0x02000000u) != 0u;
        active_material_.destination_blend = static_cast<std::uint8_t>((mode2 >> 26u) & 7u);
        active_material_.source_blend = static_cast<std::uint8_t>((mode2 >> 29u) & 7u);
        active_material_.texture_format = static_cast<std::uint8_t>((mode3 >> 27u) & 7u);
        active_material_.texture_base =
            (mode3 & (active_material_.texture_format == 5u ? 0x001FFFFFu : 0x01FFFFFFu))
            << 3u;
        active_material_.texture_x32_stride = active_material_.texture_format < 5u &&
                                              (mode3 & 0x02000000u) != 0u;
        active_material_.texture_twiddled = active_material_.texture_format >= 5u ||
                                            (mode3 & 0x04000000u) == 0u;
        if (active_material_.texture_format == 5u)
            active_material_.palette_bank = static_cast<std::uint8_t>((mode3 >> 21u) & 0x3Fu);
        else if (active_material_.texture_format == 6u)
            active_material_.palette_bank = static_cast<std::uint8_t>((mode3 >> 25u) & 3u);
        active_material_.texture_vq = (mode3 & 0x40000000u) != 0u;
        active_material_.texture_mipmapped = (mode3 & 0x80000000u) != 0u;
        if (active_color_type_ == 2u) {
            if (active_material_.offset_color_enabled) {
                pending_intensity_header_ = true;
            } else {
                active_header_argb_ = decode_ta_float_color(packet, 16u);
                active_header_oargb_ = 0u;
                intensity_face_color_valid_ = true;
            }
        } else if (active_color_type_ == 3u && !intensity_face_color_valid_) {
            throw std::logic_error(
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
            throw std::logic_error("TA-Spriteheader wechselt eine offene Objektliste.");
        }
        active_list_ = selected;
        active_textured_ = (pcw & 0x8u) != 0u;
        active_sprite_ = true;
        active_header_argb_ = ta_u32(packet, 16u);
        active_header_oargb_ = ta_u32(packet, 20u);
        const auto mode1 = ta_u32(packet, 4u);
        const auto mode2 = ta_u32(packet, 8u);
        const auto mode3 = ta_u32(packet, 12u);
        active_material_ = {};
        active_material_.gouraud = false;
        active_material_.textured = active_textured_;
        active_material_.user_clip_mode = static_cast<std::uint8_t>((pcw >> 16u) & 3u);
        active_material_.user_clip_start_x = user_clip_start_x_;
        active_material_.user_clip_start_y = user_clip_start_y_;
        active_material_.user_clip_end_x = user_clip_end_x_;
        active_material_.user_clip_end_y = user_clip_end_y_;
        active_material_.depth_compare = static_cast<std::uint8_t>((mode1 >> 29u) & 7u);
        active_material_.depth_write = (mode1 & 0x04000000u) == 0u;
        active_material_.texture_height = 8u << (mode2 & 7u);
        active_material_.texture_width = 8u << ((mode2 >> 3u) & 7u);
        active_material_.texture_shading = static_cast<std::uint8_t>((mode2 >> 6u) & 3u);
        active_material_.texture_mipmap_bias = static_cast<std::uint8_t>((mode2 >> 8u) & 0xFu);
        active_material_.texture_supersampling = (mode2 & 0x00001000u) != 0u;
        active_material_.texture_filter = static_cast<std::uint8_t>((mode2 >> 13u) & 3u);
        active_material_.clamp_v = (mode2 & 0x00008000u) != 0u;
        active_material_.clamp_u = (mode2 & 0x00010000u) != 0u;
        active_material_.flip_v = (mode2 & 0x00020000u) != 0u;
        active_material_.flip_u = (mode2 & 0x00040000u) != 0u;
        active_material_.texture_alpha_disabled = (mode2 & 0x00080000u) != 0u;
        active_material_.vertex_alpha_enabled = (mode2 & 0x00100000u) != 0u;
        active_material_.color_clamp_enabled = (mode2 & 0x00200000u) != 0u;
        active_material_.offset_color_enabled = (pcw & 0x4u) != 0u;
        active_material_.fog_mode = static_cast<std::uint8_t>((mode2 >> 22u) & 3u);
        active_material_.blend_destination_accumulation = (mode2 & 0x01000000u) != 0u;
        active_material_.blend_source_accumulation = (mode2 & 0x02000000u) != 0u;
        active_material_.destination_blend = static_cast<std::uint8_t>((mode2 >> 26u) & 7u);
        active_material_.source_blend = static_cast<std::uint8_t>((mode2 >> 29u) & 7u);
        active_material_.texture_format = static_cast<std::uint8_t>((mode3 >> 27u) & 7u);
        active_material_.texture_base =
            (mode3 & (active_material_.texture_format == 5u ? 0x001FFFFFu : 0x01FFFFFFu))
            << 3u;
        active_material_.texture_x32_stride = active_material_.texture_format < 5u &&
                                              (mode3 & 0x02000000u) != 0u;
        active_material_.texture_twiddled = active_material_.texture_format >= 5u ||
                                            (mode3 & 0x04000000u) == 0u;
        if (active_material_.texture_format == 5u)
            active_material_.palette_bank = static_cast<std::uint8_t>((mode3 >> 21u) & 0x3Fu);
        else if (active_material_.texture_format == 6u)
            active_material_.palette_bank = static_cast<std::uint8_t>((mode3 >> 25u) & 3u);
        active_material_.texture_vq = (mode3 & 0x40000000u) != 0u;
        active_material_.texture_mipmapped = (mode3 & 0x80000000u) != 0u;
        accelerator_.set_material(active_material_);
        ++metrics_.polygon_headers;
        return;
    }
    if (parameter_type == 7u) {
        if (active_list_ == PvrListType::OpaqueModifier ||
            active_list_ == PvrListType::TranslucentModifier) {
            pending_modifier_vertex_packet_ = true;
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
        std::size_t color_offset = 24u;
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
            if (color_offset + 4u < packet.size())
                vertex.oargb = ta_u32(packet, color_offset + 4u);
        }
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z) || !std::isfinite(vertex.u) || !std::isfinite(vertex.v))
            throw std::invalid_argument("TA-Vertex besitzt nicht-endliche Koordinaten.");
        accelerator_.submit_vertex(vertex, (pcw & 0x10000000u) != 0u);
        ++metrics_.vertices;
        return;
    }
    throw std::runtime_error("TA-Parametertyp ist noch nicht in den Polygonpfad integrierbar.");
}

PvrTaFrame PvrTaFifo::finish_frame() {
    if (pending_sprite_vertex_ || pending_extended_vertex_ || pending_intensity_header_ ||
        pending_modifier_vertex_packet_)
        throw std::logic_error("TA-Frame endet innerhalb eines 64-Byte-Parameters.");
    auto frame = accelerator_.finish_frame();
    frame.modifier_volumes_present = modifier_volumes_present_;
    modifier_volumes_present_ = false;
    ++metrics_.frames;
    return frame;
}

const PvrTaMetrics& PvrTaFifo::metrics() const noexcept {
    return metrics_;
}

void PvrTaFifo::continue_list() {
    if (accelerator_.list_open() || pending_sprite_vertex_ || pending_extended_vertex_ ||
        pending_intensity_header_ || pending_modifier_vertex_packet_)
        throw std::logic_error("TA-Listenfortsetzung beginnt innerhalb eines Parameters oder einer Liste.");
    active_list_ = PvrListType::Opaque;
    active_textured_ = false;
    active_uv16_ = false;
    active_color_type_ = 0u;
    active_sprite_ = false;
    active_header_argb_ = 0xFFFFFFFFu;
    active_header_oargb_ = 0u;
    intensity_face_color_valid_ = false;
    active_material_ = {};
    user_clip_start_x_ = 0u;
    user_clip_start_y_ = 0u;
    user_clip_end_x_ = 0u;
    user_clip_end_y_ = 0u;
    pending_extended_end_of_strip_ = false;
    ++metrics_.continuations;
}

void PvrTaFifo::reset() noexcept {
    accelerator_ = TileAccelerator{};
    active_list_ = PvrListType::Opaque;
    active_textured_ = false;
    active_uv16_ = false;
    active_color_type_ = 0u;
    active_sprite_ = false;
    active_header_argb_ = 0xFFFFFFFFu;
    active_header_oargb_ = 0u;
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
    modifier_volumes_present_ = false;
    pending_modifier_vertex_packet_ = false;
    metrics_ = {};
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
        throw std::runtime_error(
            "TA-FIFO-Schreibfolge verlaesst ein unvollstaendiges 32-Byte-Parameterpaket.");
    }
    const auto bit = std::uint32_t{1u} << byte;
    if ((written_mask_ & bit) != 0u)
        throw std::runtime_error("TA-FIFO-Parameterbyte wurde vor Completion doppelt geschrieben.");
    packet_[byte] = value;
    written_mask_ |= bit;
    if (written_mask_ == std::numeric_limits<std::uint32_t>::max()) {
        fifo_->submit(packet_);
        if (registers_) registers_->record_ta_packet(32u);
        written_mask_ = 0u;
        packet_active_ = false;
    }
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
            vram_->write_u16(static_cast<std::uint32_t>(output),
                             static_cast<std::uint16_t>(y_sample(x, y) << 8u | u));
            vram_->write_u16(static_cast<std::uint32_t>(output + 2u),
                             static_cast<std::uint16_t>(y_sample(x + 1u, y) << 8u | v));
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

void PvrSoftwareRenderer::render(const PvrTaFrame& frame,
                                 const PvrRegisterFile& registers,
                                 LinearMemoryDevice& vram) {
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
    const auto base = registers.read(pvr_register::FramebufferWriteSof1) & 0x007FFFFCu;
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
    std::vector<float> depth(static_cast<std::size_t>(width) * height,
                             -std::numeric_limits<float>::infinity());

    const auto edge = [](const PvrVertex& a, const PvrVertex& b, const float x, const float y) {
        return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    };
    const auto background_config = registers.read(pvr_register::BackgroundPlaneConfig);
    if ((background_config & 0x01000000u) != 0u) {
        const auto parameter_base = registers.read(pvr_register::ParameterBase) & 0x007FFFFFu;
        const auto background_offset = (background_config & 0x00FFFFFFu) >> 1u;
        const auto background_address =
            static_cast<std::uint64_t>(parameter_base) + background_offset;
        if (background_address + 60u > vram.size())
            throw std::out_of_range("PVR-Hintergrundebene liegt ausserhalb des VRAM.");
        const auto read_vertex = [&](const std::uint32_t offset) {
            return PvrVertex{std::bit_cast<float>(vram.read_u32(
                                 static_cast<std::uint32_t>(background_address + offset))),
                             std::bit_cast<float>(vram.read_u32(
                                 static_cast<std::uint32_t>(background_address + offset + 4u))),
                             std::bit_cast<float>(vram.read_u32(
                                 static_cast<std::uint32_t>(background_address + offset + 8u))),
                             0.0f,
                             0.0f,
                             vram.read_u32(
                                 static_cast<std::uint32_t>(background_address + offset + 12u))};
        };
        const auto a = read_vertex(12u);
        const auto b = read_vertex(28u);
        const auto c = read_vertex(44u);
        const auto plane_area = edge(a, b, c.x, c.y);
        if (!std::isfinite(plane_area) || !std::isfinite(a.z) || !std::isfinite(b.z) ||
            !std::isfinite(c.z))
            throw std::invalid_argument("PVR-Hintergrundebene besitzt ungueltige Koordinaten.");
        const auto channel = [](const std::uint32_t argb, const unsigned shift) {
            return static_cast<float>((argb >> shift) & 0xFFu);
        };
        for (std::uint32_t y = minimum_clip_y; y <= maximum_clip_y; ++y) {
            for (std::uint32_t x = minimum_clip_x; x <= maximum_clip_x; ++x) {
                const auto px = static_cast<float>(x) + 0.5f;
                const auto py = static_cast<float>(y) + 0.5f;
                const auto w0 = plane_area == 0.0f ? 1.0f : edge(b, c, px, py) / plane_area;
                const auto w1 = plane_area == 0.0f ? 0.0f : edge(c, a, px, py) / plane_area;
                const auto w2 = plane_area == 0.0f ? 0.0f : edge(a, b, px, py) / plane_area;
                const auto interpolate_channel = [&](const unsigned shift) {
                    return static_cast<std::uint8_t>(std::lround(std::clamp(
                        w0 * channel(a.argb, shift) + w1 * channel(b.argb, shift) +
                            w2 * channel(c.argb, shift),
                        0.0f,
                        255.0f)));
                };
                const Rgba8 color{interpolate_channel(16u),
                                  interpolate_channel(8u),
                                  interpolate_channel(0u),
                                  interpolate_channel(24u)};
                const auto offset = static_cast<std::uint32_t>(
                    base + static_cast<std::uint64_t>(y) * stride +
                    static_cast<std::uint64_t>(x) * pixel_bytes);
                write_render_pixel(vram, offset, pack_mode, color.a, color.r, color.g, color.b);
                depth[static_cast<std::size_t>(y) * width + x] =
                    w0 * a.z + w1 * b.z + w2 * c.z;
            }
        }
    }
    for (const auto& primitive : frame.primitives) {
        auto texture_material = primitive.material;
        if (texture_material.texture_filter >= 2u ||
            texture_material.blend_destination_accumulation ||
            texture_material.blend_source_accumulation)
            throw std::runtime_error(
                "PVR-Trilinear-/Akkumulationspufferpfad ist noch nicht implementiert.");
        if (texture_material.texture_supersampling)
            throw std::runtime_error("PVR-Textur-Supersampling ist noch nicht implementiert.");
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
                    if (primitive.material.textured) {
                        if (std::fabs(fragment_depth) <= std::numeric_limits<float>::epsilon())
                            throw std::runtime_error(
                                "PVR-Perspektivinterpolation besitzt eine Nulltiefe.");
                        const auto u = interpolate_float(a->u * a->z, b->u * b->z,
                                                         c->u * c->z) /
                                       fragment_depth;
                        const auto v = interpolate_float(a->v * a->z, b->v * b->z,
                                                         c->v * c->z) /
                                       fragment_depth;
                        source = shade_texture(sample_texture(vram, registers, texture_material, u, v),
                                               source,
                                               primitive.material.texture_shading);
                    }
                    if (primitive.material.offset_color_enabled)
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
                    if (primitive.list == PvrListType::PunchThrough && source.a < 0x80u)
                        continue;
                    const auto offset = static_cast<std::uint32_t>(
                        base + static_cast<std::uint64_t>(y) * stride +
                        static_cast<std::uint64_t>(x) * pixel_bytes);
                    if (primitive.list == PvrListType::Translucent)
                        source = blend_color(
                            source, read_render_pixel(vram, offset, pack_mode), primitive.material);
                    write_render_pixel(vram,
                                       offset,
                                       pack_mode,
                                       source.a,
                                       source.r,
                                       source.g,
                                       source.b);
                    if (primitive.material.depth_write) depth[pixel_index] = fragment_depth;
                    ++metrics_.pixels;
                }
            }
            ++metrics_.triangles;
        }
    }
    ++metrics_.frames;
}

const PvrSoftwareRenderMetrics& PvrSoftwareRenderer::metrics() const noexcept {
    return metrics_;
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
