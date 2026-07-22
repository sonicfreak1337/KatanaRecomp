#include "katana/runtime/pvr.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
katana::runtime::PvrVertex vertex(const float x) {
    return {x, x + 1.0f, 0.5f, 0.0f, 0.0f, 0xFFFFFFFFu};
}
using Packet = std::array<std::uint8_t, 32u>;
void put_u32(Packet& packet, const std::size_t offset, const std::uint32_t value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}
void put_float(Packet& packet, const std::size_t offset, const float value) {
    put_u32(packet, offset, std::bit_cast<std::uint32_t>(value));
}
Packet header(const std::uint32_t object_control) {
    Packet packet{};
    put_u32(packet, 0u, 0x80000000u | object_control);
    return packet;
}
Packet ta_vertex(const std::uint32_t command, const float x) {
    Packet packet{};
    put_u32(packet, 0u, command);
    put_float(packet, 4u, x);
    put_float(packet, 8u, x + 1.0f);
    put_float(packet, 12u, 0.5f);
    return packet;
}
void end_fifo_list(katana::runtime::PvrTaFifo& fifo) {
    fifo.submit(Packet{});
}
} // namespace

int main() {
    using namespace katana::runtime;
    TileAccelerator ta;
    require(throws<std::logic_error>([&] { ta.submit_vertex(vertex(0.0f), true); }),
            "Vertex ohne offene TA-Liste wird akzeptiert.");
    ta.begin_list(PvrListType::Opaque);
    ta.submit_vertex(vertex(0.0f), false);
    ta.submit_vertex(vertex(1.0f), false);
    ta.submit_vertex(vertex(2.0f), false);
    ta.submit_vertex(vertex(3.0f), true);
    ta.end_list();
    ta.begin_list(PvrListType::Translucent);
    ta.submit_vertex(vertex(4.0f), false);
    ta.submit_vertex(vertex(5.0f), false);
    ta.submit_vertex(vertex(6.0f), true);
    ta.end_list();
    const auto frame = ta.finish_frame();
    require(frame.primitives.size() == 2u && frame.primitives[0].vertices.size() == 4u &&
                frame.primitives[1].list == PvrListType::Translucent,
            "TA verliert Strips, Vertices oder Listenklassifikation.");
    require(ta.finish_frame().primitives.empty(), "TA wiederholt Primitive im naechsten Frame.");

    TileAccelerator short_strip;
    short_strip.begin_list(PvrListType::Opaque);
    short_strip.submit_vertex(vertex(0.0f), false);
    require(throws<std::invalid_argument>([&] { short_strip.submit_vertex(vertex(1.0f), true); }),
            "TA akzeptiert einen Strip mit weniger als drei Vertices.");
    TileAccelerator ordering;
    ordering.begin_list(PvrListType::Translucent);
    ordering.submit_vertex(vertex(0.0f), false);
    ordering.submit_vertex(vertex(1.0f), false);
    ordering.submit_vertex(vertex(2.0f), true);
    ordering.end_list();
    require(throws<std::logic_error>([&] { ordering.begin_list(PvrListType::Opaque); }),
            "TA akzeptiert rueckwaertige Listenreihenfolge.");
    TileAccelerator empty_ordering;
    empty_ordering.begin_list(PvrListType::Translucent);
    empty_ordering.end_list();
    require(throws<std::logic_error>([&] { empty_ordering.begin_list(PvrListType::Opaque); }),
            "TA vergisst die Reihenfolge einer leeren Liste.");

    std::uint32_t empty_eol_notifications = 0u;
    PvrTaFifo empty_eol_fifo(
        [&](const PvrListType) { ++empty_eol_notifications; });
    end_fifo_list(empty_eol_fifo);
    require(empty_eol_fifo.finish_frame().primitives.empty() &&
                empty_eol_fifo.metrics().list_completions == 0u &&
                empty_eol_notifications == 0u,
            "TA-End-of-List ohne Polygonheader ist kein gueltiger leerer No-op.");

    PvrTaFifo packed_fifo;
    packed_fifo.submit(header(0u));
    for (std::uint32_t index = 0u; index < 3u; ++index) {
        auto packet = ta_vertex(index == 2u ? 0xF0000000u : 0xE0000000u,
                                static_cast<float>(index));
        put_u32(packet, 16u, 0xDEADBEEFu);
        put_u32(packet, 24u, 0xFF102030u + index);
        packed_fifo.submit(packet);
    }
    end_fifo_list(packed_fifo);
    const auto packed_frame = packed_fifo.finish_frame();
    require(packed_frame.primitives[0].vertices[0].argb == 0xFF102030u,
            "HOLLY2-Farbe eines untexturierten Packed-Vertices wird nicht aus 0x18 gelesen.");

    PvrTaFifo user_clip_fifo;
    Packet user_clip{};
    put_u32(user_clip, 0u, 0x20000000u);
    put_u32(user_clip, 12u, 0xFFFFFFC5u);
    put_u32(user_clip, 16u, 0xFFFFFFE3u);
    put_u32(user_clip, 20u, 0xFFFFFFCAu);
    put_u32(user_clip, 24u, 0xFFFFFFE7u);
    put_u32(user_clip, 28u, 0xFFFFFFFFu);
    user_clip_fifo.submit(user_clip);
    user_clip_fifo.submit(header(2u << 16u));
    for (std::uint32_t index = 0u; index < 3u; ++index) {
        auto packet = ta_vertex(index == 2u ? 0xF0000000u : 0xE0000000u,
                                static_cast<float>(index));
        put_u32(packet, 24u, 0xFFFFFFFFu);
        user_clip_fifo.submit(packet);
    }
    end_fifo_list(user_clip_fifo);
    const auto user_clip_frame = user_clip_fifo.finish_frame();
    require(user_clip_frame.primitives.size() == 1u &&
                user_clip_frame.primitives[0].material.user_clip_mode == 2u &&
                user_clip_frame.primitives[0].material.user_clip_start_x == 5u &&
                user_clip_frame.primitives[0].material.user_clip_start_y == 3u &&
                user_clip_frame.primitives[0].material.user_clip_end_x == 10u &&
                user_clip_frame.primitives[0].material.user_clip_end_y == 7u,
            "TA-Userclip liest nicht Dwords 3 bis 6 oder maskiert X/Y falsch.");

    PvrTaFifo continued_fifo;
    const auto submit_packed_strip = [&](const float x_base) {
        continued_fifo.submit(header(0u));
        for (std::uint32_t index = 0u; index < 3u; ++index) {
            auto packet = ta_vertex(index == 2u ? 0xF0000000u : 0xE0000000u,
                                    x_base + static_cast<float>(index));
            put_u32(packet, 24u, 0xFFFFFFFFu);
            continued_fifo.submit(packet);
        }
        end_fifo_list(continued_fifo);
    };
    submit_packed_strip(0.0f);
    continued_fifo.continue_list();
    submit_packed_strip(4.0f);
    const auto continued_frame = continued_fifo.finish_frame();
    require(continued_frame.primitives.size() == 2u &&
                continued_frame.primitives[0].vertices[0].x == 0.0f &&
                continued_frame.primitives[1].vertices[0].x == 4.0f &&
                continued_fifo.metrics().continuations == 1u,
            "TA_LIST_CONT verliert bereits erzeugte Primitive oder den Fortsetzungsstatus.");

    PvrTaFifo open_list_fifo;
    open_list_fifo.submit(header(0u));
    require(throws<std::logic_error>([&] { open_list_fifo.continue_list(); }),
            "TA_LIST_CONT akzeptiert eine noch offene Objektliste.");

    PvrTaFifo float_fifo;
    float_fifo.submit(header((1u << 4u) | 0x0Cu));
    for (std::uint32_t index = 0u; index < 3u; ++index) {
        auto first = ta_vertex(index == 2u ? 0xF0000000u : 0xE0000000u,
                               static_cast<float>(index));
        put_float(first, 16u, 0.25f);
        put_float(first, 20u, 0.75f);
        float_fifo.submit(first);
        Packet colors{};
        put_float(colors, 0u, 1.0f);
        put_float(colors, 4u, 0.5f);
        put_float(colors, 8u, 0.25f);
        put_float(colors, 12u, 0.0f);
        put_float(colors, 16u, 0.5f);
        put_float(colors, 20u, 0.0f);
        put_float(colors, 24u, 0.25f);
        put_float(colors, 28u, 1.0f);
        float_fifo.submit(colors);
    }
    end_fifo_list(float_fifo);
    const auto float_frame = float_fifo.finish_frame();
    require(float_frame.primitives[0].vertices[0].argb == 0xFF804000u &&
                float_frame.primitives[0].vertices[0].oargb == 0x800040FFu,
            "64-Byte-Floatvertex verliert Base- oder Offsetfarbe.");

    PvrTaFifo invalid_mode2;
    require(throws<std::logic_error>([&] { invalid_mode2.submit(header((3u << 4u) | 0x08u)); }),
            "Intensity-Mode 2 wird ohne vorherige Mode-1-Face-Color akzeptiert.");

    PvrTaFifo intensity_fifo;
    intensity_fifo.submit(header((2u << 4u) | 0x0Cu));
    Packet face_colors{};
    put_float(face_colors, 0u, 1.0f);
    put_float(face_colors, 4u, 0.8f);
    put_float(face_colors, 8u, 0.4f);
    put_float(face_colors, 12u, 0.2f);
    put_float(face_colors, 16u, 1.0f);
    put_float(face_colors, 20u, 0.4f);
    put_float(face_colors, 24u, 0.8f);
    put_float(face_colors, 28u, 0.2f);
    intensity_fifo.submit(face_colors);
    const auto submit_intensity_strip = [&](const float x_base) {
        for (std::uint32_t index = 0u; index < 3u; ++index) {
            auto packet = ta_vertex(index == 2u ? 0xF0000000u : 0xE0000000u,
                                    x_base + static_cast<float>(index));
            put_float(packet, 16u, 0.0f);
            put_float(packet, 20u, 0.0f);
            put_float(packet, 24u, 0.5f);
            put_float(packet, 28u, 0.25f);
            intensity_fifo.submit(packet);
        }
    };
    submit_intensity_strip(0.0f);
    intensity_fifo.submit(header((3u << 4u) | 0x0Cu));
    submit_intensity_strip(4.0f);
    end_fifo_list(intensity_fifo);
    const auto intensity_frame = intensity_fifo.finish_frame();
    require(intensity_frame.primitives.size() == 2u &&
                intensity_frame.primitives[1].vertices[0].argb == 0xFF66331Au &&
                intensity_frame.primitives[1].vertices[0].oargb == 0xFF1A330Du,
            "Intensity-Mode 2 uebernimmt Face-Colors oder Vertex-Intensitaeten nicht korrekt.");

    std::cout << "KR-2803 Tile-Accelerator-Grundpfad erfolgreich.\n";
}
