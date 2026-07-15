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
    const auto red = decode_pvr_texture({std::vector<std::uint8_t>{0x00u, 0xF8u}}, 1u, 1u,
        PvrTextureFormat::Rgb565);
    require(red.rgba == std::vector<std::uint8_t>({0xFFu, 0u, 0u, 0xFFu}),
        "RGB565-Textur wird nicht korrekt dekodiert.");
    const auto transparent_blue = decode_pvr_texture({std::vector<std::uint8_t>{0x1Fu, 0x00u}}, 1u, 1u,
        PvrTextureFormat::Argb1555);
    require(transparent_blue.rgba == std::vector<std::uint8_t>({0u, 0u, 0xFFu, 0u}),
        "ARGB1555-Textur verliert Farbe oder Alpha.");
    const auto opaque_blue = decode_pvr_texture({std::vector<std::uint8_t>{0x0Fu, 0xF0u}}, 1u, 1u,
        PvrTextureFormat::Argb4444);
    require(opaque_blue.rgba == std::vector<std::uint8_t>({0u, 0u, 0xFFu, 0xFFu}),
        "ARGB4444-Textur wird nicht korrekt dekodiert.");
    require(throws<std::invalid_argument>([] {
        static_cast<void>(decode_pvr_texture({}, 0u, 1u, PvrTextureFormat::Rgb565));
    }), "Leere PVR-Texturgeometrie wird akzeptiert.");
    require(throws<std::out_of_range>([] {
        static_cast<void>(decode_pvr_texture({std::vector<std::uint8_t>{0u}}, 1u, 1u,
            PvrTextureFormat::Rgb565));
    }), "Abgeschnittene PVR-Texturdaten werden akzeptiert.");

    PvrTaFrame frame;
    frame.primitives.push_back(PvrPrimitive{PvrListType::Opaque, {
        PvrVertex{}, PvrVertex{}, PvrVertex{}
    }});
    RecordingPvrRenderBackend backend;
    const std::vector<PvrTexture> textures = {red, opaque_blue};
    backend.render(frame, textures);
    require(backend.submitted_frames() == 1u && backend.last_frame().primitives.size() == 1u &&
        backend.last_textures().size() == 2u && backend.last_textures()[1].rgba[2] == 0xFFu,
        "Render-Backend uebergibt Frame oder Texturen nicht deterministisch.");

    std::cout << "KR-2804 Texturformate und Render-Backend erfolgreich.\n";
}
