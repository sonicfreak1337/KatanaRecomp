#include "katana/runtime/native_port_graphics.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

void require(const bool value, const std::string& message) {
    if (value) return;
    std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

template <typename Callback>
bool throws_graphics(Callback&& callback,
                     const katana::runtime::NativePortGraphicsFailure failure,
                     const std::string& operation) {
    try {
        callback();
    } catch (const katana::runtime::NativePortGraphicsError& error) {
        return error.failure() == failure &&
               std::string(error.what()).find(operation) != std::string::npos;
    }
    return false;
}

katana::runtime::NativePortDrawPacket screen_packet(
    const std::span<const katana::runtime::NativePortVertex> vertices,
    const std::uint64_t batch_identity,
    const std::uint32_t submission_order) {
    using namespace katana::runtime;
    NativePortDrawPacket packet;
    packet.vertices = vertices;
    packet.vertex_space = NativePortVertexSpace::PvrScreenReciprocal;
    packet.interpolation = NativePortInterpolationMode::PvrScreenGouraud;
    packet.batch.identity = batch_identity;
    packet.batch.semantic = NativePortDrawBatchClass::Scene3D;
    packet.batch.submission_order = submission_order;
    packet.depth.compare = NativePortCompareOperation::GreaterEqual;
    packet.depth_mapping.mode =
        NativePortDepthCoordinateMode::ReciprocalPositive;
    packet.rasterizer.cull = NativePortCullMode::None;
    packet.rasterizer.depth_clip_enabled = false;
    return packet;
}

katana::runtime::NativePortDrawPacket object_packet(
    const std::span<const katana::runtime::NativePortVertex> vertices,
    const std::uint64_t batch_identity,
    const std::uint32_t submission_order) {
    using namespace katana::runtime;
    NativePortDrawPacket packet;
    packet.vertices = vertices;
    packet.vertex_space = NativePortVertexSpace::ObjectHomogeneous;
    packet.interpolation = NativePortInterpolationMode::PerspectiveCorrect;
    packet.viewport = NativePortViewportTarget::Ui;
    packet.batch.identity = batch_identity;
    packet.batch.semantic = NativePortDrawBatchClass::Scene3D;
    packet.batch.submission_order = submission_order;
    packet.depth.compare = NativePortCompareOperation::GreaterEqual;
    packet.depth_mapping.mode =
        NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip;
    packet.rasterizer.cull = NativePortCullMode::None;
    return packet;
}

katana::runtime::NativePortFrameConfig reciprocal_frame() {
    katana::runtime::NativePortFrameConfig frame;
    frame.clear_depth = 0.0f;
    frame.depth_buffer =
        katana::runtime::NativePortDepthBufferConvention::ReciprocalPositive;
    return frame;
}

} // namespace

int main(const int argc, char** const argv) {
#ifndef _WIN32
    return EXIT_SUCCESS;
#else
    using namespace katana::runtime;
    static_assert(native_port_graphics_contract_version == 13u);

    NativePortGraphicsConfig config;
    config.title = "Katana Native Graphics Contract Test";
    config.output_extent = {64u, 64u};
    config.render_extent = {64u, 64u};
    config.game_viewport.policy = NativePortViewportPolicy::FullRender;
    config.ui_viewport.policy = NativePortViewportPolicy::FullRender;
    config.initially_visible = false;
    config.synchronize_present = false;
    config.maximum_textures = 4u;
    config.maximum_texture_bytes = 1u << 20u;
    config.maximum_meshes = 4u;
    config.maximum_mesh_bytes = 1u << 20u;
    config.maximum_transient_vertices = 64u;
    config.maximum_transient_indices = 64u;
    config.maximum_pipeline_states = 64u;
    NativePortGraphicsDevice graphics(config);

    std::array<std::byte, 4u> pixel{
        std::byte{0x10u}, std::byte{0x20u}, std::byte{0x30u},
        std::byte{0xFFu}};
    NativePortImageView image;
    image.extent = {1u, 1u};
    image.format = NativePortTextureFormat::Rgba8Unorm;
    image.stride_bytes = 4u;
    image.pixels = pixel;
    NativePortTextureConfig texture_config;
    texture_config.extent = image.extent;
    texture_config.format = image.format;
    const auto first_texture = graphics.create_texture(texture_config, &image);
    graphics.destroy_texture(first_texture);
    const auto second_texture = graphics.create_texture(texture_config, &image);
    require(first_texture != second_texture,
            "Wiederverwendeter Texturslot behaelt seine alte Generation.");
    require(throws_graphics(
                [&] { graphics.update_texture(first_texture, image); },
                NativePortGraphicsFailure::InvalidResource,
                "texture-stale"),
            "Zerstoerter Texturhandle wird nach Slot-Reuse erneut akzeptiert.");
    graphics.destroy_texture(second_texture);

    const std::array screen_vertices{
        NativePortVertex{{-0.75f, -0.75f, 0.0f}},
        NativePortVertex{{0.75f, -0.75f, 0.0f}},
        NativePortVertex{{0.0f, 0.75f, 0.0f}},
    };
    auto reciprocal_vertices = screen_vertices;
    for (auto& vertex : reciprocal_vertices) vertex.depth_coordinate = 1.0f;
    const std::array object_vertices{
        NativePortVertex{{-0.5f, -0.5f, 0.0f}},
        NativePortVertex{{0.5f, -0.5f, 0.0f}},
        NativePortVertex{{0.0f, 0.5f, 0.0f}},
    };

    const auto benchmark = argc == 3 ? std::string_view(argv[1])
                                     : std::string_view{};
    if (!benchmark.empty()) {
        char* end = nullptr;
        const auto parsed = std::strtoull(argv[2], &end, 10);
        require(end != argv[2] && *end == '\0' && parsed != 0u &&
                    parsed <= std::numeric_limits<std::uint32_t>::max(),
                "Ungueltige Graphics-Benchmark-Iterationenzahl.");
        const auto iterations = static_cast<std::uint32_t>(parsed);
        if (benchmark == "--benchmark-layout") {
            std::uint64_t observed = 0u;
            const auto start = std::chrono::steady_clock::now();
            for (std::uint32_t iteration = 0u; iteration < iterations;
                 ++iteration) {
                const auto current = graphics.layout();
                observed += current.output_viewport.width;
                observed += current.game_viewport.height;
                observed += current.ui_viewport.x;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto elapsed_nanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count();
            require(observed != 0u,
                    "Layout-Benchmark wurde wegoptimiert.");
            std::cout << "KATANA_NATIVE_GRAPHICS_LAYOUT_BENCHMARK calls="
                      << iterations << " elapsed_ns=" << elapsed_nanoseconds
                      << " observed=" << observed << '\n';
            return EXIT_SUCCESS;
        }
        const bool object_draws = benchmark == "--benchmark-object-draws";
        const bool model_draws = benchmark == "--benchmark-model-draws";
        require(benchmark == "--benchmark-draws" || object_draws ||
                    model_draws,
                "Unbekannter Graphics-Benchmark-Modus.");
        NativePortMeshHandle benchmark_mesh;
        NativePortDrawPacket packet;
        if (model_draws) {
            NativePortMeshConfig mesh;
            mesh.vertices = object_vertices;
            benchmark_mesh = graphics.create_mesh(mesh);
            packet = object_packet({}, 1u, 1u);
            packet.mesh = benchmark_mesh;
        } else {
            packet = object_draws
                         ? object_packet(object_vertices, 1u, 1u)
                         : screen_packet(reciprocal_vertices, 1u, 1u);
        }
        if (object_draws || model_draws) {
            packet.viewport = NativePortViewportTarget::Game;
            packet.depth_mapping.mode =
                NativePortDepthCoordinateMode::ClipSpace;
            packet.depth.compare = NativePortCompareOperation::LessEqual;
            graphics.begin_frame();
        } else {
            graphics.begin_frame(reciprocal_frame());
        }
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t iteration = 0u; iteration < iterations;
             ++iteration) {
            packet.batch.submission_order = iteration + 1u;
            if (model_draws)
                packet.transform.values[12u] =
                    (iteration & 1u) != 0u ? 0.0001f : 0.0f;
            graphics.draw(packet);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count();
        const auto snapshot = graphics.snapshot();
        require(snapshot.draw_calls == iterations,
                "Draw-Benchmark hat nicht alle Draws ausgefuehrt.");
        require(!model_draws ||
                    snapshot.persistent_mesh_draw_calls == iterations,
                "Model-Benchmark hat den persistenten Meshpfad verlassen.");
        std::cout << "KATANA_NATIVE_GRAPHICS_BENCHMARK draws=" << iterations
                  << " elapsed_ns=" << elapsed_nanoseconds
                  << " uploaded_bytes=" << snapshot.uploaded_bytes << '\n';
        graphics.present();
        if (benchmark_mesh) graphics.destroy_mesh(benchmark_mesh);
        return EXIT_SUCCESS;
    }

    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 1u, 1u));
    graphics.draw(object_packet(object_vertices, 1u, 2u));
    require(graphics.snapshot().draw_calls == 2u,
            "Ein semantischer Batch akzeptiert keinen dynamischen Draw-State.");
    graphics.present();
    graphics.repeat_present();

    graphics.begin_frame(reciprocal_frame());
    auto flat_packet = screen_packet(reciprocal_vertices, 2u, 1u);
    flat_packet.rasterizer.shading = NativePortShadingMode::FlatLastVertex;
    graphics.draw(flat_packet);
    const auto draws_after_flat = graphics.snapshot().draw_calls;
    require(draws_after_flat == 3u,
            "Flat-Last-Vertex umgeht die erforderliche Vorverarbeitung.");
    graphics.present();

    graphics.begin_frame(reciprocal_frame());
    auto rejected_small_triangle =
        screen_packet(reciprocal_vertices, 3u, 1u);
    rejected_small_triangle.rasterizer.small_triangle_area_threshold =
        1'000.0f;
    graphics.draw(rejected_small_triangle);
    require(graphics.snapshot().draw_calls == draws_after_flat,
            "Small-Triangle-Filter verliert seine Vorverarbeitung.");
    graphics.present();

    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 4u, 1u));
    auto changed_semantic = object_packet(object_vertices, 4u, 2u);
    changed_semantic.batch.semantic = NativePortDrawBatchClass::UiOverlay;
    require(throws_graphics(
                [&] { graphics.draw(changed_semantic); },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-batch-contract"),
            "Ein Batch akzeptiert einen wechselnden semantischen Layer.");
    graphics.present();

    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 5u, 1u));
    require(throws_graphics(
                [&] {
                    graphics.draw(
                        screen_packet(reciprocal_vertices, 5u, 1u));
                },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-phase-order"),
            "Ein Batch akzeptiert eine doppelte Submission-Reihenfolge.");
    graphics.present();

    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 6u, 1u));
    require(throws_graphics(
                [&] {
                    graphics.draw(
                        screen_packet(reciprocal_vertices, 7u, 1u));
                },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-batch-order"),
            "Ein Frame akzeptiert mehrere Batches derselben Semantik.");
    graphics.present();

    return EXIT_SUCCESS;
#endif
}
