#include "katana/runtime/pvr.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <stdexcept>
#include <string>

namespace katana::runtime {

std::size_t PvrRegisterFile::index(const std::uint32_t offset) {
    if (offset >= pvr_register_size || (offset & 3u) != 0u) {
        throw std::out_of_range("Ungueltiger oder nicht ausgerichteter PVR-Registeroffset.");
    }
    return offset / 4u;
}

std::uint32_t PvrRegisterFile::read(const std::uint32_t offset) const {
    if (offset == pvr_register::Id) { return pvr_id; }
    if (offset == pvr_register::Revision) { return pvr_revision; }
    return registers_[index(offset)];
}

void PvrRegisterFile::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == pvr_register::Id || offset == pvr_register::Revision) {
        throw std::runtime_error("PVR-ID und Revision sind read-only.");
    }
    if (offset == pvr_register::SoftReset) {
        if ((value & 0x7u) != 0u) { reset(); }
        return;
    }
    if (offset == pvr_register::StartRender) {
        ++render_requests_;
        return;
    }
    registers_[index(offset)] = value;
}

void PvrRegisterFile::reset() noexcept {
    registers_.fill(0u);
    ++resets_;
}

std::uint64_t PvrRegisterFile::render_request_count() const noexcept { return render_requests_; }
std::uint64_t PvrRegisterFile::reset_count() const noexcept { return resets_; }

namespace {
std::uint8_t expand5(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 3u) | (value >> 2u));
}
std::uint8_t expand6(const std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 2u) | (value >> 4u));
}
}

void PvrFramebuffer::configure(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t stride_bytes,
    const PvrFramebufferFormat format
) {
    const std::uint32_t bytes_per_pixel = format == PvrFramebufferFormat::Rgb888 ? 3u : 2u;
    if (width == 0u || height == 0u || stride_bytes < width * bytes_per_pixel) {
        throw std::invalid_argument("Ungueltige PVR-Framebuffer-Geometrie oder Stride.");
    }
    width_ = width;
    height_ = height;
    stride_ = stride_bytes;
    format_ = format;
}

PvrFrame PvrFramebuffer::capture(
    const std::span<const std::uint8_t> vram,
    const std::size_t base_offset
) {
    if (width_ == 0u || height_ == 0u) {
        throw std::logic_error("PVR-Framebuffer wurde nicht konfiguriert.");
    }
    const auto required = base_offset + static_cast<std::size_t>(stride_) * height_;
    if (required > vram.size()) {
        throw std::out_of_range("PVR-Framebuffer liegt ausserhalb des VRAM-Abbilds.");
    }
    PvrFrame frame{width_, height_, std::vector<std::uint8_t>(static_cast<std::size_t>(width_) * height_ * 4u)};
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
                frame.rgba[destination] = expand5(static_cast<std::uint16_t>((pixel >> 11u) & 0x1Fu));
                frame.rgba[destination + 1u] = expand6(static_cast<std::uint16_t>((pixel >> 5u) & 0x3Fu));
                frame.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
                frame.rgba[destination + 3u] = 0xFFu;
            } else {
                frame.rgba[destination] = expand5(static_cast<std::uint16_t>((pixel >> 10u) & 0x1Fu));
                frame.rgba[destination + 1u] = expand5(static_cast<std::uint16_t>((pixel >> 5u) & 0x1Fu));
                frame.rgba[destination + 2u] = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
                frame.rgba[destination + 3u] = (pixel & 0x8000u) != 0u ? 0xFFu : 0u;
            }
        }
    }
    ++presented_frames_;
    return frame;
}

std::uint64_t PvrFramebuffer::presented_frames() const noexcept { return presented_frames_; }

std::shared_ptr<PvrRegisterFile> map_pvr_registers(Memory& memory) {
    auto registers = std::make_shared<PvrRegisterFile>();
    auto device = std::make_shared<MmioMemoryDevice>(
        pvr_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            return registers->read(offset);
        },
        [registers](const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            registers->write(offset, value);
        }
    );
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + pvr_register_physical_base;
        memory.map_region("dreamcast-pvr-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime
