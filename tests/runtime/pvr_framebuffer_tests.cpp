#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/pvr.hpp"

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

void write_logical_vram_byte(std::vector<std::uint8_t>& vram,
                             const std::size_t logical_offset,
                             const std::uint8_t value) {
    const auto wrapped =
        static_cast<std::uint32_t>(logical_offset % katana::runtime::dreamcast_vram_size);
    vram[katana::runtime::dreamcast_vram_32bit_to_linear_offset(wrapped)] = value;
}

void write_logical_c888(std::vector<std::uint8_t>& vram,
                        const std::size_t logical_offset,
                        const std::uint8_t red,
                        const std::uint8_t green,
                        const std::uint8_t blue) {
    write_logical_vram_byte(vram, logical_offset, blue);
    write_logical_vram_byte(vram, logical_offset + 1u, green);
    write_logical_vram_byte(vram, logical_offset + 2u, red);
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
    require(frame.rgba[0] == 0xF8u && frame.rgba[1] == 0u && frame.rgba[2] == 0u &&
                frame.rgba[4] == 0u && frame.rgba[5] == 0xFCu && frame.rgba[6] == 0u,
            "RGB565 wird nicht korrekt in RGBA dekodiert.");
    require(framebuffer.presented_frames() == 1u,
            "Frame-Synchronisationszaehler schreitet nicht fort.");

    PvrFramebuffer rgb0555;
    rgb0555.configure(2u, 1u, 4u, PvrFramebufferFormat::Rgb0555, false, false, 0u, 0u, 5u);
    const auto rgb0555_frame =
        rgb0555.capture(std::array<std::uint8_t, 4u>{0x00u, 0x7Cu, 0x00u, 0xFCu});
    require(rgb0555_frame.rgba ==
                std::vector<std::uint8_t>(
                    {0xFDu, 0x05u, 0x05u, 0xFFu, 0xFDu, 0x05u, 0x05u, 0xFFu}),
            "RGB0555 ist nicht opak, wertet Bit 15 aus oder ignoriert FB_R_CTRL-CONCAT.");

    PvrFramebuffer rgb0888;
    rgb0888.configure(1u, 1u, 4u, PvrFramebufferFormat::Rgb0888);
    const auto rgb0888_frame =
        rgb0888.capture(std::array<std::uint8_t, 4u>{0x11u, 0x22u, 0x33u, 0x00u});
    require(rgb0888_frame.rgba == std::vector<std::uint8_t>({0x33u, 0x22u, 0x11u, 0xFFu}),
            "RGB0888 wird nicht korrekt in RGBA dekodiert.");

    PvrFramebuffer rgb888;
    rgb888.configure(4u, 1u, 12u, PvrFramebufferFormat::Rgb888, false, false, 0u, 0u, 0u, true);
    std::vector<std::uint8_t> rgb888_vram(dreamcast_vram_size, 0u);
    constexpr std::size_t rgb888_base = 0x00200000u;
    write_logical_c888(rgb888_vram, rgb888_base, 0x10u, 0x20u, 0x30u);
    write_logical_c888(rgb888_vram, rgb888_base + 3u, 0x40u, 0x50u, 0x60u);
    write_logical_c888(rgb888_vram, rgb888_base + 6u, 0x70u, 0x80u, 0x90u);
    write_logical_c888(rgb888_vram, rgb888_base + 9u, 0xA0u, 0xB0u, 0xC0u);
    const auto rgb888_frame = rgb888.capture(rgb888_vram, rgb888_base);
    require(rgb888_frame.rgba == std::vector<std::uint8_t>({
                                     0x10u,
                                     0x20u,
                                     0x30u,
                                     0xFFu,
                                     0x40u,
                                     0x50u,
                                     0x60u,
                                     0xFFu,
                                     0x70u,
                                     0x80u,
                                     0x90u,
                                     0xFFu,
                                     0xA0u,
                                     0xB0u,
                                     0xC0u,
                                     0xFFu,
                                 }),
            "Gepacktes RGB888 wird nicht bytegenau in vier Pixel dekodiert.");

    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler);
    require(!decode_pvr_scanout(registers, dreamcast_vram_size).has_value(),
            "Deaktivierter PVR-Scanout wird als aktiv gemeldet.");
    registers.write(pvr_register::FramebufferReadControl, 0x5u);
    registers.write(pvr_register::FramebufferReadSof1, 0x001000u);
    registers.write(pvr_register::FramebufferReadSize, (1u << 20u) | (479u << 10u) | 319u);
    const auto scanout = decode_pvr_scanout(registers, dreamcast_vram_size);
    require(scanout.has_value() && scanout->width == 640u && scanout->height == 480u &&
                scanout->stride_bytes == 1280u && scanout->base_offset == 0x1000u &&
                scanout->format == PvrFramebufferFormat::Rgb565,
            "PVR-Scanout-Register werden nicht korrekt dekodiert.");
    registers.write(pvr_register::BorderColor, 0x00123456u);
    registers.write(pvr_register::VideoControl,
                    registers.read(pvr_register::VideoControl) | 0x8u);
    const auto blanked = decode_pvr_scanout(registers, dreamcast_vram_size);
    PvrFramebuffer blanked_framebuffer;
    blanked_framebuffer.configure(blanked->width,
                                  blanked->height,
                                  blanked->stride_bytes,
                                  blanked->format,
                                  blanked->line_double,
                                  blanked->interlaced,
                                  blanked->source_width,
                                  blanked->source_height);
    const auto blanked_frame = blanked_framebuffer.capture(
        {}, blanked->base_offset, std::nullopt, blanked->border_rgba);
    require(blanked->video_blank && blanked->border_rgba ==
                                         std::array<std::uint8_t, 4u>{0x12u, 0x34u, 0x56u, 0xFFu} &&
                blanked_frame.rgba.front() == 0x12u && blanked_frame.rgba.back() == 0xFFu,
            "VO_CONTROL-Blanking oder BORDER_COL erreicht die native Scanoutausgabe nicht.");
    const auto original_read_control = registers.read(pvr_register::FramebufferReadControl);
    const auto original_spg_control = registers.read(pvr_register::SpgControl);
    registers.write(pvr_register::FramebufferReadControl, original_read_control | 2u);
    registers.write(pvr_register::SpgControl, original_spg_control | 0x10u);
    registers.write(pvr_register::ScalerControl, 1u);
    require(throws<std::out_of_range>([&] {
                static_cast<void>(decode_pvr_scanout(registers, dreamcast_vram_size));
            }),
            "Scale, Interlace und Line-Doubling duerfen keinen Multi-GiB-Hostframe erzeugen.");
    registers.write(pvr_register::ScalerControl, 0x400u);
    registers.write(pvr_register::SpgControl, original_spg_control);
    registers.write(pvr_register::FramebufferReadControl, original_read_control);
    require(throws<std::invalid_argument>([] {
                PvrFramebuffer value;
                value.configure(2u, 1u, 3u, PvrFramebufferFormat::Rgb565);
            }),
            "Zu kleiner Framebuffer-Stride wird akzeptiert.");
    require(throws<std::out_of_range>(
                [&] { static_cast<void>(framebuffer.capture(std::vector<std::uint8_t>(3u))); }),
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

    const auto capture_registered_c888 = [](const std::uint32_t modulus,
                                            const std::size_t second_row_offset) {
        EventScheduler local_scheduler;
        PvrRegisterFile local_registers(local_scheduler);
        local_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        local_registers.write(pvr_register::FramebufferReadSof1, 0u);
        local_registers.write(pvr_register::FramebufferReadSize,
                              (modulus << 20u) | (1u << 10u) | 1u);
        const auto descriptor = decode_pvr_scanout(local_registers, dreamcast_vram_size);
        require(descriptor.has_value() && descriptor->width == 2u && descriptor->height == 2u &&
                    descriptor->source_width == 2u && descriptor->source_height == 2u,
                "FB_R_SIZE-Modulus veraendert die aktive Pixelgeometrie.");
        std::vector<std::uint8_t> local_vram(dreamcast_vram_size, 0u);
        write_logical_c888(local_vram, 0u, 0x10u, 0x11u, 0x12u);
        write_logical_c888(local_vram, 4u, 0x40u, 0x41u, 0x42u);
        write_logical_c888(local_vram, second_row_offset, 0x70u, 0x71u, 0x72u);
        write_logical_c888(
            local_vram, second_row_offset + 4u, 0xA0u, 0xA1u, 0xA2u);
        PvrFramebuffer local_framebuffer;
        local_framebuffer.configure(descriptor->width,
                                    descriptor->height,
                                    descriptor->stride_bytes,
                                    descriptor->format,
                                    descriptor->line_double,
                                    descriptor->interlaced,
                                    descriptor->source_width,
                                    descriptor->source_height,
                                    descriptor->concat,
                                    true);
        return std::pair{descriptor->stride_bytes,
                         local_framebuffer.capture(local_vram, descriptor->base_offset)};
    };
    const auto [zero_modulus_stride, zero_modulus] = capture_registered_c888(0u, 4u);
    const auto [one_modulus_stride, one_modulus] = capture_registered_c888(1u, 8u);
    const auto [three_modulus_stride, three_modulus] = capture_registered_c888(3u, 16u);
    const auto row = [](const std::uint8_t first, const std::uint8_t second) {
        return std::vector<std::uint8_t>{
            first,
            static_cast<std::uint8_t>(first + 1u),
            static_cast<std::uint8_t>(first + 2u),
            0xFFu,
            second,
            static_cast<std::uint8_t>(second + 1u),
            static_cast<std::uint8_t>(second + 2u),
            0xFFu,
        };
    };
    auto expected_zero_modulus = row(0x10u, 0x70u);
    const auto zero_second_row = row(0x70u, 0xA0u);
    expected_zero_modulus.insert(
        expected_zero_modulus.end(), zero_second_row.begin(), zero_second_row.end());
    auto expected_regular_modulus = row(0x10u, 0x40u);
    const auto regular_second_row = row(0x70u, 0xA0u);
    expected_regular_modulus.insert(expected_regular_modulus.end(),
                                    regular_second_row.begin(),
                                    regular_second_row.end());
    require(zero_modulus_stride == 4u && one_modulus_stride == 8u &&
                three_modulus_stride == 16u &&
                zero_modulus.rgba == expected_zero_modulus &&
                one_modulus.rgba == expected_regular_modulus &&
                three_modulus.rgba == expected_regular_modulus,
            "FB_R_SIZE-Modulus 0, 1 oder >1 erzeugt einen falschen Zeilenstride.");

    const auto capture_pal_fields = [](const bool woven) {
        EventScheduler local_scheduler;
        PvrRegisterFile local_registers(local_scheduler);
        local_registers.write(pvr_register::FramebufferReadControl, 0xDu);
        local_registers.write(pvr_register::SpgControl, 0x10u);
        local_registers.write(pvr_register::FramebufferReadSof1, woven ? 0u : 0x100u);
        local_registers.write(pvr_register::FramebufferReadSof2, woven ? 4u : 0x200u);
        local_registers.write(pvr_register::FramebufferReadSize,
                              ((woven ? 2u : 1u) << 20u) | (1u << 10u));
        const auto descriptor = decode_pvr_scanout(local_registers, dreamcast_vram_size);
        require(descriptor.has_value() && descriptor->interlaced &&
                    descriptor->weave_fields == woven && descriptor->width == 1u &&
                    descriptor->height == (woven ? 4u : 2u) &&
                    descriptor->source_height == (woven ? 4u : 2u),
                "PAL-Feldgeometrie unterscheidet gewebte und getrennte Felder nicht.");
        std::vector<std::uint8_t> local_vram(dreamcast_vram_size, 0u);
        if (woven) {
            write_logical_c888(local_vram, 0u, 0x10u, 0x20u, 0x30u);
            write_logical_c888(local_vram, 4u, 0x40u, 0x50u, 0x60u);
            write_logical_c888(local_vram, 8u, 0x70u, 0x80u, 0x90u);
            write_logical_c888(local_vram, 12u, 0xA0u, 0xB0u, 0xC0u);
        } else {
            write_logical_c888(local_vram, 0x100u, 0x10u, 0x20u, 0x30u);
            write_logical_c888(local_vram, 0x104u, 0x70u, 0x80u, 0x90u);
            write_logical_c888(local_vram, 0x200u, 0x40u, 0x50u, 0x60u);
            write_logical_c888(local_vram, 0x204u, 0xA0u, 0xB0u, 0xC0u);
        }
        PvrFramebuffer local_framebuffer;
        local_framebuffer.configure(descriptor->width,
                                    descriptor->height,
                                    descriptor->stride_bytes,
                                    descriptor->format,
                                    descriptor->line_double,
                                    descriptor->weave_fields,
                                    descriptor->source_width,
                                    descriptor->source_height,
                                    descriptor->concat,
                                    true);
        if (woven) {
            return std::pair{
                local_framebuffer.capture(
                    local_vram, descriptor->base_offset, descriptor->second_base_offset),
                PvrFrame{}};
        }
        const auto first_field = local_framebuffer.capture(local_vram, descriptor->base_offset);
        const auto second_field =
            local_framebuffer.capture(local_vram, descriptor->second_base_offset);
        return std::pair{first_field, second_field};
    };
    const auto woven_pal = capture_pal_fields(true).first;
    const auto [pal_first_field, pal_second_field] = capture_pal_fields(false);
    const auto expected_woven_pal = std::vector<std::uint8_t>{
        0x10u, 0x20u, 0x30u, 0xFFu, 0x40u, 0x50u, 0x60u, 0xFFu,
        0x70u, 0x80u, 0x90u, 0xFFu, 0xA0u, 0xB0u, 0xC0u, 0xFFu};
    const auto expected_first_field =
        std::vector<std::uint8_t>{0x10u, 0x20u, 0x30u, 0xFFu, 0x70u, 0x80u, 0x90u, 0xFFu};
    const auto expected_second_field =
        std::vector<std::uint8_t>{0x40u, 0x50u, 0x60u, 0xFFu, 0xA0u, 0xB0u, 0xC0u, 0xFFu};
    require(woven_pal.rgba == expected_woven_pal &&
                pal_first_field.rgba == expected_first_field &&
                pal_second_field.rgba == expected_second_field,
            "PAL-Scanout liest gewebte oder getrennte SOF1-/SOF2-Felder nicht pixelgenau.");

    std::cout << "KR-2802 Framebuffer-Ausgabe erfolgreich.\n";
}
