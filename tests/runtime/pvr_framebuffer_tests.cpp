#include "katana/runtime/pvr.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

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
    require(framebuffer.presented_frames() == 1u, "Frame-Synchronisationszaehler schreitet nicht fort.");

    PvrFramebuffer alpha;
    alpha.configure(1u, 1u, 2u, PvrFramebufferFormat::Argb1555);
    const auto transparent = alpha.capture(std::vector<std::uint8_t>{0x1Fu, 0x00u});
    require(transparent.rgba[2] == 0xFFu && transparent.rgba[3] == 0u,
        "ARGB1555-Alpha oder Blau ist falsch.");
    require(throws<std::invalid_argument>([] { PvrFramebuffer value; value.configure(2u, 1u, 3u, PvrFramebufferFormat::Rgb565); }),
        "Zu kleiner Framebuffer-Stride wird akzeptiert.");
    require(throws<std::out_of_range>([&] { static_cast<void>(framebuffer.capture(std::vector<std::uint8_t>(5u))); }),
        "Framebuffer ausserhalb des VRAM wird akzeptiert.");

    std::cout << "KR-2802 Framebuffer-Ausgabe erfolgreich.\n";
}
