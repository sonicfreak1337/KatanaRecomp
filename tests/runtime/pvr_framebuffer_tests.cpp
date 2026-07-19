#include "katana/runtime/pvr.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    PvrFramebuffer framebuffer;
    framebuffer.configure(2u, 1u, 6u, PvrFramebufferFormat::Rgb565);
    const std::vector<std::uint8_t> vram = {0x00u, 0xF8u, 0xE0u, 0x07u, 0xAAu, 0xAAu};
    const auto frame = framebuffer.capture(vram);
    require(frame.width == 2u && frame.height == 1u && frame.rgba.size() == 8u,
            "Framebuffer-Ausgabe besitzt falsche Geometrie.");
    require(frame.rgba[0] == 0xFFu && frame.rgba[1] == 0u && frame.rgba[2] == 0u &&
                frame.rgba[4] == 0u && frame.rgba[5] == 0xFFu && frame.rgba[6] == 0u,
            "RGB565 wird nicht korrekt in RGBA dekodiert.");
    require(framebuffer.presented_frames() == 1u,
            "Frame-Synchronisationszaehler schreitet nicht fort.");

    PvrFramebuffer alpha;
    alpha.configure(1u, 1u, 2u, PvrFramebufferFormat::Argb1555);
    const auto transparent = alpha.capture(std::vector<std::uint8_t>{0x1Fu, 0x00u});
    require(transparent.rgba[2] == 0xFFu && transparent.rgba[3] == 0u,
            "ARGB1555-Alpha oder Blau ist falsch.");

    PvrFramebuffer rgb0888;
    rgb0888.configure(1u, 1u, 4u, PvrFramebufferFormat::Rgb0888);
    const auto rgb0888_frame =
        rgb0888.capture(std::array<std::uint8_t, 4u>{0x11u, 0x22u, 0x33u, 0x00u});
    require(rgb0888_frame.rgba ==
                std::vector<std::uint8_t>({0x33u, 0x22u, 0x11u, 0xFFu}),
            "RGB0888 wird nicht korrekt in RGBA dekodiert.");

    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler);
    require(!decode_pvr_scanout(registers, dreamcast_vram_size).has_value(),
            "Deaktivierter PVR-Scanout wird als aktiv gemeldet.");
    registers.write(pvr_register::FramebufferReadControl, 0x5u);
    registers.write(pvr_register::FramebufferReadSof1, 0x001000u);
    registers.write(pvr_register::FramebufferReadSize,
                    (1u << 20u) | (479u << 10u) | 319u);
    const auto scanout = decode_pvr_scanout(registers, dreamcast_vram_size);
    require(scanout.has_value() && scanout->width == 640u && scanout->height == 480u &&
                scanout->stride_bytes == 1284u && scanout->base_offset == 0x1000u &&
                scanout->format == PvrFramebufferFormat::Rgb565,
            "PVR-Scanout-Register werden nicht korrekt dekodiert.");
    require(throws<std::invalid_argument>([] {
                PvrFramebuffer value;
                value.configure(2u, 1u, 3u, PvrFramebufferFormat::Rgb565);
            }),
            "Zu kleiner Framebuffer-Stride wird akzeptiert.");
    require(throws<std::out_of_range>(
                [&] { static_cast<void>(framebuffer.capture(std::vector<std::uint8_t>(5u))); }),
            "Framebuffer ausserhalb des VRAM wird akzeptiert.");
    require(throws<std::invalid_argument>([] {
                PvrFramebuffer value;
                value.configure(0x80000000u, 1u, 0u, PvrFramebufferFormat::Rgb565);
            }),
            "Ueberlaufende uint32_t-Zeilenbreite wird als gueltiger Stride akzeptiert.");
    require(throws<std::out_of_range>([] {
                PvrFramebuffer value;
                value.configure(
                    0x40000001u, 0xFFFFFFFFu, 0x80000002u, PvrFramebufferFormat::Rgb565);
                static_cast<void>(value.capture({}));
            }),
            "Ueberlaufende RGBA-Allokationsgroesse wird akzeptiert.");
    require(throws<std::out_of_range>([] {
                PvrFramebuffer value;
                value.configure(1u, 1u, 2u, PvrFramebufferFormat::Rgb565);
                static_cast<void>(value.capture({}, std::numeric_limits<std::size_t>::max() - 1u));
            }),
            "Ueberlaufende VRAM-Endadresse wird akzeptiert.");

    std::cout << "KR-2802 Framebuffer-Ausgabe erfolgreich.\n";
}
