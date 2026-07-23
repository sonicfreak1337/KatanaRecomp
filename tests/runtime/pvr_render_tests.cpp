#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/pvr.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <memory>
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

std::uint32_t framebuffer_backing_offset(const std::uint32_t logical_offset) {
    return katana::runtime::dreamcast_vram_32bit_to_linear_offset(logical_offset & 0x007FFFFFu);
}

std::uint32_t read_fb_u32(const katana::runtime::LinearMemoryDevice& vram,
                          const std::uint32_t logical_offset) {
    return vram.read_u32(framebuffer_backing_offset(logical_offset));
}

void write_fb_u32(katana::runtime::LinearMemoryDevice& vram,
                  const std::uint32_t logical_offset,
                  const std::uint32_t value) {
    vram.write_u32(framebuffer_backing_offset(logical_offset), value);
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
    auto registers_storage = std::make_unique<PvrRegisterFile>(scheduler);
    auto& registers = *registers_storage;
    LinearMemoryDevice vram(8u << 20u);
    registers.write(pvr_register::FramebufferXClip, 1u << 16u);
    registers.write(pvr_register::FramebufferYClip, 1u << 16u);
    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::FramebufferWriteSof1, 0x1000u);
    registers.write(pvr_register::ParameterBase, 0x00123454u);
    constexpr std::uint32_t background_tag_address = 0x40u;
    constexpr std::uint32_t background_tag_offset = 1u;
    constexpr std::uint32_t background_skip = 2u;
    constexpr std::uint32_t background_strip = 0x00100000u + background_tag_address * 4u;
    constexpr std::uint32_t background_stride = (3u + background_skip) * 4u;
    constexpr std::uint32_t background_first =
        background_strip + 12u + background_tag_offset * background_stride;
    registers.write(pvr_register::BackgroundPlaneConfig,
                    background_tag_offset | (background_tag_address << 3u) |
                        (background_skip << 24u));
    registers.write(pvr_register::BackgroundPlaneDepth, std::bit_cast<std::uint32_t>(0.75f));
    const auto put_float = [&](const std::uint32_t offset, const float value) {
        vram.write_u32(offset, std::bit_cast<std::uint32_t>(value));
    };
    const auto put_background_vertex = [&](const std::uint32_t address,
                                           const float x,
                                           const float y,
                                           const std::uint32_t color) {
        put_float(address, x);
        put_float(address + 4u, y);
        put_float(address + 8u, 0.10f);
        vram.write_u32(address + 12u, color);
        vram.write_u32(address + 16u, 0xDEADBEEFu);
    };
    constexpr std::uint32_t background_tsp =
        (1u << 29u) | (2u << 22u) | (1u << 20u);
    vram.write_u32(background_strip, 0u);
    vram.write_u32(background_strip + 4u, background_tsp);
    vram.write_u32(background_strip + 8u, 0u);
    // An untextured background is the full background surface; its stored
    // coordinates define interpolation, not a triangle-shaped coverage clip.
    put_background_vertex(background_first, 100.0f, 102.0f, 0x80402010u);
    put_background_vertex(
        background_first + background_stride, 100.0f, 100.0f, 0x80402010u);
    put_background_vertex(
        background_first + background_stride * 2u, 102.0f, 102.0f, 0x80402010u);
    PvrSoftwareRenderer software;
    const auto observe_scanout = [&] {
        software.observe_vblank_scanout(registers, vram.bytes());
        return software.take_guest_frame_proof();
    };
    software.render({}, registers, vram);
    require(read_fb_u32(vram, 0x1000u) == 0x80402010u,
            "PVR-Hintergrundebene dekodiert Tagadresse, Offset, Skip, ARGB oder Fullscreen-Coverage falsch.");
    const auto first_render_generation = software.last_render_generation();
    require(first_render_generation != 0u && software.metrics().proven_guest_frames == 0u &&
                software.pending_render_generations() == 1u &&
                software.metrics().last_frame_pixel_writes != 0u &&
                software.metrics().last_frame_changed_pixels != 0u,
            "Renderer beweist einen Gastframe vor dessen sichtbarem VBlank-Scanout.");
    require(!observe_scanout().has_value(),
            "Deaktivierter FB_R-Scanout beweist einen Gastframe.");
    registers.write(pvr_register::FramebufferReadControl, 0xDu);
    registers.write(pvr_register::FramebufferReadSize, (1u << 10u) | 1u);
    registers.write(pvr_register::FramebufferReadSof1, 0x1000u);
    require(!observe_scanout().has_value(),
            "Geblankter Videoausgang beweist einen Gastframe.");
    registers.write(pvr_register::VideoControl,
                    registers.read(pvr_register::VideoControl) & ~0x8u);
    registers.write(pvr_register::FramebufferReadSof1, 0x1200u);
    require(!observe_scanout().has_value() &&
                software.pending_render_generations() == 1u,
            "Nicht gebundener Renderbuffer wird als aktiver Gastframe bewiesen.");
    registers.write(pvr_register::FramebufferReadSof1, 0x1000u);
    const auto first_guest_frame = observe_scanout();
    require(first_guest_frame.has_value() &&
                first_guest_frame->render_generation == first_render_generation &&
                first_guest_frame->changed_pixels != 0u &&
                first_guest_frame->frame.width == 2u &&
                first_guest_frame->frame.height == 2u &&
                software.metrics().proven_guest_frames == 1u &&
                software.pending_render_generations() == 0u &&
                !observe_scanout().has_value(),
            "Gastframe-Nachweis ist nicht einmalig an veraenderte, abgetastete Pixel gebunden.");

    {
        EventScheduler ordering_scheduler;
        auto ordering_registers_storage = std::make_unique<PvrRegisterFile>(
            ordering_scheduler, PvrTiming{20u, 100u, 100u});
        auto& ordering_registers = *ordering_registers_storage;
        LinearMemoryDevice ordering_vram(8u << 20u);
        PvrSoftwareRenderer ordering_renderer;
        ordering_registers.write(pvr_register::FramebufferXClip, 0u);
        ordering_registers.write(pvr_register::FramebufferYClip, 0u);
        ordering_registers.write(pvr_register::FramebufferWriteControl, 6u);
        ordering_registers.write(pvr_register::FramebufferWriteSof1, 0x4000u);
        ordering_registers.write(pvr_register::ParameterBase, 0x00100000u);
        ordering_registers.write(pvr_register::BackgroundPlaneConfig, 1u << 24u);
        ordering_registers.write(pvr_register::BackgroundPlaneDepth,
                                 std::bit_cast<std::uint32_t>(0.5f));
        ordering_registers.write(
            pvr_register::FramebufferReadControl, 0xDu | (1u << 23u));
        ordering_registers.write(pvr_register::FramebufferReadSize, 1u << 20u);
        ordering_registers.write(pvr_register::FramebufferReadSof1, 0x4000u);
        ordering_registers.write(
            pvr_register::VideoControl,
            ordering_registers.read(pvr_register::VideoControl) & ~0x8u);
        ordering_registers.write(pvr_register::SpgLoad, (9u << 16u) | 9u);
        ordering_registers.write(pvr_register::SpgVblankInterrupt, (2u << 16u) | 1u);
        ordering_vram.write_u32(0x00100000u, 0u);
        ordering_vram.write_u32(0x00100004u, background_tsp);
        ordering_vram.write_u32(0x00100008u, 0u);
        const auto put_ordering_vertex = [&](const std::uint32_t address,
                                             const float x,
                                             const float y) {
            ordering_vram.write_u32(address, std::bit_cast<std::uint32_t>(x));
            ordering_vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(y));
            ordering_vram.write_u32(address + 8u, 0u);
            ordering_vram.write_u32(address + 12u, 0xFF204060u);
        };
        put_ordering_vertex(0x0010000Cu, 0.0f, 1.0f);
        put_ordering_vertex(0x0010001Cu, 0.0f, 0.0f);
        put_ordering_vertex(0x0010002Cu, 1.0f, 1.0f);
        ordering_registers.set_render_observer([&] {
            ordering_renderer.render({}, ordering_registers, ordering_vram);
        });
        ordering_registers.set_vblank_observer([&](const bool entering) {
            if (entering)
                ordering_renderer.observe_vblank_scanout(ordering_registers,
                                                         ordering_vram.bytes());
        });
        ordering_registers.write(pvr_register::StartRender, 1u);
        const auto same_advance = ordering_scheduler.advance_to(20u, 8u);
        require(same_advance.processed_events >= 2u &&
                    !ordering_renderer.take_guest_frame_proof().has_value() &&
                    ordering_renderer.pending_render_generations() == 1u,
                "Spaeterer Renderabschluss wird rueckwirkend dem frueheren VBlank zugeordnet.");
        static_cast<void>(ordering_scheduler.advance_to(110u, 8u));
        const auto next_vblank_proof = ordering_renderer.take_guest_frame_proof();
        require(next_vblank_proof.has_value() &&
                    next_vblank_proof->render_generation ==
                        ordering_renderer.last_render_generation(),
                "Rendergeneration wird am folgenden echten Scheduler-VBlank nicht bewiesen.");
    }
    {
        EventScheduler pal_scheduler;
        auto pal_registers_storage = std::make_unique<PvrRegisterFile>(
            pal_scheduler, PvrTiming{20u, 100u, 100u});
        auto& pal_registers = *pal_registers_storage;
        LinearMemoryDevice pal_vram(8u << 20u);
        PvrSoftwareRenderer pal_renderer;
        constexpr std::uint32_t pal_parameter_base = 0x00100000u;
        constexpr std::uint32_t pal_field_1 = 0x5000u;
        constexpr std::uint32_t pal_field_2 = 0x6000u;
        pal_registers.write(pvr_register::FramebufferXClip, 0u);
        pal_registers.write(pvr_register::FramebufferYClip, 0u);
        pal_registers.write(pvr_register::FramebufferWriteControl, 6u);
        pal_registers.write(pvr_register::FramebufferWriteSof1, pal_field_2);
        pal_registers.write(pvr_register::ParameterBase, pal_parameter_base);
        pal_registers.write(pvr_register::BackgroundPlaneConfig, 1u << 24u);
        pal_registers.write(pvr_register::BackgroundPlaneDepth,
                            std::bit_cast<std::uint32_t>(0.5f));
        pal_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        pal_registers.write(pvr_register::FramebufferReadSize, 0u);
        pal_registers.write(pvr_register::FramebufferReadSof1, pal_field_1);
        pal_registers.write(pvr_register::FramebufferReadSof2, pal_field_2);
        pal_registers.write(pvr_register::VideoControl,
                            pal_registers.read(pvr_register::VideoControl) & ~0x8u);
        pal_registers.write(pvr_register::SpgLoad, (9u << 16u) | 9u);
        pal_registers.write(pvr_register::SpgControl,
                            pal_registers.read(pvr_register::SpgControl) | 0x10u);
        const auto write_pal_background = [&](const std::uint32_t color) {
            pal_vram.write_u32(pal_parameter_base, 0u);
            pal_vram.write_u32(pal_parameter_base + 4u, background_tsp);
            pal_vram.write_u32(pal_parameter_base + 8u, 0u);
            const auto write_vertex = [&](const std::uint32_t address,
                                          const float x,
                                          const float y) {
                pal_vram.write_u32(address, std::bit_cast<std::uint32_t>(x));
                pal_vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(y));
                pal_vram.write_u32(address + 8u, 0u);
                pal_vram.write_u32(address + 12u, color);
            };
            write_vertex(pal_parameter_base + 12u, 0.0f, 1.0f);
            write_vertex(pal_parameter_base + 28u, 0.0f, 0.0f);
            write_vertex(pal_parameter_base + 44u, 1.0f, 1.0f);
        };
        write_pal_background(0xFF123456u);
        pal_renderer.render({}, pal_registers, pal_vram);
        require(pal_registers.field() == 0u,
                "PAL-Test beginnt nicht im ersten Scanoutfeld.");
        pal_renderer.observe_vblank_scanout(pal_registers, pal_vram.bytes());
        require(!pal_renderer.take_guest_frame_proof().has_value(),
                "SOF2-Render wird waehrend des aktiven PAL-SOF1-Felds bewiesen.");
        static_cast<void>(pal_scheduler.advance_to(100u, 64u));
        require(pal_registers.field() == 1u,
                "PAL-Scanout wechselt nach einem Feldzeitraum nicht auf SOF2.");
        pal_renderer.observe_vblank_scanout(pal_registers, pal_vram.bytes());
        const auto pal_field_proof = pal_renderer.take_guest_frame_proof();
        require(pal_field_proof.has_value() && pal_field_proof->scanout_field == 1u &&
                    pal_field_proof->frame.width == 1u && pal_field_proof->frame.height == 1u,
                "Aktives PAL-Feld bindet SOF2 nicht an den exakten VBlank-Frame.");

        write_pal_background(0xFF654321u);
        pal_renderer.render({}, pal_registers, pal_vram);
        write_fb_u32(pal_vram, pal_field_2, 0xFFA0B0C0u);
        pal_renderer.observe_vblank_scanout(pal_registers, pal_vram.bytes());
        require(!pal_renderer.take_guest_frame_proof().has_value() &&
                    pal_renderer.pending_render_generations() == 1u,
                "Nach dem Render ueberschriebene VRAM-Bytes beweisen einen veralteten Frame.");

        const auto field_2_before = read_fb_u32(pal_vram, pal_field_2);
        pal_registers.write(pvr_register::FramebufferWriteSof1, 0x7100u);
        pal_registers.write(pvr_register::FramebufferWriteSof2, 0x7000u);
        pal_registers.write(pvr_register::ScalerControl, 0x00060800u);
        write_pal_background(0xFF0A1B2Cu);
        pal_renderer.render({}, pal_registers, pal_vram);
        require(read_fb_u32(pal_vram, 0x7000u) == 0xFF0A1B2Cu &&
                    read_fb_u32(pal_vram, pal_field_2) == field_2_before,
                "SCALER_CTL-Interlace/Fieldselect schreibt Feld 2 nicht nach FB_W_SOF2.");
        pal_registers.write(pvr_register::ScalerControl, 0x00020800u);
        write_pal_background(0xFF2C1B0Au);
        pal_renderer.render({}, pal_registers, pal_vram);
        require(read_fb_u32(pal_vram, 0x7100u) == 0xFF2C1B0Au,
                "SCALER_CTL-Interlace ohne Fieldselect schreibt Feld 1 nicht nach FB_W_SOF1.");
    }
    software.render({}, registers, vram);
    require(software.last_render_generation() > first_render_generation &&
                software.metrics().last_frame_pixel_writes != 0u &&
                software.metrics().last_frame_changed_pixels == 0u &&
                software.pending_render_generations() == 0u &&
                !observe_scanout().has_value(),
            "Nur zwischenzeitliche Writes ohne finale Pixelaenderung erzeugen Frame-Evidenz.");

    PvrSoftwareRenderer textured_background;
    constexpr std::uint32_t textured_tag_address = 0xC0u;
    constexpr std::uint32_t textured_strip = 0x00100000u + textured_tag_address * 4u;
    constexpr std::uint32_t textured_stride = (3u + 3u) * 4u;
    constexpr std::uint32_t textured_first = textured_strip + 12u;
    constexpr std::uint32_t texture_base = 0x00200000u;
    registers.write(pvr_register::BackgroundPlaneConfig,
                    (textured_tag_address << 3u) | (3u << 24u));
    registers.write(pvr_register::FramebufferWriteSof1, 0x3000u);
    vram.write_u32(textured_strip, (1u << 25u) | (1u << 23u));
    vram.write_u32(textured_strip + 4u, (1u << 29u) | (2u << 22u));
    vram.write_u32(textured_strip + 8u,
                   (1u << 27u) | (1u << 26u) |
                       ((texture_base >> 3u) & 0x01FFFFFFu));
    const auto put_textured_background_vertex = [&](const std::uint32_t address,
                                                    const float x,
                                                    const float y,
                                                    const float u,
                                                    const float v) {
        put_float(address, x);
        put_float(address + 4u, y);
        put_float(address + 8u, 0.1f);
        put_float(address + 12u, u);
        put_float(address + 16u, v);
        vram.write_u32(address + 20u, 0xFFFFFFFFu);
    };
    put_textured_background_vertex(textured_first, 0.0f, 0.0f, 0.0f, 0.0f);
    put_textured_background_vertex(
        textured_first + textured_stride, 2.0f, 0.0f, 1.0f, 0.0f);
    put_textured_background_vertex(
        textured_first + textured_stride * 2u, 0.0f, 2.0f, 0.0f, 1.0f);
    for (std::uint32_t y = 0u; y < 8u; ++y) {
        for (std::uint32_t x = 0u; x < 8u; ++x)
            vram.write_u16(texture_base + (y * 8u + x) * 2u,
                           x < 4u ? 0xF800u : 0x07E0u);
    }
    textured_background.render({}, registers, vram);
    const auto textured_left = read_fb_u32(vram, 0x3000u);
    const auto textured_right = read_fb_u32(vram, 0x3004u);
    require(textured_left == 0xFFFF0000u && textured_right == 0xFF00FF00u,
            "Texturierter PVR-Hintergrund verliert ISP/TSP/TCW, UV oder Decal-Sampling: " +
                std::to_string(textured_left) + "/" + std::to_string(textured_right));

    {
        EventScheduler background_scheduler;
        auto background_registers_storage =
            std::make_unique<PvrRegisterFile>(background_scheduler);
        auto& background_registers = *background_registers_storage;
        LinearMemoryDevice background_vram(8u << 20u);
        PvrSoftwareRenderer background_renderer;
        constexpr std::uint32_t parameter_base = 0x00700000u;
        constexpr std::uint32_t first_target = 0x00010000u;
        constexpr std::uint32_t second_target = 0x00160000u;
        constexpr std::uint32_t hscale_target = 0x002B0000u;
        constexpr std::uint32_t gradient_stride = 20u;
        background_registers.write(pvr_register::FramebufferXClip, 639u << 16u);
        background_registers.write(pvr_register::FramebufferYClip, 479u << 16u);
        background_registers.write(pvr_register::FramebufferWriteControl, 6u);
        background_registers.write(pvr_register::ParameterBase, parameter_base);
        background_registers.write(pvr_register::BackgroundPlaneConfig, 2u << 24u);
        background_registers.write(pvr_register::BackgroundPlaneDepth,
                                   std::bit_cast<std::uint32_t>(0.5f));
        const auto write_gradient_background = [&](const float coordinate_bias) {
            background_vram.write_u32(parameter_base, 0x01800000u);
            background_vram.write_u32(parameter_base + 4u, background_tsp);
            background_vram.write_u32(parameter_base + 8u, 0u);
            const auto write_vertex = [&](const std::uint32_t address,
                                          const float x,
                                          const float y,
                                          const std::uint32_t color,
                                          const std::uint32_t offset) {
                background_vram.write_u32(address, std::bit_cast<std::uint32_t>(x));
                background_vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(y));
                background_vram.write_u32(address + 8u, 0u);
                background_vram.write_u32(address + 12u, color);
                background_vram.write_u32(address + 16u, offset);
            };
            write_vertex(parameter_base + 12u,
                         coordinate_bias + 100.0f,
                         coordinate_bias + 300.0f,
                         0xFFFF0000u,
                         0x00010000u);
            write_vertex(parameter_base + 12u + gradient_stride,
                         coordinate_bias - 700.0f,
                         coordinate_bias - 900.0f,
                         0xFF00FF00u,
                         0x00000100u);
            write_vertex(parameter_base + 12u + gradient_stride * 2u,
                         coordinate_bias + 5.0f,
                         coordinate_bias - 11.0f,
                         0xFF0000FFu,
                         0x00000001u);
        };
        write_gradient_background(0.0f);
        background_registers.write(pvr_register::FramebufferWriteSof1, first_target);
        background_renderer.render({}, background_registers, background_vram);
        const auto first_top_left = read_fb_u32(background_vram, first_target);
        const auto first_top_right =
            read_fb_u32(background_vram, first_target + 639u * 4u);
        const auto first_bottom_left =
            read_fb_u32(background_vram, first_target + 479u * 640u * 4u);
        const auto first_bottom_right =
            read_fb_u32(background_vram, first_target + (479u * 640u + 639u) * 4u);
        require(first_top_left != first_top_right &&
                    ((first_bottom_left >> 8u) & 0xFFu) <= 2u &&
                    ((first_bottom_right >> 8u) & 0xFFu) <= 2u &&
                    (first_bottom_right & 0xFFu) >= 0xFCu,
                "BGP-Quad uebernimmt unten rechts nicht Farbe/Offset von Vertex C.");

        write_gradient_background(50'000.0f);
        background_registers.write(pvr_register::FramebufferWriteSof1, second_target);
        background_renderer.render({}, background_registers, background_vram);
        require(read_fb_u32(background_vram, second_target) == first_top_left &&
                    read_fb_u32(background_vram, second_target + 639u * 4u) == first_top_right &&
                    read_fb_u32(background_vram, second_target + 479u * 640u * 4u) ==
                        first_bottom_left &&
                    read_fb_u32(background_vram,
                                second_target + (479u * 640u + 639u) * 4u) ==
                        first_bottom_right,
                "Untexturierte BGP-Gouraud-/Offsetgewichte haengen von gespeicherten XY ab.");

        write_gradient_background(-50'000.0f);
        background_registers.write(pvr_register::ScalerControl, 0x00010400u);
        background_registers.write(pvr_register::FramebufferWriteSof1, hscale_target);
        background_renderer.render({}, background_registers, background_vram);
        require(read_fb_u32(background_vram, hscale_target + 639u * 4u) != first_top_right,
                "SCALER_CTL.hscale erreicht die feste untexturierte BGP-Geometrie nicht.");
    }

    {
        EventScheduler textured_scheduler;
        auto textured_registers_storage =
            std::make_unique<PvrRegisterFile>(textured_scheduler);
        auto& textured_registers = *textured_registers_storage;
        LinearMemoryDevice textured_vram(8u << 20u);
        PvrSoftwareRenderer textured_renderer;
        constexpr std::uint32_t parameter_base = 0x00700000u;
        constexpr std::uint32_t texture_address = 0x00680000u;
        constexpr std::uint32_t normal_target = 0x00400000u;
        constexpr std::uint32_t hscale_target = 0x00540000u;
        constexpr std::uint32_t vertex_stride = 24u;
        textured_registers.write(pvr_register::FramebufferXClip, 639u << 16u);
        textured_registers.write(pvr_register::FramebufferYClip, 479u << 16u);
        textured_registers.write(pvr_register::FramebufferWriteControl, 6u);
        textured_registers.write(pvr_register::ParameterBase, parameter_base);
        textured_registers.write(pvr_register::BackgroundPlaneConfig, 3u << 24u);
        textured_registers.write(pvr_register::BackgroundPlaneDepth,
                                 std::bit_cast<std::uint32_t>(0.5f));
        const auto write_textured_parameters = [&] {
            textured_vram.write_u32(parameter_base, 0x02800000u);
            textured_vram.write_u32(parameter_base + 4u, (1u << 29u) | (2u << 22u));
            textured_vram.write_u32(parameter_base + 8u,
                                    (1u << 27u) | (1u << 26u) |
                                        ((texture_address >> 3u) & 0x01FFFFFFu));
            const auto write_vertex = [&](const std::uint32_t address,
                                          const float x,
                                          const float y,
                                          const float u,
                                          const float v) {
                textured_vram.write_u32(address, std::bit_cast<std::uint32_t>(x));
                textured_vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(y));
                textured_vram.write_u32(address + 8u, 0u);
                textured_vram.write_u32(address + 12u, std::bit_cast<std::uint32_t>(u));
                textured_vram.write_u32(address + 16u, std::bit_cast<std::uint32_t>(v));
                textured_vram.write_u32(address + 20u, 0xFFFFFFFFu);
            };
            write_vertex(parameter_base + 12u, 0.0f, 0.0f, 0.0f, 0.0f);
            write_vertex(
                parameter_base + 12u + vertex_stride, 640.0f, 0.0f, 1.0f, 0.0f);
            write_vertex(parameter_base + 12u + vertex_stride * 2u,
                         0.0f,
                         480.0f,
                         0.0f,
                         1.0f);
        };
        for (std::uint32_t y = 0u; y < 8u; ++y) {
            for (std::uint32_t x = 0u; x < 8u; ++x)
                textured_vram.write_u16(texture_address + (y * 8u + x) * 2u,
                                        x < 4u ? 0xF800u : 0x07E0u);
        }
        write_textured_parameters();
        textured_registers.write(pvr_register::FramebufferWriteSof1, normal_target);
        textured_renderer.render({}, textured_registers, textured_vram);
        const auto normal_middle =
            read_fb_u32(textured_vram, normal_target + (240u * 640u + 320u) * 4u);
        write_textured_parameters();
        textured_registers.write(pvr_register::ScalerControl, 0x00010400u);
        textured_registers.write(pvr_register::FramebufferWriteSof1, hscale_target);
        textured_renderer.render({}, textured_registers, textured_vram);
        const auto hscale_middle =
            read_fb_u32(textured_vram, hscale_target + (240u * 640u + 320u) * 4u);
        require(normal_middle == 0xFF00FF00u && hscale_middle == 0xFFFF0000u,
                "Texturierte BGP-Erweiterung passt X/U bei SCALER_CTL.hscale nicht gemeinsam an.");
    }

    registers.write(pvr_register::BackgroundPlaneConfig,
                    background_tag_offset | (background_tag_address << 3u) |
                        (background_skip << 24u));
    PvrTaFrame background_depth_frame;
    PvrPrimitive depth_triangle;
    depth_triangle.list = PvrListType::Opaque;
    depth_triangle.material.depth_compare = 4u;
    depth_triangle.material.fog_mode = 2u;
    depth_triangle.vertices = {{0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0xFFFF0000u},
                               {2.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0xFFFF0000u},
                               {0.0f, 2.0f, 0.5f, 0.0f, 0.0f, 0xFFFF0000u}};
    background_depth_frame.primitives.push_back(depth_triangle);
    registers.write(pvr_register::FramebufferWriteSof1, 0x1400u);
    software.render(background_depth_frame, registers, vram);
    require(read_fb_u32(vram, 0x1400u) == 0x80402010u,
            "ISP_BACKGND_D ersetzt die im Hintergrundvertex gespeicherte Tiefe nicht.");

    constexpr std::uint32_t shadow_tag_address = 0x80u;
    constexpr std::uint32_t shadow_strip = 0x00100000u + shadow_tag_address * 4u;
    constexpr std::uint32_t shadow_stride = (3u + 1u + 1u) * 4u;
    registers.write(pvr_register::BackgroundPlaneConfig,
                    (shadow_tag_address << 3u) | (1u << 24u) | (1u << 27u));
    registers.write(pvr_register::ShadingScale, 0x00000100u);
    vram.write_u32(shadow_strip, 0u);
    vram.write_u32(shadow_strip + 4u, background_tsp);
    vram.write_u32(shadow_strip + 8u, 0u);
    put_background_vertex(shadow_strip + 12u, 0.0f, 2.0f, 0xFF102030u);
    put_background_vertex(shadow_strip + 12u + shadow_stride, 0.0f, 0.0f, 0xFF102030u);
    put_background_vertex(
        shadow_strip + 12u + shadow_stride * 2u, 2.0f, 2.0f, 0xFF102030u);
    registers.write(pvr_register::FramebufferWriteSof1, 0x01001800u);
    software.render({}, registers, vram);
    const auto rtt_generation = software.last_render_generation();
    require(registers.read(pvr_register::FramebufferWriteSof1) == 0x01001800u &&
                read_fb_u32(vram, 0x1800u) == 0xFF102030u,
            "PVR-Hintergrund verliert Shadow-Stride oder physische RTT-Zielreduktion.");
    require(!observe_scanout().has_value(),
            "Unsichtbare Render- oder RTT-Generation wird ohne Read-FB-Flip bewiesen.");
    registers.write(pvr_register::FramebufferReadSof1, 0x1800u);
    const auto flipped_guest_frame = observe_scanout();
    require(flipped_guest_frame.has_value() &&
                flipped_guest_frame->render_generation == rtt_generation &&
                software.metrics().proven_guest_frames == 2u &&
                software.pending_render_generations() == 0u,
            "Spaeterer Read-FB-Flip bindet die erhaltene RTT-Generation nicht sichtbar ein.");

    registers.write(pvr_register::FramebufferWriteControl, 4u);
    require(throws<std::invalid_argument>([&] { software.render({}, registers, vram); }),
            "Reservierter PVR-Renderpackmodus 4 wird als 24-Bit-Format erfunden.");

    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::BackgroundPlaneConfig,
                    background_tag_offset | (background_tag_address << 3u) |
                        (background_skip << 24u));
    registers.write(pvr_register::ShadingScale, 0u);
    registers.write(pvr_register::BackgroundPlaneDepth, 0u);
    vram.write_u32(background_first + 12u, 0u);
    vram.write_u32(background_first + background_stride + 12u, 0u);
    vram.write_u32(background_first + background_stride * 2u + 12u, 0u);
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
    require(read_fb_u32(vram, 0x2000u) == 0xFF7F0080u,
            "Per-Vertex-Fog verwendet Offset-Alpha oder Vertex-Fogfarbe nicht.");

    fog_frame.primitives[0].material.fog_mode = 0u;
    fog_frame.primitives[0].material.offset_color_enabled = false;
    registers.write(pvr_register::FogDensity, 0x00008000u);
    registers.write(pvr_register::FogTableBase, 0x0000FF00u);
    registers.write(pvr_register::FogTableColor, 0x0000FF00u);
    software.render(fog_frame, registers, vram);
    require(read_fb_u32(vram, 0x2000u) == 0xFF00FF00u,
            "Tabellen-Fog wertet Density, Tabelle oder Fogfarbe nicht aus.");

    fog_frame.primitives[0].material.fog_mode = 3u;
    registers.write(pvr_register::FogTableBase, 0x00008000u);
    registers.write(pvr_register::FogTableColor, 0x000000FFu);
    software.render(fog_frame, registers, vram);
    require(read_fb_u32(vram, 0x2000u) == 0x800000FFu,
            "Tabellen-Fog-Mode 2 ersetzt Base-RGB und Base-Alpha nicht.");

    fog_frame.primitives[0].material.fog_mode = 2u;
    fog_frame.primitives[0].material.color_clamp_enabled = true;
    registers.write(pvr_register::ColorClampMinimum, 0x00201000u);
    registers.write(pvr_register::ColorClampMaximum, 0x004080FFu);
    software.render(fog_frame, registers, vram);
    require(read_fb_u32(vram, 0x2000u) == 0xFF401000u,
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
    require(read_fb_u32(vram, 0x2000u) == 0xFF0000FFu,
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
    write_fb_u32(vram, 0x2000u, 0xFFFFFFFFu);
    const auto before_accumulation = software.metrics().frames;
    software.render(perspective_frame, registers, vram);
    require(software.metrics().frames == before_accumulation + 1u &&
                read_fb_u32(vram, 0x2000u) == 0u,
            "Sekundaer-Akkumulationsquelle wird nicht im produktiven Renderer ausgewertet.");

    {
        EventScheduler direct_scheduler;
        Memory direct_memory(0u);
        const auto direct_vram = map_dreamcast_vram(direct_memory);
        auto direct_registers_storage =
            std::make_unique<PvrRegisterFile>(direct_scheduler, PvrTiming{20u, 100u, 100u});
        auto& direct_registers = *direct_registers_storage;
        PvrSoftwareRenderer direct_renderer;
        direct_memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
            direct_renderer.observe_vram_write(event.address, event.size, event.bytes_changed);
        });
        direct_registers.set_vblank_observer([&](const bool entering) {
            if (entering)
                direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        });
        direct_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        direct_registers.write(pvr_register::FramebufferReadSof1, 0x00200000u);
        direct_registers.write(pvr_register::FramebufferReadSize, 0u);
        direct_registers.write(pvr_register::SpgLoad, (9u << 16u) | 9u);
        direct_registers.write(pvr_register::SpgVblankInterrupt, (2u << 16u) | 1u);

        constexpr std::uint32_t direct_32bit_offset = 0x00200000u;
        constexpr std::uint32_t direct_32bit_address = 0xA5000000u + direct_32bit_offset;
        constexpr std::uint32_t offscreen_32bit_address = direct_32bit_address + 0x00010000u;
        constexpr std::uint32_t direct_linear_offset =
            dreamcast_vram_32bit_to_linear_offset(direct_32bit_offset);
        direct_vram->write_u32(direct_linear_offset, 0x00112233u);
        direct_registers.write(pvr_register::VideoControl,
                               direct_registers.read(pvr_register::VideoControl) & ~0x8u);
        static_cast<void>(direct_scheduler.advance_to(20u, 8u));
        require(!direct_renderer.take_guest_frame_proof().has_value() &&
                    direct_renderer.metrics().proven_guest_frames == 0u,
                "Hostseitig vorgefuelltes VRAM wird als direkter Gastframe bewiesen.");

        direct_memory.write_u32(offscreen_32bit_address, 0x00AABBCCu);
        static_cast<void>(direct_scheduler.advance_to(220u, 16u));
        require(!direct_renderer.take_guest_frame_proof().has_value() &&
                    direct_renderer.metrics().proven_guest_frames == 0u,
                "Nicht sichtbarer direkter VRAM-Write beweist den aktiven Scanout.");

        direct_registers.write(pvr_register::FramebufferReadSof1,
                               direct_32bit_offset + 0x00010000u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        const auto flipped_offscreen_proof = direct_renderer.take_guest_frame_proof();
        require(flipped_offscreen_proof.has_value() &&
                    flipped_offscreen_proof->source ==
                        PvrGuestFrameProofSource::DirectFramebuffer &&
                    flipped_offscreen_proof->frame.rgba ==
                        std::vector<std::uint8_t>({0xAAu, 0xBBu, 0xCCu, 0xFFu}),
                "Offscreen-Dirty-Evidenz bleibt nicht bis zum sichtbaren Buffer-Flip erhalten.");
        direct_registers.write(pvr_register::FramebufferReadSof1, direct_32bit_offset);

        direct_registers.write(pvr_register::VideoControl,
                               direct_registers.read(pvr_register::VideoControl) | 0x8u);
        direct_memory.write_u32(direct_32bit_address, 0x00445566u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        require(!direct_renderer.take_guest_frame_proof().has_value() &&
                    direct_renderer.metrics().proven_guest_frames == 1u,
                "Geblankter direkter VRAM-Write wird als Gastframe bewiesen.");

        direct_registers.write(pvr_register::VideoControl,
                               direct_registers.read(pvr_register::VideoControl) & ~0x8u);
        static_cast<void>(direct_scheduler.advance_to(240u, 8u));
        const auto direct_proof = direct_renderer.take_guest_frame_proof();
        require(direct_proof.has_value() && direct_proof->frame.width == 1u &&
                    direct_proof->frame.height == 1u &&
                    direct_proof->source == PvrGuestFrameProofSource::DirectFramebuffer &&
                    direct_proof->frame.rgba ==
                        std::vector<std::uint8_t>({0x44u, 0x55u, 0x66u, 0xFFu}) &&
                    direct_renderer.metrics().proven_guest_frames == 2u &&
                    direct_renderer.metrics().direct_scanout_frames == 2u &&
                    direct_renderer.metrics().direct_scanout_changed_pixels == 2u &&
                    direct_renderer.metrics().frames == 0u &&
                    direct_registers.render_request_count() == 0u &&
                    direct_vram->read_u32(direct_linear_offset) == 0x00445566u,
                "32-Bit-VRAM-Alias, VBlank und aktiver Scanout beweisen keinen "
                "direkten C888-Gastframe ohne TA.");
        static_cast<void>(direct_scheduler.advance_to(440u, 16u));
        require(!direct_renderer.take_guest_frame_proof().has_value() &&
                    direct_renderer.metrics().proven_guest_frames == 2u,
                "Ein direkter VRAM-Write wird an mehreren VBlanks erneut bewiesen.");

        direct_memory.write_u32(offscreen_32bit_address, 0xFFAABBCCu);
        direct_registers.write(pvr_register::FramebufferReadSof1,
                               direct_32bit_offset + 0x00010000u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        require(!direct_renderer.take_guest_frame_proof().has_value() &&
                    direct_renderer.metrics().proven_guest_frames == 2u,
                "Nur das unsichtbare C888-Highbyte erzeugt nach einem Buffer-Revisit einen Frame.");
        direct_registers.write(pvr_register::FramebufferReadSof1, direct_32bit_offset);

        direct_registers.write(pvr_register::FramebufferReadControl, 1u | (5u << 4u));
        direct_memory.write_u16(direct_32bit_address, 0x7C00u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        const auto rgb0555_proof = direct_renderer.take_guest_frame_proof();
        require(rgb0555_proof.has_value() && rgb0555_proof->frame.width == 2u &&
                    rgb0555_proof->frame.rgba.size() == 8u &&
                    std::equal(rgb0555_proof->frame.rgba.begin(),
                               rgb0555_proof->frame.rgba.begin() + 4u,
                               std::array<std::uint8_t, 4u>{0xFDu, 0x05u, 0x05u, 0xFFu}.begin()),
                "FB_R_CTRL-RGB0555 ignoriert Concatbits oder behandelt Bit 15 "
                "faelschlich als Scanout-Alpha.");

        direct_memory.write_u32(offscreen_32bit_address, 0x00010203u);
        direct_renderer.reset_guest_frame_evidence();
        direct_registers.write(pvr_register::FramebufferReadSof1,
                               direct_32bit_offset + 0x00010000u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        require(!direct_renderer.take_guest_frame_proof().has_value(),
                "PVR-Evidenzreset laesst einen alten Direct-Write spaeter wieder erscheinen.");

        direct_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        direct_registers.write(pvr_register::FramebufferReadSof1, direct_32bit_offset);
        direct_memory.write_u32(direct_32bit_address, 0x00102030u);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        require(direct_renderer.take_guest_frame_proof().has_value(),
                "C888-Scanout wird vor dem Evidenzreset nicht synchronisiert.");
        direct_renderer.reset_guest_frame_evidence();
        direct_memory.write_u8(direct_32bit_address + 3u, 0xFFu);
        direct_renderer.observe_vblank_scanout(direct_registers, direct_vram->bytes());
        require(!direct_renderer.take_guest_frame_proof().has_value(),
                "Unsichtbares C888-Highbyte erzeugt direkt nach einem Evidenzreset "
                "einen falschen Gastframe.");
    }

    PvrSoftwareRenderer bounded_evidence;
    registers.write(pvr_register::FramebufferWriteSof1, 0x6000u);
    for (std::size_t generation = 0u;
         generation < PvrSoftwareRenderer::render_evidence_capacity + 8u;
         ++generation) {
        const auto color = (generation & 1u) == 0u ? 0xFF102030u : 0xFF405060u;
        vram.write_u32(background_first + 12u, color);
        vram.write_u32(background_first + background_stride + 12u, color);
        vram.write_u32(background_first + background_stride * 2u + 12u, color);
        bounded_evidence.render({}, registers, vram);
    }
    require(bounded_evidence.pending_render_generations() ==
                PvrSoftwareRenderer::render_evidence_capacity &&
                bounded_evidence.pending_render_evidence_bytes() <=
                    PvrSoftwareRenderer::render_evidence_byte_capacity &&
                bounded_evidence.metrics().dropped_render_evidence_generations == 8u &&
                bounded_evidence.last_render_generation() ==
                    PvrSoftwareRenderer::render_evidence_capacity + 8u,
            "Nie gescannte RTT-/Offscreen-Evidenz waechst ohne feste Obergrenze.");
    const auto offscreen_examined_before =
        bounded_evidence.metrics().render_evidence_pixels_examined;
    bounded_evidence.observe_vblank_scanout(registers, vram.bytes());
    require(bounded_evidence.metrics().render_evidence_pixels_examined ==
                offscreen_examined_before &&
                bounded_evidence.metrics().render_evidence_range_rejections >=
                    PvrSoftwareRenderer::render_evidence_capacity,
            "Offscreen-Evidenz wird am VBlank pixelweise statt per Adressbereich verworfen.");

    {
        EventScheduler cap_scheduler;
        auto cap_registers_storage = std::make_unique<PvrRegisterFile>(cap_scheduler);
        auto& cap_registers = *cap_registers_storage;
        LinearMemoryDevice cap_vram(8u << 20u);
        PvrSoftwareRenderer cap_renderer;
        constexpr std::uint32_t cap_parameter_base = 0x00700000u;
        cap_registers.write(pvr_register::FramebufferXClip, 511u << 16u);
        cap_registers.write(pvr_register::FramebufferYClip, 511u << 16u);
        cap_registers.write(pvr_register::FramebufferWriteControl, 6u);
        cap_registers.write(pvr_register::FramebufferWriteSof1, 0u);
        cap_registers.write(pvr_register::ParameterBase, cap_parameter_base);
        cap_registers.write(pvr_register::BackgroundPlaneConfig, 1u << 24u);
        cap_registers.write(pvr_register::BackgroundPlaneDepth,
                            std::bit_cast<std::uint32_t>(0.5f));
        const auto write_cap_background = [&](const std::uint32_t color) {
            cap_vram.write_u32(cap_parameter_base, 0u);
            cap_vram.write_u32(cap_parameter_base + 4u, background_tsp);
            cap_vram.write_u32(cap_parameter_base + 8u, 0u);
            const auto write_vertex = [&](const std::uint32_t address,
                                          const float x,
                                          const float y) {
                cap_vram.write_u32(address, std::bit_cast<std::uint32_t>(x));
                cap_vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(y));
                cap_vram.write_u32(address + 8u, 0u);
                cap_vram.write_u32(address + 12u, color);
            };
            write_vertex(cap_parameter_base + 12u, 0.0f, 512.0f);
            write_vertex(cap_parameter_base + 28u, 0.0f, 0.0f);
            write_vertex(cap_parameter_base + 44u, 512.0f, 512.0f);
        };
        for (std::uint32_t generation = 0u; generation < 22u; ++generation) {
            write_cap_background((generation & 1u) == 0u ? 0xFF102030u : 0xFF405060u);
            cap_renderer.render({}, cap_registers, cap_vram);
        }
        require(cap_renderer.pending_render_evidence_bytes() <=
                    PvrSoftwareRenderer::render_evidence_byte_capacity &&
                    cap_renderer.metrics().dropped_render_evidence_generations != 0u,
                "Pixelgenaue Renderevidenz ueberschreitet ihr globales Bytebudget.");
        for (std::uint32_t offset = 0u; offset < 512u * 512u * 4u; offset += 4u)
            write_fb_u32(cap_vram, offset, 0u);
        cap_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        cap_registers.write(pvr_register::FramebufferReadSize, (511u << 10u) | 511u);
        cap_registers.write(pvr_register::FramebufferReadSof1, 0u);
        cap_registers.write(pvr_register::VideoControl,
                            cap_registers.read(pvr_register::VideoControl) & ~0x8u);
        cap_renderer.observe_vblank_scanout(cap_registers, cap_vram.bytes());
        require(!cap_renderer.take_guest_frame_proof().has_value() &&
                    cap_renderer.metrics().render_evidence_pixels_examined ==
                        PvrSoftwareRenderer::render_evidence_scan_pixel_budget &&
                    cap_renderer.metrics().render_evidence_scan_budget_exhaustions == 1u,
                "VBlank-Evidenzpruefung besitzt kein hartes, messbares Scanbudget.");
    }

    std::cout << "KR-2804 Texturformate, Hintergrundebene und Render-Backend erfolgreich.\n";
}
