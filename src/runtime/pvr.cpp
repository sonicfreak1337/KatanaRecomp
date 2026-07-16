#include "katana/runtime/pvr.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {

PvrRegisterFile::PvrRegisterFile(std::function<void()> render_observer)
    : render_observer_(std::move(render_observer)) {}

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
    return registers_[index(offset)];
}

void PvrRegisterFile::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == pvr_register::Id || offset == pvr_register::Revision) {
        throw std::runtime_error("PVR-ID und Revision sind read-only.");
    }
    if (offset == pvr_register::SoftReset) {
        if ((value & 0x7u) != 0u) {
            reset();
        }
        return;
    }
    if (offset == pvr_register::StartRender) {
        ++render_requests_;
        if (render_observer_) render_observer_();
        return;
    }
    registers_[index(offset)] = value;
}

void PvrRegisterFile::reset() noexcept {
    registers_.fill(0u);
    ++resets_;
}

std::uint64_t PvrRegisterFile::render_request_count() const noexcept {
    return render_requests_;
}
std::uint64_t PvrRegisterFile::reset_count() const noexcept {
    return resets_;
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
} // namespace

void PvrFramebuffer::configure(const std::uint32_t width,
                               const std::uint32_t height,
                               const std::uint32_t stride_bytes,
                               const PvrFramebufferFormat format) {
    if (width == 0u || height == 0u) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    const std::size_t bytes_per_pixel = format == PvrFramebufferFormat::Rgb888 ? 3u : 2u;
    const auto minimum_stride = checked_multiply(static_cast<std::size_t>(width),
                                                 bytes_per_pixel,
                                                 "PVR-Framebuffer-Zeilenbreite ist zu gross.");
    if (static_cast<std::size_t>(stride_bytes) < minimum_stride) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    width_ = width;
    height_ = height;
    stride_ = stride_bytes;
    format_ = format;
}

PvrFrame PvrFramebuffer::capture(const std::span<const std::uint8_t> vram,
                                 const std::size_t base_offset) {
    if (width_ == 0u || height_ == 0u) {
        throw std::logic_error("PVR-Framebuffer wurde nicht konfiguriert.");
    }
    const auto pixel_count = checked_multiply(static_cast<std::size_t>(width_),
                                              static_cast<std::size_t>(height_),
                                              "PVR-Framebuffer-Pixelzahl ist zu gross.");
    const auto rgba_size =
        checked_multiply(pixel_count, 4u, "PVR-Framebuffer-RGBA-Ausgabe ist zu gross.");
    const auto image_bytes = checked_multiply(static_cast<std::size_t>(stride_),
                                              static_cast<std::size_t>(height_),
                                              "PVR-Framebuffer-VRAM-Ausdehnung ist zu gross.");
    const auto required =
        checked_add(base_offset, image_bytes, "PVR-Framebuffer-VRAM-Endadresse laeuft ueber.");
    if (required > vram.size()) {
        throw std::out_of_range("PVR-Framebuffer liegt ausserhalb des VRAM-Abbilds.");
    }
    PvrFrame frame{width_, height_, std::vector<std::uint8_t>(rgba_size)};
    for (std::uint32_t y = 0u; y < height_; ++y) {
        for (std::uint32_t x = 0u; x < width_; ++x) {
            const auto source = base_offset + static_cast<std::size_t>(y) * stride_ +
                                x * (format_ == PvrFramebufferFormat::Rgb888 ? 3u : 2u);
            const auto destination = (static_cast<std::size_t>(y) * width_ + x) * 4u;
            if (format_ == PvrFramebufferFormat::Rgb888) {
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
    primitives_.push_back(PvrPrimitive{current_list_, std::move(current_strip_)});
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
    highest_list_rank_ = 0u;
    frame_has_list_ = false;
    return result;
}

bool TileAccelerator::list_open() const noexcept {
    return list_open_;
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
                                                   std::function<void()> render_observer) {
    auto registers = std::make_shared<PvrRegisterFile>(std::move(render_observer));
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
