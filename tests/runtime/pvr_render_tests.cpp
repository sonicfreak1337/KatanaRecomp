#include "katana/runtime/pvr.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>
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
    const auto red = decode_pvr_texture(
        {std::vector<std::uint8_t>{0x00u, 0xF8u}}, 1u, 1u, PvrTextureFormat::Rgb565);
    require(red.rgba == std::vector<std::uint8_t>({0xFFu, 0u, 0u, 0xFFu}),
            "RGB565-Textur wird nicht korrekt dekodiert.");
    const auto transparent_blue = decode_pvr_texture(
        {std::vector<std::uint8_t>{0x1Fu, 0x00u}}, 1u, 1u, PvrTextureFormat::Argb1555);
    require(transparent_blue.rgba == std::vector<std::uint8_t>({0u, 0u, 0xFFu, 0u}),
            "ARGB1555-Textur verliert Farbe oder Alpha.");
    const auto opaque_blue = decode_pvr_texture(
        {std::vector<std::uint8_t>{0x0Fu, 0xF0u}}, 1u, 1u, PvrTextureFormat::Argb4444);
    require(opaque_blue.rgba == std::vector<std::uint8_t>({0u, 0u, 0xFFu, 0xFFu}),
            "ARGB4444-Textur wird nicht korrekt dekodiert.");
    require(throws<std::invalid_argument>([] {
                static_cast<void>(decode_pvr_texture({}, 0u, 1u, PvrTextureFormat::Rgb565));
            }),
            "Leere PVR-Texturgeometrie wird akzeptiert.");
    require(throws<std::out_of_range>([] {
                static_cast<void>(decode_pvr_texture(
                    {std::vector<std::uint8_t>{0u}}, 1u, 1u, PvrTextureFormat::Rgb565));
            }),
            "Abgeschnittene PVR-Texturdaten werden akzeptiert.");

    PvrTaFrame frame;
    frame.primitives.push_back(
        PvrPrimitive{PvrListType::Opaque, {PvrVertex{}, PvrVertex{}, PvrVertex{}}});
    RecordingPvrRenderBackend backend;
    const std::vector<PvrTexture> textures = {red, opaque_blue};
    backend.render(frame, textures);
    require(backend.submitted_frames() == 1u && backend.last_frame().primitives.size() == 1u &&
                backend.last_textures().size() == 2u && backend.last_textures()[1].rgba[2] == 0xFFu,
            "Render-Backend uebergibt Frame oder Texturen nicht deterministisch.");

    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler);
    LinearMemoryDevice vram(8u << 20u);
    registers.write(pvr_register::FramebufferXClip, 1u << 16u);
    registers.write(pvr_register::FramebufferYClip, 1u << 16u);
    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::FramebufferWriteSof1, 0x1000u);
    registers.write(pvr_register::ParameterBase, 0u);
    registers.write(pvr_register::BackgroundPlaneConfig, 0x01000000u);
    const auto put_float = [&](const std::uint32_t offset, const float value) {
        vram.write_u32(offset, std::bit_cast<std::uint32_t>(value));
    };
    put_float(12u, 0.0f);
    put_float(16u, 2.0f);
    put_float(20u, 1.0f);
    vram.write_u32(24u, 0x80402010u);
    put_float(28u, 0.0f);
    put_float(32u, 0.0f);
    put_float(36u, 1.0f);
    vram.write_u32(40u, 0x80402010u);
    put_float(44u, 2.0f);
    put_float(48u, 2.0f);
    put_float(52u, 1.0f);
    vram.write_u32(56u, 0x80402010u);
    PvrSoftwareRenderer software;
    software.render({}, registers, vram);
    require(vram.read_u32(0x1000u) == 0x80402010u,
            "PVR-Hintergrundebene vertauscht ARGB-Kanaele im Renderziel.");
    require(software.metrics().proven_guest_frames == 1u &&
                software.metrics().last_frame_pixel_writes != 0u &&
                software.metrics().last_frame_changed_pixels != 0u,
            "Gastframe-Nachweis ist nicht an echte, veraendernde VRAM-Pixelwrites gebunden.");
    registers.write(pvr_register::FramebufferWriteControl, 4u);
    require(throws<std::invalid_argument>([&] { software.render({}, registers, vram); }),
            "Reservierter PVR-Renderpackmodus 4 wird als 24-Bit-Format erfunden.");

    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::BackgroundPlaneConfig, 0u);
    registers.write(pvr_register::FramebufferWriteSof1, 0x2000u);
    PvrTaFrame fog_frame;
    PvrPrimitive fog_triangle;
    fog_triangle.list = PvrListType::Opaque;
    fog_triangle.material.fog_mode = 1u;
    fog_triangle.material.offset_color_enabled = true;
    fog_triangle.vertices = {{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFF0000u, 0x80000000u},
                             {2.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFF0000u, 0x80000000u},
                             {0.0f, 2.0f, 1.0f, 0.0f, 0.0f, 0xFFFF0000u, 0x80000000u}};
    fog_frame.primitives.push_back(fog_triangle);
    registers.write(pvr_register::FogVertexColor, 0x000000FFu);
    software.render(fog_frame, registers, vram);
    require(vram.read_u32(0x2000u) == 0xFF7F0080u,
            "Per-Vertex-Fog verwendet Offset-Alpha oder Vertex-Fogfarbe nicht.");

    fog_frame.primitives[0].material.fog_mode = 0u;
    fog_frame.primitives[0].material.offset_color_enabled = false;
    registers.write(pvr_register::FogDensity, 0x00008000u);
    registers.write(pvr_register::FogTableBase, 0x0000FF00u);
    registers.write(pvr_register::FogTableColor, 0x0000FF00u);
    software.render(fog_frame, registers, vram);
    require(vram.read_u32(0x2000u) == 0xFF00FF00u,
            "Tabellen-Fog wertet Density, Tabelle oder Fogfarbe nicht aus.");

    fog_frame.primitives[0].material.fog_mode = 3u;
    registers.write(pvr_register::FogTableBase, 0x00008000u);
    registers.write(pvr_register::FogTableColor, 0x000000FFu);
    software.render(fog_frame, registers, vram);
    require(vram.read_u32(0x2000u) == 0x800000FFu,
            "Tabellen-Fog-Mode 2 ersetzt Base-RGB und Base-Alpha nicht.");

    fog_frame.primitives[0].material.fog_mode = 2u;
    fog_frame.primitives[0].material.color_clamp_enabled = true;
    registers.write(pvr_register::ColorClampMinimum, 0x00201000u);
    registers.write(pvr_register::ColorClampMaximum, 0x004080FFu);
    software.render(fog_frame, registers, vram);
    require(vram.read_u32(0x2000u) == 0xFF401000u,
            "PVR-Farbclamp bleibt trotz TSP-Freigabe wirkungslos.");

    PvrTaFrame perspective_frame;
    PvrPrimitive perspective_triangle;
    perspective_triangle.list = PvrListType::Opaque;
    perspective_triangle.material.textured = true;
    perspective_triangle.material.texture_twiddled = false;
    perspective_triangle.material.texture_width = 8u;
    perspective_triangle.material.texture_height = 8u;
    perspective_triangle.material.texture_base = 0x3000u;
    perspective_triangle.material.texture_format = 1u;
    perspective_triangle.material.texture_shading = 0u;
    perspective_triangle.material.fog_mode = 2u;
    perspective_triangle.vertices = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFu, 0u},
        {2.0f, 0.0f, 2.0f, 1.0f, 0.0f, 0xFFFFFFFFu, 0u},
        {0.0f, 2.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFu, 0u}};
    perspective_frame.primitives.push_back(perspective_triangle);
    for (std::uint32_t index = 0u; index < 64u; ++index)
        vram.write_u16(0x3000u + index * 2u, 0u);
    vram.write_u16(0x3000u + 2u * 2u, 0xF800u);
    vram.write_u16(0x3000u + 3u * 2u, 0x001Fu);
    software.render(perspective_frame, registers, vram);
    require(vram.read_u32(0x2000u) == 0xFF0000FFu,
            "Texturkoordinaten werden affin statt perspektivisch ueber 1/W interpoliert.");

    perspective_frame.primitives[0].material.texture_filter = 2u;
    require(throws<std::runtime_error>([&] { software.render(perspective_frame, registers, vram); }),
            "Nicht modelliertes Trilinear-Filtering wird still als Bilinear ausgegeben.");
    perspective_frame.primitives[0].material.texture_filter = 0u;
    perspective_frame.primitives[0].material.texture_supersampling = true;
    const auto before_supersampling = software.metrics().frames;
    software.render(perspective_frame, registers, vram);
    require(software.metrics().frames == before_supersampling + 1u,
            "Textur-Supersampling erreicht den produktiven Renderer nicht.");
    perspective_frame.primitives[0].material.texture_supersampling = false;
    perspective_frame.primitives[0].material.blend_source_accumulation = true;
    vram.write_u32(0x2000u, 0xFFFFFFFFu);
    const auto before_accumulation = software.metrics().frames;
    software.render(perspective_frame, registers, vram);
    require(software.metrics().frames == before_accumulation + 1u &&
                vram.read_u32(0x2000u) == 0u,
            "Sekundaer-Akkumulationsquelle wird nicht im produktiven Renderer ausgewertet.");

    std::cout << "KR-2804 Texturformate, Hintergrundebene und Render-Backend erfolgreich.\n";
}
