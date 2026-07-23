#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/dma.hpp"
#include "katana/runtime/pvr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
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
std::uint64_t packet_count(const katana::runtime::PvrTaMetrics& metrics,
                           const katana::runtime::PvrTaPacketKind kind) {
    return metrics.normalized_packets[static_cast<std::size_t>(kind)];
}

struct YuvAccessRecorder {
    std::array<katana::runtime::GuestMemoryAccessEvent, 256u> events{};
    std::size_t count = 0u;
    bool overflow = false;

    static void record(void* const context,
                       const katana::runtime::GuestMemoryAccessEvent& event) noexcept {
        auto& self = *static_cast<YuvAccessRecorder*>(context);
        if (self.count == self.events.size()) {
            self.overflow = true;
            return;
        }
        self.events[self.count++] = event;
    }

    [[nodiscard]] katana::runtime::GuestMemoryAccessSink sink() noexcept {
        return {this, &record};
    }
};
} // namespace

int main() {
    using namespace katana::runtime;
    const auto fifo_destination =
        plan_pvr_channel2_destination(0x10001220u, 64u);
    require(fifo_destination.kind == PvrChannel2DestinationKind::TaFifo &&
                !fifo_destination.destination_progresses() &&
                fifo_destination.unit_count == 2u &&
                fifo_destination.destination_for_unit(0u) == 0x10001220u &&
                fifo_destination.destination_for_unit(1u) == 0x10001220u,
            "TA-FIFO-Ziel wird nicht als festes 32-Byte-Channel-2-Ziel geplant.");
    const auto yuv_destination =
        plan_pvr_channel2_destination(0x12800000u, 32u);
    require(yuv_destination.kind == PvrChannel2DestinationKind::YuvConverter &&
                !yuv_destination.destination_progresses(),
            "YUV-Fenster wird nicht als festes Channel-2-Geraeteziel erkannt.");
    const auto direct64_destination =
        plan_pvr_channel2_destination(0x11FFFFC0u, 64u);
    const auto direct32_destination =
        plan_pvr_channel2_destination(0x13000400u, 96u);
    require(direct64_destination.kind ==
                    PvrChannel2DestinationKind::DirectTexture64 &&
                direct64_destination.destination_progresses() &&
                direct64_destination.destination_for_unit(0u) == 0x11FFFFC0u &&
                direct64_destination.destination_for_unit(1u) == 0x11FFFFE0u &&
                direct32_destination.kind ==
                    PvrChannel2DestinationKind::DirectTexture32 &&
                direct32_destination.destination_for_unit(2u) == 0x13000440u,
            "Direct-Texture-Ziele 0x11/0x13 schreiten nicht je 32-Byte-Einheit fort.");
    require(throws<std::out_of_range>(
                [&] { static_cast<void>(direct64_destination.destination_for_unit(2u)); }) &&
                throws<std::out_of_range>([&] {
                    static_cast<void>(
                        plan_pvr_channel2_destination(0x11FFFFE0u, 64u));
                }) &&
                throws<std::invalid_argument>([&] {
                    static_cast<void>(
                        plan_pvr_channel2_destination(0x11000004u, 32u));
                }) &&
                throws<std::invalid_argument>([&] {
                    static_cast<void>(
                        plan_pvr_channel2_destination(0x11000000u, 48u));
                }) &&
                throws<std::out_of_range>([&] {
                    static_cast<void>(
                        plan_pvr_channel2_destination(0x14000000u, 32u));
                }),
            "PVR-Channel-2-Plan akzeptiert Einheiten ausserhalb von Laenge, "
            "Ausrichtung oder Direct-Texture-Fenster.");

    {
        EventScheduler direct_scheduler;
        Memory direct_memory(0u);
        auto direct_source = std::make_shared<LinearMemoryDevice>(128u);
        auto direct_vram = std::make_shared<LinearMemoryDevice>(dreamcast_vram_size);
        direct_memory.map_region(
            "synthetic-channel2-source", 0x0C000000u, direct_source);
        map_dreamcast_ta_vram_aliases(direct_memory, direct_vram);
        Sh4Dmac direct_dmac(direct_scheduler, direct_memory, DmaTiming{1u});
        const auto run_direct_transfer =
            [&](const std::uint32_t source_offset,
                const PvrChannel2DestinationPlan& plan,
                const std::uint32_t pattern) {
                direct_dmac.reset();
                for (std::uint32_t offset = 0u;
                     offset < static_cast<std::uint32_t>(plan.byte_count);
                     offset += 4u)
                    direct_source->write_u32(
                        source_offset + offset, pattern + offset);
                direct_dmac.write_source(2u, 0x0C000000u + source_offset);
                direct_dmac.write_destination(2u, plan.initial_address);
                direct_dmac.write_count(
                    2u, static_cast<std::uint32_t>(plan.unit_count));
                direct_dmac.write_control(2u, 0x000012C1u);
                direct_dmac.write_operation(
                    Sh4Dmac::master_enable | Sh4Dmac::on_demand_enable);
                direct_dmac.request_transfer(
                    2u,
                    static_cast<std::uint32_t>(plan.unit_count),
                    plan.destination_progresses()
                        ? DmaExternalDestinationProgression::IncrementByTransferUnit
                        : DmaExternalDestinationProgression::AddressMode);
                static_cast<void>(direct_scheduler.advance_by(
                    plan.byte_count, plan.unit_count));
                bool matches = true;
                for (std::uint32_t offset = 0u;
                     offset < static_cast<std::uint32_t>(plan.byte_count);
                     offset += 4u)
                    matches =
                        matches &&
                        direct_memory.read_u32(plan.initial_address + offset) ==
                            pattern + offset;
                require(
                    matches &&
                        direct_dmac.destination(2u) ==
                            plan.initial_address +
                                static_cast<std::uint32_t>(plan.byte_count),
                    "Mehrteiliger Direct-Texture-Transfer erreicht sein "
                    "fortschreitendes VRAM-Ziel nicht.");
            };
        run_direct_transfer(
            0u,
            plan_pvr_channel2_destination(0x11000200u, 64u),
            0x64000000u);
        run_direct_transfer(
            64u,
            plan_pvr_channel2_destination(0x13000400u, 64u),
            0x32000000u);
    }

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

    PvrTaFifo normalized_object_list_fifo;
    Packet object_list_set{};
    put_u32(object_list_set, 0u, 0x40000000u);
    normalized_object_list_fifo.submit(object_list_set);
    end_fifo_list(normalized_object_list_fifo);
    require(packet_count(normalized_object_list_fifo.metrics(),
                         PvrTaPacketKind::ObjectListSet) == 1u &&
                packet_count(normalized_object_list_fifo.metrics(),
                             PvrTaPacketKind::EndOfList) == 1u &&
                normalized_object_list_fifo.metrics().normalized_packets.size() ==
                    pvr_ta_packet_kind_count,
            "Normalisierte TA-Diagnostik verliert Object-List-Set oder EOL.");

    PvrTaFifo normalized_sprite_fifo;
    Packet sprite_header{};
    put_u32(sprite_header, 0u, 0xA0000000u);
    put_u32(sprite_header, 16u, 0xFFFFFFFFu);
    normalized_sprite_fifo.submit(sprite_header);
    Packet sprite_first{};
    put_u32(sprite_first, 0u, 0xE0000000u);
    put_float(sprite_first, 4u, 0.0f);
    put_float(sprite_first, 8u, 0.0f);
    put_float(sprite_first, 12u, 0.5f);
    put_float(sprite_first, 16u, 1.0f);
    put_float(sprite_first, 20u, 0.0f);
    put_float(sprite_first, 24u, 0.5f);
    put_float(sprite_first, 28u, 0.0f);
    normalized_sprite_fifo.submit(sprite_first);
    Packet sprite_second{};
    put_float(sprite_second, 0u, 1.0f);
    put_float(sprite_second, 4u, 0.5f);
    put_float(sprite_second, 8u, 1.0f);
    put_float(sprite_second, 12u, 1.0f);
    normalized_sprite_fifo.submit(sprite_second);
    end_fifo_list(normalized_sprite_fifo);
    const auto normalized_sprite_frame = normalized_sprite_fifo.finish_frame();
    require(normalized_sprite_frame.primitives.size() == 1u &&
                packet_count(normalized_sprite_fifo.metrics(),
                             PvrTaPacketKind::SpriteHeader) == 1u &&
                packet_count(normalized_sprite_fifo.metrics(),
                             PvrTaPacketKind::Vertex) == 1u &&
                packet_count(normalized_sprite_fifo.metrics(),
                             PvrTaPacketKind::SpriteContinuation) == 1u,
            "TA-Sprite oder seine normalisierte Continuation-Diagnostik fehlt.");

    PvrTaFifo normalized_modifier_fifo;
    normalized_modifier_fifo.submit(header(1u << 24u));
    Packet modifier_first{};
    put_u32(modifier_first, 0u, 0xE0000000u);
    normalized_modifier_fifo.submit(modifier_first);
    normalized_modifier_fifo.submit(Packet{});
    end_fifo_list(normalized_modifier_fifo);
    const auto normalized_modifier_frame = normalized_modifier_fifo.finish_frame();
    require(normalized_modifier_frame.modifier_volumes.size() == 1u &&
                normalized_modifier_frame.modifier_volumes[0].triangles.size() == 1u &&
                packet_count(normalized_modifier_fifo.metrics(),
                             PvrTaPacketKind::ModifierVertexContinuation) == 1u,
            "TA-Modifier-Continuation wird nicht normalisiert oder verarbeitet.");

    PvrTaFifo normalized_unsupported_fifo;
    Packet reserved_parameter3{};
    Packet reserved_parameter6{};
    put_u32(reserved_parameter3, 0u, 3u << 29u);
    put_u32(reserved_parameter6, 0u, 6u << 29u);
    require(throws<std::runtime_error>(
                [&] { normalized_unsupported_fifo.submit(reserved_parameter3); }) &&
                throws<std::runtime_error>(
                    [&] { normalized_unsupported_fifo.submit(reserved_parameter6); }) &&
                packet_count(normalized_unsupported_fifo.metrics(),
                             PvrTaPacketKind::ReservedParameter3) == 1u &&
                packet_count(normalized_unsupported_fifo.metrics(),
                             PvrTaPacketKind::ReservedParameter6) == 1u,
            "Reservierte TA-Pakettypen scheitern nicht sichtbar in bounded Diagnostik.");

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
                float_frame.primitives[0].vertices[0].oargb == 0x800040FFu &&
                packet_count(float_fifo.metrics(),
                             PvrTaPacketKind::ExtendedVertexContinuation) == 3u,
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
                intensity_frame.primitives[1].vertices[0].oargb == 0xFF1A330Du &&
                packet_count(intensity_fifo.metrics(),
                             PvrTaPacketKind::IntensityContinuation) == 1u,
            "Intensity-Mode 2 uebernimmt Face-Colors oder Vertex-Intensitaeten nicht korrekt.");

    {
        EventScheduler yuv_scheduler;
        auto yuv_registers = std::make_shared<PvrRegisterFile>(yuv_scheduler);
        auto yuv_vram = std::make_shared<LinearMemoryDevice>(dreamcast_vram_size);
        Memory yuv_access_memory(0u);
        YuvAccessRecorder yuv_accesses;
        yuv_access_memory.set_guest_memory_access_sink(yuv_accesses.sink());
        bool yuv_completed = false;
        PvrYuvConverterMemoryDevice yuv_converter(
            yuv_registers, yuv_vram, [&] { yuv_completed = true; });
        yuv_converter.set_guest_memory_access_memory(&yuv_access_memory);
        constexpr std::uint32_t yuv_destination = 0x2000u;
        yuv_registers->write(pvr_register::YuvConfig, 0u);
        yuv_registers->write(pvr_register::YuvAddress, yuv_destination);
        std::array<std::uint8_t, 384u> macroblock{};
        std::fill_n(macroblock.begin(), 64u, std::uint8_t{0x11u});
        std::fill_n(macroblock.begin() + 64u, 64u, std::uint8_t{0x22u});
        std::fill(macroblock.begin() + 128u, macroblock.end(), std::uint8_t{0x33u});
        for (std::size_t index = 0u; index < macroblock.size(); ++index)
            yuv_converter.write_u8(static_cast<std::uint32_t>(index), macroblock[index]);

        require(yuv_completed && yuv_converter.converted_macroblocks() == 1u &&
                    yuv_registers->read(pvr_register::YuvStatus) == 1u &&
                    !yuv_accesses.overflow && yuv_accesses.count == 256u,
                "PVR-YUV meldet nicht genau einen Zugriff je erfolgreichem Halfword-Store.");
        for (std::size_t index = 0u; index < yuv_accesses.count; ++index) {
            const auto& access = yuv_accesses.events[index];
            const auto expected_offset =
                yuv_destination + static_cast<std::uint32_t>(index * 2u);
            const auto expected_value =
                static_cast<std::uint32_t>((index & 1u) == 0u ? 0x3311u : 0x3322u);
            require(access.operation == MemoryAccessOperation::Write &&
                        access.access_origin == GuestMemoryAccessOrigin::PvrYuv &&
                        !access.instruction.valid &&
                        access.virtual_address == access.physical_address &&
                        access.physical_address ==
                            dreamcast_vram_64bit_physical_bases.front() + expected_offset &&
                        access.width == MemoryAccessWidth::Halfword &&
                        access.value == expected_value &&
                        access.size == sizeof(std::uint16_t) &&
                        access.write_source == CodeWriteSource::Fallback &&
                        access.scalar_value_valid && access.bytes_changed &&
                        access.linear_backing == yuv_vram.get() &&
                        access.linear_offset == expected_offset &&
                        access.linear_size == sizeof(std::uint16_t) &&
                        access.linear_contiguous &&
                        access.linear_byte_offsets[0] == expected_offset &&
                        access.linear_byte_offsets[1] == expected_offset + 1u &&
                        access.linear_byte_count == sizeof(std::uint16_t),
                    "PVR-YUV verliert Origin, Adresse, Wert oder lineare VRAM-Projektion.");
        }
        yuv_accesses.count = 0u;
        yuv_accesses.overflow = false;
        PvrYuvConverterMemoryDevice unchanged_yuv_converter(
            yuv_registers, yuv_vram);
        unchanged_yuv_converter.set_guest_memory_access_memory(&yuv_access_memory);
        for (std::size_t index = 0u; index < macroblock.size(); ++index)
            unchanged_yuv_converter.write_u8(
                static_cast<std::uint32_t>(index), macroblock[index]);
        require(!yuv_accesses.overflow && yuv_accesses.count == 256u &&
                    std::all_of(yuv_accesses.events.begin(),
                                yuv_accesses.events.end(),
                                [](const auto& access) { return !access.bytes_changed; }),
                "Identische PVR-YUV-Stores werden faelschlich als geaendert gemeldet.");

        EventScheduler unobserved_scheduler;
        auto unobserved_registers =
            std::make_shared<PvrRegisterFile>(unobserved_scheduler);
        auto unobserved_vram =
            std::make_shared<LinearMemoryDevice>(dreamcast_vram_size);
        bool unobserved_completed = false;
        PvrYuvConverterMemoryDevice unobserved_converter(
            unobserved_registers,
            unobserved_vram,
            [&] { unobserved_completed = true; });
        unobserved_registers->write(pvr_register::YuvConfig, 0u);
        unobserved_registers->write(pvr_register::YuvAddress, yuv_destination);
        for (std::size_t index = 0u; index < macroblock.size(); ++index)
            unobserved_converter.write_u8(
                static_cast<std::uint32_t>(index), macroblock[index]);
        require(unobserved_completed &&
                    unobserved_converter.converted_macroblocks() ==
                        yuv_converter.converted_macroblocks() &&
                    std::equal(yuv_vram->bytes().begin(),
                               yuv_vram->bytes().end(),
                               unobserved_vram->bytes().begin(),
                               unobserved_vram->bytes().end()),
                "Aktivierter PVR-YUV-Sink veraendert VRAM oder Completionzustand.");

        EventScheduler invalid_scheduler;
        auto invalid_registers =
            std::make_shared<PvrRegisterFile>(invalid_scheduler);
        auto invalid_vram = std::make_shared<LinearMemoryDevice>(dreamcast_vram_size);
        PvrYuvConverterMemoryDevice invalid_converter(invalid_registers, invalid_vram);
        invalid_converter.set_guest_memory_access_memory(&yuv_access_memory);
        invalid_registers->write(pvr_register::YuvConfig, 0u);
        invalid_registers->write(
            pvr_register::YuvAddress,
            static_cast<std::uint32_t>(dreamcast_vram_size - 0x100u));
        yuv_accesses.count = 0u;
        yuv_accesses.overflow = false;
        for (std::size_t index = 0u; index + 1u < macroblock.size(); ++index)
            invalid_converter.write_u8(
                static_cast<std::uint32_t>(index), macroblock[index]);
        require(throws<std::out_of_range>([&] {
                    invalid_converter.write_u8(
                        static_cast<std::uint32_t>(macroblock.size() - 1u),
                        macroblock.back());
                }) &&
                    yuv_accesses.count == 0u && !yuv_accesses.overflow,
                "Abgelehnter PVR-YUV-Ausgabebereich erzeugt Speicherzugriffsevidenz.");
    }

    std::cout << "KR-2803 Tile-Accelerator-Grundpfad erfolgreich.\n";
}
