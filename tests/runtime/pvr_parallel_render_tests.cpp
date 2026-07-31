#include "katana/runtime/pvr.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace katana::runtime;

void require(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct AccessCounter {
    std::uint64_t accesses = 0u;

    static void record(void* const context,
                       const GuestMemoryAccessEvent&) noexcept {
        ++static_cast<AccessCounter*>(context)->accesses;
    }

    [[nodiscard]] GuestMemoryAccessSink sink() noexcept {
        return {this, &record};
    }
};

[[nodiscard]] std::uint32_t backing_offset(
    const std::uint32_t logical) noexcept {
    return dreamcast_vram_32bit_to_linear_offset(
        logical & 0x007FFFFFu);
}

void write_logical_u32(LinearMemoryDevice& vram,
                       const std::uint32_t logical,
                       const std::uint32_t value) {
    vram.write_u32(backing_offset(logical), value);
}

void write_logical_float(LinearMemoryDevice& vram,
                         const std::uint32_t logical,
                         const float value) {
    write_logical_u32(vram, logical,
                      std::bit_cast<std::uint32_t>(value));
}

void configure_background(PvrRegisterFile& registers,
                          LinearMemoryDevice& vram,
                          const std::uint32_t target,
                          const std::uint32_t width,
                          const std::uint32_t height) {
    constexpr std::uint32_t parameter_base = 0x00300000u;
    constexpr std::uint32_t skip = 1u;
    constexpr std::uint32_t vertex_stride = (3u + skip) * 4u;
    registers.write(pvr_register::FramebufferXClip,
                    (width - 1u) << 16u);
    registers.write(pvr_register::FramebufferYClip,
                    (height - 1u) << 16u);
    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::FramebufferWriteSof1, target);
    registers.write(pvr_register::FramebufferRenderModulo,
                    width / 2u);
    registers.write(pvr_register::ParameterBase, parameter_base);
    registers.write(pvr_register::BackgroundPlaneConfig, skip << 24u);
    registers.write(pvr_register::BackgroundPlaneDepth,
                    std::bit_cast<std::uint32_t>(0.25f));
    write_logical_u32(vram, parameter_base, 0u);
    write_logical_u32(vram, parameter_base + 4u, 2u << 22u);
    write_logical_u32(vram, parameter_base + 8u, 0u);
    for (std::uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const auto address =
            parameter_base + 12u + vertex * vertex_stride;
        write_logical_float(vram, address,
                            vertex == 1u
                                ? static_cast<float>(width)
                                : 0.0f);
        write_logical_float(vram, address + 4u,
                            vertex == 2u
                                ? static_cast<float>(height)
                                : 0.0f);
        write_logical_float(vram, address + 8u, 0.25f);
        write_logical_u32(vram, address + 12u, 0xFF102030u);
    }
}

[[nodiscard]] PvrTaFrame make_textured_frame(
    const std::uint32_t texture_base,
    const float width,
    const float height) {
    PvrMaterial material;
    material.textured = true;
    material.texture_twiddled = false;
    material.texture_width = 8u;
    material.texture_height = 8u;
    material.texture_base = texture_base;
    material.texture_format = 1u;
    material.texture_shading = 0u;
    material.fog_mode = 2u;
    material.depth_compare = 7u;
    material.depth_write = true;
    material.shadow_enabled = true;
    material.volume_material =
        std::make_shared<PvrMaterial>(material);
    material.volume_material->shadow_enabled = false;
    material.volume_material->volume_material.reset();
    PvrPrimitive opaque;
    opaque.list = PvrListType::Opaque;
    opaque.material = material;
    opaque.vertices = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFu, 0u},
        {width, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFu, 0u},
        {0.0f, height, 1.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0u},
        {width, height, 1.0f, 1.0f, 1.0f, 0xFFFFFFFFu, 0u}};
    PvrPrimitive translucent = opaque;
    translucent.list = PvrListType::Translucent;
    translucent.material.source_blend = 4u;
    translucent.material.destination_blend = 5u;
    for (auto& vertex : translucent.vertices)
        vertex.argb = 0x80FFFFFFu;
    PvrTaFrame frame;
    frame.primitives.push_back(std::move(opaque));
    frame.primitives.push_back(std::move(translucent));
    const std::array<PvrVertex, 3u> modifier_triangle{
        PvrVertex{0.0f, 0.0f, 2.0f},
        PvrVertex{width, 0.0f, 2.0f},
        PvrVertex{0.0f, height, 2.0f}};
    frame.modifier_volumes.push_back(
        PvrModifierVolume{
            PvrListType::OpaqueModifier,
            {modifier_triangle},
            1u});
    frame.modifier_volumes.push_back(
        PvrModifierVolume{
            PvrListType::TranslucentModifier,
            {modifier_triangle},
            1u});
    return frame;
}

void write_texture(LinearMemoryDevice& vram,
                   const std::uint32_t base) {
    for (std::uint32_t pixel = 0u; pixel < 64u; ++pixel)
        vram.write_u16(
            base + pixel * 2u,
            (pixel & 1u) != 0u ? 0xF800u : 0x07E0u);
}

[[nodiscard]] bool equal_metrics(
    const PvrSoftwareRenderMetrics& left,
    const PvrSoftwareRenderMetrics& right) noexcept {
    return left.frames == right.frames &&
           left.triangles == right.triangles &&
           left.pixels == right.pixels &&
           left.pixel_writes == right.pixel_writes &&
           left.changed_pixels == right.changed_pixels &&
           left.proven_guest_frames == right.proven_guest_frames &&
           left.direct_scanout_frames == right.direct_scanout_frames &&
           left.direct_scanout_changed_pixels ==
               right.direct_scanout_changed_pixels &&
           left.last_frame_pixel_writes ==
               right.last_frame_pixel_writes &&
           left.last_frame_changed_pixels ==
               right.last_frame_changed_pixels &&
           left.dropped_render_evidence_generations ==
               right.dropped_render_evidence_generations &&
           left.render_evidence_pixels_examined ==
               right.render_evidence_pixels_examined &&
           left.render_evidence_range_rejections ==
               right.render_evidence_range_rejections &&
           left.render_evidence_scan_budget_exhaustions ==
               right.render_evidence_scan_budget_exhaustions;
}

[[nodiscard]] bool equal_evidence(
    const PvrRenderGenerationEvidence& left,
    const PvrRenderGenerationEvidence& right) {
    if (left.generation != right.generation ||
        left.write_base != right.write_base ||
        left.stride_bytes != right.stride_bytes ||
        left.width != right.width ||
        left.height != right.height ||
        left.pixel_bytes != right.pixel_bytes ||
        left.render_to_texture != right.render_to_texture ||
        left.pixel_writes != right.pixel_writes ||
        left.changed_pixels != right.changed_pixels ||
        left.validation_cursor != right.validation_cursor ||
        left.changed_pixel_values.size() !=
            right.changed_pixel_values.size())
        return false;
    for (std::size_t index = 0u;
         index < left.changed_pixel_values.size();
         ++index) {
        const auto& a = left.changed_pixel_values[index];
        const auto& b = right.changed_pixel_values[index];
        if (a.offset != b.offset ||
            a.packed_value != b.packed_value ||
            a.changed_byte_mask != b.changed_byte_mask)
            return false;
    }
    return true;
}

[[nodiscard]] bool equal_snapshot(
    const PvrSoftwareRendererSnapshot& left,
    const PvrSoftwareRendererSnapshot& right) {
    if (!equal_metrics(left.metrics, right.metrics) ||
        left.next_render_generation != right.next_render_generation ||
        left.last_render_generation != right.last_render_generation ||
        left.pending_render_evidence_bytes !=
            right.pending_render_evidence_bytes ||
        left.next_evidence_scan_generation !=
            right.next_evidence_scan_generation ||
        left.next_direct_write_generation !=
            right.next_direct_write_generation ||
        left.pending_direct_write_generation !=
            right.pending_direct_write_generation ||
        left.pending_direct_first_write_generation !=
            right.pending_direct_first_write_generation ||
        left.pending_direct_last_write_generation !=
            right.pending_direct_last_write_generation ||
        left.direct_dirty_words != right.direct_dirty_words ||
        left.direct_dirty_byte_count !=
            right.direct_dirty_byte_count ||
        left.direct_vram_shadow != right.direct_vram_shadow ||
        left.guest_memory_access_bound !=
            right.guest_memory_access_bound ||
        left.direct_vram_shadow_valid !=
            right.direct_vram_shadow_valid ||
        left.queued_guest_frame_proof.has_value() ||
        right.queued_guest_frame_proof.has_value() ||
        left.first_error.has_value() ||
        right.first_error.has_value() ||
        left.pending_render_evidence.size() !=
            right.pending_render_evidence.size())
        return false;
    for (std::size_t index = 0u;
         index < left.pending_render_evidence.size();
         ++index)
        if (!equal_evidence(left.pending_render_evidence[index],
                            right.pending_render_evidence[index]))
            return false;
    return true;
}

[[nodiscard]] std::string render_error(
    PvrSoftwareRenderer& renderer,
    const PvrTaFrame& frame,
    const PvrRegisterSnapshot& registers,
    LinearMemoryDevice& vram) {
    try {
        renderer.render(frame, registers, vram, 9u);
    } catch (const std::exception& error) {
        return std::string(typeid(error).name()) + ":" + error.what();
    }
    return {};
}

[[nodiscard]] std::uint64_t digest_bytes(
    const std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t digest = 1469598103934665603ull;
    for (const auto byte : bytes) {
        digest ^= byte;
        digest *= 1099511628211ull;
    }
    return digest;
}

[[nodiscard]] bool equal_bytes(
    const std::span<const std::uint8_t> left,
    const std::span<const std::uint8_t> right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] std::string run_child() {
    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler);
    constexpr std::uint32_t target = 0x00010000u;
    constexpr std::uint32_t texture = 0x00400000u;
    constexpr std::uint32_t width = 160u;
    constexpr std::uint32_t height = 160u;
    LinearMemoryDevice initial_vram(dreamcast_vram_size);
    configure_background(
        registers, initial_vram, target, width, height);
    write_texture(initial_vram, texture);
    const auto frame = make_textured_frame(
        texture,
        static_cast<float>(width),
        static_cast<float>(height));

    auto traced_vram = initial_vram;
    auto parallel_vram = initial_vram;
    Memory trace_memory(0u);
    AccessCounter trace;
    trace_memory.set_guest_memory_access_sink(trace.sink());
    PvrSoftwareRenderer traced;
    traced.set_guest_memory_access_memory(&trace_memory);
    traced.render(frame, registers.snapshot(), traced_vram, 7u);
    require(traced.last_render_job_count() == 1u &&
                trace.accesses != 0u,
            "Trace-Sink benutzt nicht exakt den seriellen Rasterpfad.");
    traced.set_guest_memory_access_memory(nullptr);
    PvrSoftwareRenderer parallel;
    parallel.render(frame, registers.snapshot(), parallel_vram, 7u);
    constexpr std::size_t tile_count = 25u;
    require(parallel.last_render_job_count() ==
                std::min(tile_count,
                         parallel.render_job_capacity()),
            "Beweisbar sicherer Texturframe nutzt nicht die gesamte "
            "verfuegbare Tile-/Hostkapazitaet.");
    require(equal_bytes(traced_vram.bytes(),
                        parallel_vram.bytes()),
            "jobs=1/N erzeugen unterschiedliche VRAM-Bytes.");
    require(equal_metrics(traced.metrics(), parallel.metrics()),
            "jobs=1/N erzeugen unterschiedliche PVR-Metriken.");
    require(equal_snapshot(traced.snapshot(), parallel.snapshot()),
            "jobs=1/N erzeugen unterschiedliche Renderer-Snapshots.");

    constexpr std::uint32_t overlapping_texture =
        dreamcast_vram_32bit_to_linear_offset(target);
    auto overlap_initial = initial_vram;
    write_texture(overlap_initial, overlapping_texture);
    const auto overlap_frame =
        make_textured_frame(
            overlapping_texture,
            static_cast<float>(width),
            static_cast<float>(height));
    auto overlap_traced_vram = overlap_initial;
    auto overlap_plain_vram = overlap_initial;
    PvrSoftwareRenderer overlap_traced;
    overlap_traced.set_guest_memory_access_memory(&trace_memory);
    overlap_traced.render(overlap_frame,
                          registers.snapshot(),
                          overlap_traced_vram,
                          8u);
    overlap_traced.set_guest_memory_access_memory(nullptr);
    PvrSoftwareRenderer overlap_plain;
    overlap_plain.render(overlap_frame,
                         registers.snapshot(),
                         overlap_plain_vram,
                         8u);
    require(overlap_plain.last_render_job_count() == 1u &&
                equal_bytes(overlap_traced_vram.bytes(),
                            overlap_plain_vram.bytes()) &&
                equal_snapshot(overlap_traced.snapshot(),
                               overlap_plain.snapshot()),
            "Textur-Renderziel-Overlap faellt nicht byte- und "
            "snapshotgleich auf seriell zurueck.");

    auto error_frame = frame;
    error_frame.primitives.back().material.texture_filter = 2u;
    auto error_traced_vram = initial_vram;
    auto error_plain_vram = initial_vram;
    PvrSoftwareRenderer error_traced;
    error_traced.set_guest_memory_access_memory(&trace_memory);
    const auto traced_error =
        render_error(error_traced,
                     error_frame,
                     registers.snapshot(),
                     error_traced_vram);
    error_traced.set_guest_memory_access_memory(nullptr);
    PvrSoftwareRenderer error_plain;
    const auto plain_error =
        render_error(error_plain,
                     error_frame,
                     registers.snapshot(),
                     error_plain_vram);
    require(!plain_error.empty() &&
                traced_error == plain_error &&
                error_plain.last_render_job_count() == 1u &&
                error_plain.metrics().triangles == 4u &&
                error_plain.metrics().pixels != 0u &&
                equal_bytes(error_traced_vram.bytes(),
                            error_plain_vram.bytes()) &&
                equal_snapshot(error_traced.snapshot(),
                               error_plain.snapshot()),
            "Moeglicher Rasterfehler faellt nicht mit identischem "
            "Fehler- und Partial-State-Verhalten auf seriell zurueck.");

    std::ostringstream summary;
    summary << std::hex << digest_bytes(parallel_vram.bytes()) << ':'
            << parallel.metrics().triangles << ':'
            << parallel.metrics().pixels << ':'
            << parallel.metrics().pixel_writes << ':'
            << parallel.metrics().changed_pixels << ':'
            << parallel.snapshot().pending_render_evidence_bytes;
    return summary.str();
}

[[nodiscard]] std::string run_benchmark() {
    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler);
    constexpr std::uint32_t target = 0x00010000u;
    constexpr std::uint32_t texture = 0x00400000u;
    constexpr std::uint32_t width = 640u;
    constexpr std::uint32_t height = 480u;
    constexpr std::uint64_t repetitions = 4u;
    LinearMemoryDevice vram(dreamcast_vram_size);
    configure_background(
        registers, vram, target, width, height);
    write_texture(vram, texture);
    const auto frame = make_textured_frame(
        texture,
        static_cast<float>(width),
        static_cast<float>(height));
    PvrSoftwareRenderer renderer;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t generation = 1u;
         generation <= repetitions;
         ++generation)
        renderer.render(
            frame, registers.snapshot(), vram, generation);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    constexpr std::size_t tile_count = 20u * 15u;
    require(renderer.last_render_job_count() ==
                std::min(tile_count,
                         renderer.render_job_capacity()),
            "640x480-Benchmark nutzt nicht die erwartete "
            "Tile-/Hostkapazitaet.");
    std::ostringstream result;
    result << "elapsed_ms=" << elapsed
           << " jobs=" << renderer.last_render_job_count()
           << " digest=" << std::hex
           << digest_bytes(vram.bytes()) << std::dec
           << " pixels=" << renderer.metrics().pixels
           << " writes=" << renderer.metrics().pixel_writes;
    return result.str();
}

[[nodiscard]] int process_id() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

[[nodiscard]] std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

[[nodiscard]] std::string run_configuration(
    const std::filesystem::path& executable,
    const unsigned jobs) {
    const auto output =
        std::filesystem::temp_directory_path() /
        ("katana-pvr-parallel-" +
         std::to_string(process_id()) + "-" +
         std::to_string(jobs) + ".txt");
#if defined(_WIN32)
    const auto command =
        "set \"KATANA_RUNTIME_JOBS=" +
        std::to_string(jobs) + "\" && " +
        quote(executable) + " --child > " + quote(output);
#else
    const auto command =
        "KATANA_RUNTIME_JOBS=" + std::to_string(jobs) + " " +
        quote(executable) + " --child > " + quote(output);
#endif
    const auto result = std::system(command.c_str());
    require(result == 0,
            "PVR-Parallel-Childprozess ist fehlgeschlagen.");
    std::ifstream input(output, std::ios::binary);
    const std::string summary{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    require(!summary.empty(),
            "PVR-Parallel-Childprozess lieferte keinen Beleg.");
    return summary;
}
} // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc == 2 &&
            std::string_view(argv[1]) == "--child") {
            std::cout << run_child();
            return EXIT_SUCCESS;
        }
        if (argc == 2 &&
            std::string_view(argv[1]) == "--benchmark") {
            std::cout << run_benchmark() << '\n';
            return EXIT_SUCCESS;
        }
        require(argc >= 1,
                "Testprozess besitzt keinen Programmpfad.");
        const auto serial =
            run_configuration(
                std::filesystem::absolute(argv[0]), 1u);
        const auto parallel =
            run_configuration(
                std::filesystem::absolute(argv[0]), 24u);
        require(serial == parallel,
                "JOBS=1 und JOBS=N liefern verschiedene "
                "Byte-/Snapshot-/Metrik-Digests.");
        std::cout
            << "PVR-Tile-Rasterplan und serielle Fallbacks erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
