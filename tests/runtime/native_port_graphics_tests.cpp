#include "katana/runtime/native_port_graphics.hpp"
#include "katana/runtime/native_port_telemetry.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

void require(const bool value, const std::string& message) {
    if (value) return;
    std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require_graphics_telemetry(
    const katana::runtime::NativePortTelemetrySnapshot& snapshot) {
    using namespace katana::runtime;
    const auto& render_submit = snapshot.stages[static_cast<std::size_t>(
        NativePortTelemetryStage::RenderSubmit)];
    const auto& present_wait = snapshot.stages[static_cast<std::size_t>(
        NativePortTelemetryStage::PresentWait)];
    const auto& gpu_time = snapshot.stages[static_cast<std::size_t>(
        NativePortTelemetryStage::GpuTime)];
    require(snapshot.render_queue_available &&
                snapshot.render_queue_depth == 0u &&
                snapshot.render_queue_high_watermark >= 1u,
            "Render-Queue-Telemetrie verlor Depth oder High-Watermark.");
    require(render_submit.available && render_submit.calls != 0u &&
                present_wait.available && present_wait.calls != 0u,
            "Consumer-Owner publizierte RenderSubmit/PresentWait nicht.");
    require(gpu_time.available
                ? gpu_time.calls != 0u &&
                      gpu_time.items == gpu_time.calls &&
                      gpu_time.time_ns != 0u
                : gpu_time.calls == 0u && gpu_time.items == 0u &&
                      gpu_time.time_ns == 0u,
            "GPU-Zeit war weder ein pensioniertes reales Query noch klar "
            "unavailable.");
}

template <typename Callback>
bool throws_graphics(Callback&& callback,
                     const katana::runtime::NativePortGraphicsFailure failure,
                     const std::string& operation) {
    try {
        callback();
    } catch (const katana::runtime::NativePortGraphicsError& error) {
        return error.failure() == failure &&
               error.operation_id() ==
                   katana::runtime::native_port_graphics_operation_id(operation);
    }
    return false;
}

template <typename Callback>
bool submits_frame_throws(
    katana::runtime::NativePortGraphicsDevice& graphics,
    Callback&& callback,
    const katana::runtime::NativePortGraphicsFailure failure,
    const std::string& operation) {
    try {
        callback();
    } catch (const katana::runtime::NativePortGraphicsError& error) {
        return error.failure() == failure &&
               error.operation_id() ==
                   katana::runtime::native_port_graphics_operation_id(operation);
    }
    try {
        graphics.present();
        static_cast<void>(graphics.snapshot());
    } catch (const katana::runtime::NativePortGraphicsError& error) {
        return error.failure() == failure &&
               error.operation_id() ==
                   katana::runtime::native_port_graphics_operation_id(operation);
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

katana::runtime::NativePortDrawPacket type_two_packet(
    const std::span<const katana::runtime::NativePortVertex> vertices,
    const std::uint64_t batch_identity,
    const std::uint32_t submission_order) {
    using namespace katana::runtime;
    auto packet = screen_packet(vertices, batch_identity, submission_order);
    packet.draw_class = NativePortDrawClass::Translucent;
    packet.translucency = NativePortTranslucencyPolicy::Type2AutoSorted;
    packet.blend.enabled = true;
    packet.blend.source_color = NativePortBlendFactor::SourceAlpha;
    packet.blend.destination_color = NativePortBlendFactor::InverseSourceAlpha;
    packet.blend.source_alpha = NativePortBlendFactor::SourceAlpha;
    packet.blend.destination_alpha = NativePortBlendFactor::InverseSourceAlpha;
    packet.blend.color_operation = NativePortBlendOperation::Add;
    packet.blend.alpha_operation = NativePortBlendOperation::Add;
    packet.blend.color_write_mask = 0x0Fu;
    packet.depth.test_enabled = true;
    packet.depth.write_enabled = false;
    packet.depth.compare = NativePortCompareOperation::GreaterEqual;
    packet.type2_autosort.contract_version =
        native_port_type2_autosort_contract_version;
    packet.type2_autosort.presort = 0u;
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

#ifdef _WIN32

class ScopedEnvironmentOverride final {
  public:
    ScopedEnvironmentOverride(std::string name, const std::string& value)
        : name_(std::move(name)) {
        char* previous = nullptr;
        std::size_t previous_size = 0u;
        require(_dupenv_s(&previous, &previous_size, name_.c_str()) == 0,
                "Test-Environment konnte nicht gelesen werden.");
        if (previous != nullptr) {
            previous_ = previous;
            std::free(previous);
        }
        require(_putenv_s(name_.c_str(), value.c_str()) == 0,
                "Test-Environment konnte nicht gesetzt werden.");
    }

    ~ScopedEnvironmentOverride() {
        static_cast<void>(_putenv_s(
            name_.c_str(), previous_.has_value() ? previous_->c_str() : ""));
    }

    ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
    ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) =
        delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

void require_autonomous_lifecycle_mailbox(
    const katana::runtime::NativePortGraphicsConfig& source_config,
    const bool parallel_expected) {
    using namespace katana::runtime;
    auto config = source_config;
    config.title = "Katana Autonomous Graphics Lifecycle";
    config.initially_visible = false;
    config.telemetry = nullptr;
    NativePortGraphicsDevice graphics(config);
    const auto before = graphics.snapshot();

    HWND window = nullptr;
    for (std::uint32_t attempt = 0u; attempt < 500u && window == nullptr;
         ++attempt) {
        window = FindWindowW(
            nullptr, L"Katana Autonomous Graphics Lifecycle");
        if (window == nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(window != nullptr,
            "Das Consumer-eigene Graphics-Fenster wurde nicht gefunden.");
    require(PostMessageW(
                window, WM_SIZE, SIZE_RESTORED,
                MAKELPARAM(80u, 72u)) != FALSE &&
                PostMessageW(window, WM_CLOSE, 0u, 0) != FALSE,
            "Lifecycle-Testnachrichten konnten nicht zugestellt werden.");

    if (!parallel_expected) graphics.poll_events();
    NativePortLifecycleState lifecycle = NativePortLifecycleState::Running;
    NativePortGraphicsLayout layout;
    for (std::uint32_t attempt = 0u; attempt < 500u; ++attempt) {
        lifecycle = graphics.lifecycle_state();
        layout = graphics.layout();
        if (lifecycle == NativePortLifecycleState::Shutdown &&
            layout.output_extent.width == 80u &&
            layout.output_extent.height == 72u)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(lifecycle == NativePortLifecycleState::Shutdown &&
                layout.output_extent.width == 80u &&
                layout.output_extent.height == 72u,
            "Lifecycle-/Resize-Mailbox wurde nicht autonom publiziert.");
    const auto after = graphics.snapshot();
    require(after.recorded_commands ==
                before.recorded_commands + (parallel_expected ? 0u : 1u),
            "Parallel-Lifecycle erzeugte einen PollEvents-Queue-Roundtrip.");
    graphics.finish();
}

[[nodiscard]] std::array<katana::runtime::NativePortVertex, 3u>
type_two_pixel_triangle(const float center_x, const float center_y) {
    using katana::runtime::NativePortVertex;
    constexpr float extent = 64.0f;
    const auto position = [](const float pixel_x, const float pixel_y) {
        return std::array{
            -1.0f + 2.0f * pixel_x / extent,
            1.0f - 2.0f * pixel_y / extent,
            0.0f};
    };
    std::array vertices{
        NativePortVertex{position(center_x - 3.0f, center_y + 3.0f)},
        NativePortVertex{position(center_x + 3.0f, center_y + 3.0f)},
        NativePortVertex{position(center_x, center_y - 3.0f)},
    };
    for (auto& vertex : vertices) vertex.depth_coordinate = 1.0f;
    return vertices;
}

void run_type_two_global_fragment_capture(
    const katana::runtime::NativePortGraphicsConfig& source_config,
    const bool parallel_expected) {
    using namespace katana::runtime;
    constexpr std::uint32_t extent = 64u;
    constexpr std::size_t triangle_count = 17u;
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("katana-type2-global-index-" + unique);
    {
        ScopedEnvironmentOverride background_test(
            "KATANA_PORT_BACKGROUND_TEST", "0");
        ScopedEnvironmentOverride capture_directory(
            "KATANA_NATIVE_GRAPHICS_CAPTURE_DIRECTORY", directory.string());
        ScopedEnvironmentOverride capture_start(
            "KATANA_NATIVE_GRAPHICS_CAPTURE_START_FRAME", "1");
        ScopedEnvironmentOverride capture_end(
            "KATANA_NATIVE_GRAPHICS_CAPTURE_END_FRAME", "1");
        ScopedEnvironmentOverride capture_interval(
            "KATANA_NATIVE_GRAPHICS_CAPTURE_INTERVAL", "1");

        auto capture_config = source_config;
        capture_config.title = "Katana Type2 Global Fragment Regression";
        capture_config.initially_visible = true;
        capture_config.synchronize_present = false;
        capture_config.maximum_type2_fragment_nodes = 8'192u;
        capture_config.telemetry = nullptr;
        NativePortGraphicsDevice capture_graphics(capture_config);

        std::array<std::array<NativePortVertex, 3u>, triangle_count>
            triangles{};
        std::array<std::array<std::uint32_t, 2u>, triangle_count>
            sample_pixels{};
        capture_graphics.begin_frame(reciprocal_frame());
        for (std::size_t index = 0u; index < triangles.size(); ++index) {
            const auto x = 6u + static_cast<std::uint32_t>(index % 6u) * 10u;
            const auto y = 7u + static_cast<std::uint32_t>(index / 6u) * 20u;
            sample_pixels[index] = {x, y};
            triangles[index] = type_two_pixel_triangle(
                static_cast<float>(x) + 0.5f,
                static_cast<float>(y) + 0.5f);
            capture_graphics.draw(type_two_packet(
                triangles[index], 0x7711u,
                static_cast<std::uint32_t>(index + 1u)));
        }
        capture_graphics.present();
        static_cast<void>(capture_graphics.snapshot());

        const auto bitmap_path = directory / "frame-1.bmp";
        std::ifstream bitmap(bitmap_path, std::ios::binary | std::ios::ate);
        require(bitmap.is_open(),
                "Type2-Pixelregression erzeugte kein BMP.");
        const auto bitmap_size =
            static_cast<std::streamoff>(bitmap.tellg());
        require(bitmap_size >=
                    static_cast<std::streamoff>(54u + extent * extent * 4u),
                "Type2-Capture besitzt kein vollstaendiges 64x64-BMP.");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(bitmap_size));
        bitmap.seekg(0, std::ios::beg);
        bitmap.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        require(bitmap.good(), "Type2-Capture konnte nicht gelesen werden.");
        require(bytes[0] == static_cast<std::uint8_t>('B') &&
                    bytes[1] == static_cast<std::uint8_t>('M'),
                "Type2-Capture ist kein BMP.");
        for (const auto& sample : sample_pixels) {
            const auto row = extent - sample[1] - 1u;
            const auto offset = 54u +
                (static_cast<std::size_t>(row) * extent + sample[0]) * 4u;
            require(bytes[offset + 0u] >= 0xC0u &&
                        bytes[offset + 1u] >= 0xC0u &&
                        bytes[offset + 2u] >= 0xC0u,
                    "Ein globaler Type2-Node >=16 wurde nicht composited.");
        }

        const auto overlapping = type_two_pixel_triangle(32.5f, 32.5f);
        capture_graphics.begin_frame(reciprocal_frame());
        for (std::uint32_t index = 0u; index < triangle_count; ++index)
            capture_graphics.draw(type_two_packet(
                overlapping, 0x7712u, index + 1u));
        require(throws_graphics(
                    [&] {
                        capture_graphics.present();
                        if (parallel_expected)
                            static_cast<void>(capture_graphics.snapshot());
                    },
                    NativePortGraphicsFailure::ResourceLimit,
                    "type2-fragment-overflow"),
                "17 Type2-Fragmente in einem Pixel wurden nicht fail-closed.");
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    require(!cleanup_error, "Type2-Capture-Tempverzeichnis blieb zurueck.");
}

#endif

} // namespace

int main(const int argc, char** const argv) {
#ifndef _WIN32
    return EXIT_SUCCESS;
#else
    using namespace katana::runtime;
    static_assert(native_port_graphics_contract_version == 18u);

    // The backend caches this gate on its first construction. Bind it before
    // even the invalid-config constructor probe so one-shot failure injection
    // is deterministic outside CTest as well.
    ScopedEnvironmentOverride background_test(
        "KATANA_PORT_BACKGROUND_TEST", "1");

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
    config.maximum_transient_vertices = 512u;
    config.maximum_transient_indices = 64u;
    config.maximum_pipeline_states = 64u;
    config.maximum_type2_fragment_nodes = 8'192u;
    config.maximum_render_commands_per_frame = 128u;
    config.maximum_render_payload_bytes_per_frame = 1u << 20u;
    config.maximum_render_resource_payload_bytes_per_command = 64u << 10u;
    NativePortTelemetry telemetry;
    config.telemetry = &telemetry;

    auto invalid_resource_capacity = config;
    invalid_resource_capacity.maximum_render_resource_payload_bytes_per_command =
        invalid_resource_capacity.maximum_render_payload_bytes_per_frame + 1u;
    require(throws_graphics(
                [&] {
                    NativePortGraphicsDevice invalid_graphics(
                        invalid_resource_capacity);
                },
                NativePortGraphicsFailure::InvalidConfig,
                "config"),
            "Eine atomare Resource-Payloadgrenze ausserhalb der Frame-Arena "
            "wurde akzeptiert.");

    NativePortGraphicsDevice graphics(config);

    const auto initial_execution = graphics.snapshot();
    const auto* const disable_render_thread =
        std::getenv("KATANA_PORT_DISABLE_RENDER_THREAD");
    const bool parallel_expected =
        disable_render_thread == nullptr ||
        std::string_view(disable_render_thread) != "1";
    const auto expected_mode =
        parallel_expected ? NativePortGraphicsExecutionMode::Parallel
                          : NativePortGraphicsExecutionMode::SerialReference;
    require(initial_execution.requested_execution_mode == expected_mode &&
                initial_execution.active_execution_mode == expected_mode,
            "Die Graphics-Fassade wechselte still den angeforderten Modus.");
    require(initial_execution.producer_thread_identity != 0u &&
                initial_execution.consumer_thread_identity != 0u &&
                (parallel_expected
                     ? initial_execution.producer_thread_identity !=
                           initial_execution.consumer_thread_identity
                     : initial_execution.producer_thread_identity ==
                           initial_execution.consumer_thread_identity),
            "Producer-/Consumer-Domains entsprechen nicht dem aktiven Modus.");
    require(initial_execution.recorded_commands == 0u &&
                initial_execution.consumed_commands == 0u &&
                initial_execution.executed_commands == 0u &&
                initial_execution.failed_commands == 0u &&
                initial_execution.skipped_commands == 0u,
            "Die Fassade meldet Kommandos vor dem ersten API-Aufruf.");

    require_autonomous_lifecycle_mailbox(config, parallel_expected);

    {
        auto finish_guard_config = config;
        finish_guard_config.title = "Katana Graphics Finish Guard";
        finish_guard_config.telemetry = nullptr;
        NativePortGraphicsDevice finish_guard(finish_guard_config);
        finish_guard.begin_frame(reciprocal_frame());
        require(throws_graphics(
                    [&] { finish_guard.finish(); },
                    NativePortGraphicsFailure::InvalidFrame,
                    "render-finish-open-frame"),
                "Finish publizierte still einen unversiegelten Frame.");
    }

    bool foreign_snapshot_rejected = false;
    std::thread foreign_snapshot([&] {
        foreign_snapshot_rejected = throws_graphics(
            [&] { static_cast<void>(graphics.snapshot()); },
            NativePortGraphicsFailure::ThreadViolation,
            "render-producer-thread");
    });
    foreign_snapshot.join();
    require(foreign_snapshot_rejected,
            "Ein fremder Thread durfte den Facade-Snapshot lesen.");
    const auto after_foreign_snapshot = graphics.snapshot();
    require(after_foreign_snapshot.recorded_commands == 0u &&
                after_foreign_snapshot.consumed_commands == 0u,
            "Ein abgewiesener Fremdthread mutierte die Command-Counter.");

    std::vector<std::byte> oversized_present_pixels(
        256u * 256u * 4u, std::byte{0x7Fu});
    NativePortImageView oversized_present_image;
    oversized_present_image.extent = {256u, 256u};
    oversized_present_image.format = NativePortTextureFormat::Rgba8Unorm;
    oversized_present_image.stride_bytes = 256u * 4u;
    oversized_present_image.pixels = oversized_present_pixels;
    const auto before_oversized_present = graphics.snapshot();
    require(throws_graphics(
                [&] {
                    graphics.present_image(
                        oversized_present_image,
                        NativePortViewportTarget::Game,
                        NativePortImageFit::Contain);
                },
                NativePortGraphicsFailure::ResourceLimit,
                "render-resource-command-capacity"),
            "PresentImage umging die atomare Resource-Payloadgrenze.");
    const auto after_oversized_present = graphics.snapshot();
    require(after_oversized_present.recorded_commands ==
                    before_oversized_present.recorded_commands &&
                after_oversized_present.presented_frames ==
                    before_oversized_present.presented_frames &&
                after_oversized_present.frame_queue_producer_position ==
                    before_oversized_present.frame_queue_producer_position &&
                after_oversized_present.frame_queue_consumer_position ==
                    before_oversized_present.frame_queue_consumer_position,
            "Abgewiesenes PresentImage mutierte Queue oder Presentation.");

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

    // A failed composite/DXGI attempt must revoke the previous repeatable
    // surface, abort every in-flight frame/Type-2 state and leave the backend
    // able to accept the next independent frame. Run the same proof through
    // Parallel and SerialReference.
    {
        ScopedEnvironmentOverride inject_present_failure(
            "KATANA_PORT_TEST_PRESENT_FAILURE", "1");
        auto failure_config = config;
        failure_config.title = "Katana Present Failure Recovery";
        failure_config.telemetry = nullptr;
        NativePortGraphicsDevice failure_graphics(failure_config);
        const auto before_failure = failure_graphics.snapshot();

        failure_graphics.begin_frame(reciprocal_frame());
        failure_graphics.draw(
            type_two_packet(reciprocal_vertices, 0x700u, 1u));
        bool present_failed = false;
        if (parallel_expected) {
            failure_graphics.present();
            present_failed = throws_graphics(
                [&] { failure_graphics.finish(); },
                NativePortGraphicsFailure::DeviceLost,
                "present");
        } else {
            present_failed = throws_graphics(
                [&] { failure_graphics.present(); },
                NativePortGraphicsFailure::DeviceLost,
                "present");
            failure_graphics.finish();
        }
        require(present_failed,
                "Der injizierte Present-Fehler erreichte den Producer nicht.");
        const auto failed_present = failure_graphics.snapshot();
        require(!failed_present.frame_open &&
                    failed_present.presented_frames ==
                        before_failure.presented_frames,
                "Fehlgeschlagenes Present publizierte einen fertigen Frame.");

        require(throws_graphics(
                    [&] { failure_graphics.repeat_present(); },
                    NativePortGraphicsFailure::InvalidFrame,
                    "repeat-without-completed-frame"),
                "RepeatPresent verwendete den fehlgeschlagenen Frame erneut.");
        const auto after_rejected_repeat = failure_graphics.snapshot();
        require(after_rejected_repeat.presented_frames ==
                    failed_present.presented_frames,
                "Abgewiesenes RepeatPresent erhoehte den Present-Zaehler.");

        failure_graphics.begin_frame(reciprocal_frame());
        failure_graphics.draw(
            screen_packet(reciprocal_vertices, 0x701u, 1u));
        failure_graphics.present();
        failure_graphics.finish();
        const auto recovered_present = failure_graphics.snapshot();
        require(!recovered_present.frame_open &&
                    recovered_present.begun_frames ==
                        after_rejected_repeat.begun_frames + 1u &&
                    recovered_present.draw_calls ==
                        after_rejected_repeat.draw_calls + 1u &&
                    (recovered_present.presented_frames ==
                         after_rejected_repeat.presented_frames + 1u ||
                     (recovered_present.occluded &&
                      recovered_present.presented_frames ==
                          after_rejected_repeat.presented_frames)) &&
                    recovered_present.executed_commands ==
                        after_rejected_repeat.executed_commands + 4u &&
                    recovered_present.failed_commands ==
                        after_rejected_repeat.failed_commands,
                "Der Backendzustand erholte sich nach Present-Fehler nicht.");
        failure_graphics.repeat_present();
        const auto repeated_recovered_frame = failure_graphics.snapshot();
        require(repeated_recovered_frame.executed_commands ==
                    recovered_present.executed_commands + 1u &&
                    repeated_recovered_frame.failed_commands ==
                        recovered_present.failed_commands,
                "Der erfolgreich wiederhergestellte Frame blieb nicht "
                "repeatierbar.");
    }

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
        const bool large_object_draws =
            benchmark == "--benchmark-large-object-draws";
        const bool large_model_draws =
            benchmark == "--benchmark-large-model-draws";
        const bool medium_object_draws =
            benchmark == "--benchmark-medium-object-draws";
        const bool medium_model_draws =
            benchmark == "--benchmark-medium-model-draws";
        const bool object_draws =
            benchmark == "--benchmark-object-draws" || large_object_draws ||
            medium_object_draws;
        const bool model_draws =
            benchmark == "--benchmark-model-draws" || large_model_draws ||
            medium_model_draws;
        require(benchmark == "--benchmark-draws" || object_draws ||
                    model_draws,
                "Unbekannter Graphics-Benchmark-Modus.");
        std::vector<NativePortVertex> large_vertices;
        if (large_object_draws || large_model_draws || medium_object_draws ||
            medium_model_draws) {
            // 74 expanded triangles / 222 vertices match a representative
            // r178 BasicAttach character mesh; eight triangles also measure
            // the fixed-overhead crossover. Keep both benchmark-only.
            const auto triangle_count =
                large_object_draws || large_model_draws ? 74u : 8u;
            large_vertices.reserve(triangle_count * 3u);
            for (std::uint32_t triangle = 0u; triangle < triangle_count;
                 ++triangle) {
                const auto x = static_cast<float>(triangle % 10u) * 0.01f;
                const auto y = static_cast<float>(triangle / 10u) * 0.01f;
                large_vertices.push_back(NativePortVertex{{x, y, 0.0f}});
                large_vertices.push_back(
                    NativePortVertex{{x + 0.005f, y, 0.0f}});
                large_vertices.push_back(
                    NativePortVertex{{x, y + 0.005f, 0.0f}});
            }
        }
        const std::span<const NativePortVertex> benchmark_vertices =
            large_vertices.empty()
                ? std::span<const NativePortVertex>(object_vertices)
                : std::span<const NativePortVertex>(large_vertices);
        NativePortMeshHandle benchmark_mesh;
        NativePortDrawPacket packet;
        if (model_draws) {
            NativePortMeshConfig mesh;
            mesh.vertices = benchmark_vertices;
            benchmark_mesh = graphics.create_mesh(mesh);
            packet = object_packet({}, 1u, 1u);
            packet.mesh = benchmark_mesh;
        } else {
            packet = object_draws
                         ? object_packet(benchmark_vertices, 1u, 1u)
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
        graphics.present();
        const auto snapshot = graphics.snapshot();
        require(snapshot.draw_calls == iterations,
                "Draw-Benchmark hat nicht alle Draws ausgefuehrt.");
        require(!model_draws ||
                    snapshot.persistent_mesh_draw_calls == iterations,
                "Model-Benchmark hat den persistenten Meshpfad verlassen.");
        std::cout << "KATANA_NATIVE_GRAPHICS_BENCHMARK draws=" << iterations
                  << " elapsed_ns=" << elapsed_nanoseconds
                  << " uploaded_bytes=" << snapshot.uploaded_bytes << '\n';
        if (benchmark_mesh) graphics.destroy_mesh(benchmark_mesh);
        return EXIT_SUCCESS;
    }

    // This GPU/readback regression deliberately precedes the Parallel-only
    // facade branch below so both CTest modes execute the same Type-2 proof.
    run_type_two_global_fragment_capture(config, parallel_expected);

    // SerialReference and Parallel share the exact same sealed ordinary-frame
    // codec contract.  No per-operation lease is permitted in either mode.
    const auto frame_boundary_texture =
        graphics.create_texture(texture_config, &image);
    const auto before_frame = graphics.snapshot();
    const auto publication_before_frame = telemetry.snapshot().publication;
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 0x701u, 1u));
    graphics.draw(screen_packet(reciprocal_vertices, 0x701u, 2u));
    const auto while_recording = graphics.snapshot();
    require(while_recording.frame_open &&
                while_recording.recorded_commands ==
                    before_frame.recorded_commands &&
                while_recording.frame_queue_producer_position ==
                    before_frame.frame_queue_producer_position,
            "Draw publizierte oder synchronisierte den offenen Frame.");
    graphics.present();
    const auto completed_frame = graphics.snapshot();
    const auto publication_after_frame = telemetry.snapshot().publication;
    require(completed_frame.draw_calls == before_frame.draw_calls + 2u &&
                completed_frame.recorded_commands ==
                    before_frame.recorded_commands + 4u &&
                completed_frame.consumed_commands ==
                    before_frame.consumed_commands + 4u &&
                completed_frame.executed_commands ==
                    before_frame.executed_commands + 4u &&
                completed_frame.frame_queue_producer_position ==
                    before_frame.frame_queue_producer_position + 1u &&
                completed_frame.frame_queue_consumer_position ==
                    completed_frame.frame_queue_producer_position &&
                completed_frame.backend_reply_sequence ==
                    completed_frame.frame_queue_consumer_position,
            "Begin/Draw/Draw/Present war kein einzelner FIFO-Frame.");
    require(publication_after_frame > publication_before_frame,
            "Der Consumer publizierte am Frame-Fence keine Telemetrie-Epoche.");
    graphics.destroy_texture(frame_boundary_texture);

    // Sonic legally creates, updates and retires lazy GPU resources between
    // NINJA submissions.  Each synchronous resource boundary retires the
    // ordered prefix while the logical backend frame stays open; following
    // draws and Present remain in that same frame in both execution modes.
    const auto before_resource_frame = graphics.snapshot();
    graphics.begin_frame(reciprocal_frame());
    const auto lazy_texture = graphics.create_texture(texture_config, &image);
    graphics.update_texture(lazy_texture, image);
    auto lazy_packet = screen_packet(reciprocal_vertices, 0x704u, 1u);
    lazy_packet.texture = lazy_texture;
    lazy_packet.texture_stage = NativePortTextureStage::RequiredResolved;
    graphics.draw(lazy_packet);
    graphics.destroy_texture(lazy_texture);
    require(graphics.snapshot().frame_open,
            "Ein synchroner Resource-Fence schloss den logischen Frame.");
    graphics.present();
    const auto completed_resource_frame = graphics.snapshot();
    require(completed_resource_frame.begun_frames ==
                    before_resource_frame.begun_frames + 1u &&
                completed_resource_frame.draw_calls ==
                    before_resource_frame.draw_calls + 1u &&
                completed_resource_frame.recorded_commands ==
                    before_resource_frame.recorded_commands + 6u &&
                completed_resource_frame.consumed_commands ==
                    before_resource_frame.consumed_commands + 6u &&
                completed_resource_frame.executed_commands ==
                    before_resource_frame.executed_commands + 6u &&
                completed_resource_frame.resource_fence_count ==
                    before_resource_frame.resource_fence_count + 3u &&
                completed_resource_frame.frame_prefix_publications ==
                    before_resource_frame.frame_prefix_publications + 2u &&
                completed_resource_frame.render_resource_fence_wait_ns >
                    before_resource_frame.render_resource_fence_wait_ns &&
                (parallel_expected
                     ? completed_resource_frame.render_producer_wait_ns >
                           before_resource_frame.render_producer_wait_ns
                     : completed_resource_frame.render_producer_wait_ns ==
                           before_resource_frame.render_producer_wait_ns) &&
                !completed_resource_frame.frame_open,
            "Open-frame Resource-Commands verloren Reihenfolge oder ACK.");

    NativePortMeshConfig rejected_lazy_mesh;
    rejected_lazy_mesh.vertices = object_vertices;
    rejected_lazy_mesh.small_triangle_area_threshold = 1.0f;
    const auto before_resource_fallback = graphics.snapshot();
    graphics.begin_frame(reciprocal_frame());
    require(throws_graphics(
                [&] {
                    static_cast<void>(graphics.create_mesh(rejected_lazy_mesh));
                },
                NativePortGraphicsFailure::InvalidResource,
                "mesh-transform-dependent-triangle-filter"),
            "Lazy-Mesh-Fehler verlor seine synchrone Resource-Reply.");
    require(graphics.snapshot().frame_open,
            "Fangbarer Resource-Fehler brach den offenen Frame ab.");
    graphics.draw(object_packet(object_vertices, 0x705u, 1u));
    graphics.present();
    const auto completed_resource_fallback = graphics.snapshot();
    require(completed_resource_fallback.begun_frames ==
                    before_resource_fallback.begun_frames + 1u &&
                completed_resource_fallback.draw_calls ==
                    before_resource_fallback.draw_calls + 1u &&
                completed_resource_fallback.failed_commands ==
                    before_resource_fallback.failed_commands + 1u &&
                !completed_resource_fallback.frame_open,
            "Lazy-Mesh-Fallback setzte den Frame nicht transient fort.");

    if (parallel_expected) {
        const auto before_failed_frame = graphics.snapshot();

        auto missing_texture =
            screen_packet(reciprocal_vertices, 0x702u, 1u);
        missing_texture.texture_stage =
            NativePortTextureStage::RequiredResolved;
        const auto presented_before_failed_frame =
            before_failed_frame.presented_frames;
        graphics.begin_frame(reciprocal_frame());
        graphics.draw(missing_texture);
        graphics.present();
        require(throws_graphics(
                    [&] { static_cast<void>(graphics.snapshot()); },
                    NativePortGraphicsFailure::MissingRequiredTexture,
                    "draw-texture-required"),
                "Der asynchrone Backendfehler verlor Operation oder Reply.");
        const auto failed_frame = graphics.snapshot();
        require(failed_frame.last_failed_command_ordinal == 1u &&
                    failed_frame.presented_frames ==
                        presented_before_failed_frame &&
                    failed_frame.recorded_commands ==
                        failed_frame.consumed_commands &&
                    failed_frame.recorded_commands ==
                        failed_frame.executed_commands +
                            failed_frame.failed_commands +
                            failed_frame.skipped_commands &&
                    failed_frame.failed_commands ==
                        before_failed_frame.failed_commands + 1u &&
                    failed_frame.skipped_commands ==
                        before_failed_frame.skipped_commands + 1u,
                "Der Framefehler verlor Ordinal, Skip oder fuehrte Present aus.");

        graphics.begin_frame(reciprocal_frame());
        graphics.draw(screen_packet(reciprocal_vertices, 0x703u, 1u));
        graphics.present();
        const auto recovered = graphics.snapshot();
        const auto recovered_exact =
            recovered.recorded_commands == recovered.consumed_commands &&
                    recovered.recorded_commands ==
                        recovered.executed_commands +
                            recovered.failed_commands +
                            recovered.skipped_commands &&
                    recovered.begun_frames ==
                        failed_frame.begun_frames + 1u &&
                    recovered.draw_calls == failed_frame.draw_calls + 1u &&
                    recovered.executed_commands ==
                        failed_frame.executed_commands + 3u &&
                    recovered.failed_commands ==
                        failed_frame.failed_commands &&
                    recovered.skipped_commands ==
                        failed_frame.skipped_commands &&
                    !recovered.frame_open &&
                    recovered.frame_queue_producer_position ==
                        recovered.frame_queue_consumer_position &&
                    recovered.producer_thread_identity !=
                        recovered.consumer_thread_identity;
        require(recovered_exact,
                "Die Parallel-Fassade erholte sich nicht deterministisch.");
        require_graphics_telemetry(telemetry.snapshot());
        return EXIT_SUCCESS;
    }

    const auto draw_texture = graphics.create_texture(texture_config, &image);
    NativePortMeshConfig near_clipped_mesh_config;
    near_clipped_mesh_config.vertices = object_vertices;
    const auto near_clipped_mesh =
        graphics.create_mesh(near_clipped_mesh_config);

    const auto before_serial_contract_failures = graphics.snapshot();
    graphics.begin_frame(reciprocal_frame());
    auto missing_required_texture =
        screen_packet(reciprocal_vertices, 1u, 1u);
    missing_required_texture.texture_stage =
        NativePortTextureStage::RequiredResolved;
    auto& missing_diagnostics = missing_required_texture.diagnostics;
    missing_diagnostics.enabled = true;
    missing_diagnostics.material_identity = 0x101u;
    missing_diagnostics.origin_identity = 0x202u;
    missing_diagnostics.model_identity = 0x303u;
    missing_diagnostics.material_identity_bound = true;
    missing_diagnostics.origin_identity_bound = true;
    missing_diagnostics.model_identity_bound = true;
    missing_diagnostics.texture_list_index = 3u;
    missing_diagnostics.texture_list_index_bound = true;
    auto& missing_binding = missing_diagnostics.texture_binding;
    missing_binding.texture_list_identity = 0x404u;
    missing_binding.texture_list_epoch = 7u;
    missing_binding.texture_list_count = 8u;
    missing_binding.last_writer_identity = 0x505u;
    missing_binding.last_writer_sequence = 9u;
    missing_binding.expected_asset_identity = 0x606u;
    missing_binding.resolver =
        NativePortTextureResolverKind::ExactArchiveNames;
    missing_binding.last_writer =
        NativePortTextureBindingWriterKind::TextureNumberSelect;
    missing_binding.texture_list_bound = true;
    missing_binding.texture_list_epoch_bound = true;
    missing_binding.last_writer_bound = true;
    missing_binding.expected_asset_bound = true;
    require(submits_frame_throws(
                graphics,
                [&] { graphics.draw(missing_required_texture); },
                NativePortGraphicsFailure::MissingRequiredTexture,
                "draw-texture-required"),
            "Eine fehlende Pflichttextur faellt auf den untexturierten Pfad zurueck.");
    const auto missing_snapshot = graphics.snapshot();
    const auto& missing_witness =
        missing_snapshot.last_contract_failure;
    require(missing_snapshot.contract_failures ==
                    before_serial_contract_failures.contract_failures + 1u &&
                missing_witness.valid &&
                missing_witness.failure ==
                    NativePortGraphicsFailure::MissingRequiredTexture &&
                missing_witness.frame ==
                    before_serial_contract_failures.begun_frames + 1u &&
                missing_witness.draw_sequence ==
                    before_serial_contract_failures.draw_calls + 1u &&
                missing_witness.batch_identity == 1u &&
                missing_witness.submission_order == 1u &&
                missing_witness.diagnostics.material_identity == 0x101u &&
                missing_witness.diagnostics.origin_identity == 0x202u &&
                missing_witness.diagnostics.model_identity == 0x303u &&
                missing_witness.diagnostics.texture_list_index == 3u &&
                missing_witness.diagnostics.texture_binding
                        .texture_list_identity == 0x404u &&
                missing_witness.diagnostics.texture_binding
                        .texture_list_epoch == 7u &&
                missing_witness.diagnostics.texture_binding
                        .last_writer_identity == 0x505u &&
                missing_witness.diagnostics.texture_binding
                        .last_writer_sequence == 9u &&
                missing_witness.diagnostics.texture_binding
                        .expected_asset_identity == 0x606u &&
                missing_witness.diagnostics.texture_binding.resolver ==
                    NativePortTextureResolverKind::ExactArchiveNames,
            "Der Pflichttexturfehler verlor seinen kompakten Material-/"
            "Texlist-/Last-Writer-Zeugen.");
    auto contradictory_disabled_texture =
        screen_packet(reciprocal_vertices, 1u, 1u);
    contradictory_disabled_texture.texture = draw_texture;
    graphics.begin_frame(reciprocal_frame());
    require(submits_frame_throws(
                graphics,
                [&] { graphics.draw(contradictory_disabled_texture); },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-texture-stage-disabled"),
            "Ein deaktivierter Texture-Stage-Vertrag akzeptiert einen Handle.");
    const auto invalid_draw_snapshot = graphics.snapshot();
    require(invalid_draw_snapshot.contract_failures ==
                    before_serial_contract_failures.contract_failures + 2u &&
                invalid_draw_snapshot.last_contract_failure.valid &&
                invalid_draw_snapshot.last_contract_failure.failure ==
                    NativePortGraphicsFailure::InvalidDraw &&
                invalid_draw_snapshot.last_contract_failure.frame ==
                    before_serial_contract_failures.begun_frames + 2u &&
                invalid_draw_snapshot.last_contract_failure.draw_sequence ==
                    before_serial_contract_failures.draw_calls + 2u &&
                invalid_draw_snapshot.last_contract_failure.batch_identity == 1u &&
                invalid_draw_snapshot.last_contract_failure.submission_order == 1u,
            "Ein allgemeiner Draw-Vertragsfehler wurde nicht zentral mit "
            "Frame-/Batch-/Submission-Zeugen gebunden.");
    auto resolved_required_texture =
        screen_packet(reciprocal_vertices, 1u, 1u);
    resolved_required_texture.texture = draw_texture;
    resolved_required_texture.texture_stage =
        NativePortTextureStage::RequiredResolved;
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(resolved_required_texture);
    graphics.present();
    graphics.destroy_texture(draw_texture);

    graphics.begin_frame(reciprocal_frame());
    auto disabled_fog_with_unused_invalid_table =
        screen_packet(reciprocal_vertices, 1u, 1u);
    disabled_fog_with_unused_invalid_table.fog.lookup_table.front() =
        std::numeric_limits<float>::quiet_NaN();
    graphics.draw(disabled_fog_with_unused_invalid_table);
    auto lookup_fog_with_invalid_table =
        screen_packet(reciprocal_vertices, 1u, 2u);
    lookup_fog_with_invalid_table.fog.mode = NativePortFogMode::LookupTable;
    lookup_fog_with_invalid_table.fog.lookup_table.front() =
        std::numeric_limits<float>::quiet_NaN();
    require(submits_frame_throws(
                graphics,
                [&] { graphics.draw(lookup_fog_with_invalid_table); },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-layout"),
            "Ein aktiver Lookup-Fog akzeptiert eine ungueltige Tabelle.");
    NativePortMeshConfig filtered_persistent_mesh;
    filtered_persistent_mesh.vertices = object_vertices;
    filtered_persistent_mesh.small_triangle_area_threshold = 1.0f;
    require(throws_graphics(
                [&] {
                    static_cast<void>(
                        graphics.create_mesh(filtered_persistent_mesh));
                },
                NativePortGraphicsFailure::InvalidResource,
                "mesh-transform-dependent-triangle-filter"),
            "Ein persistentes Mesh backt einen transformabhaengigen Filter ein.");

    const auto before_dynamic_batch = graphics.snapshot();
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 1u, 1u));
    graphics.draw(object_packet(object_vertices, 1u, 2u));
    auto near_clipped = object_packet(object_vertices, 1u, 3u);
    near_clipped.transform.values = {
        1.0f, 0.0f, 0.5f, 1.0f,
        0.0f, 0.25f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.125f, 0.25f};
    graphics.draw(near_clipped);

    auto persistent_near_clipped = object_packet({}, 1u, 4u);
    persistent_near_clipped.mesh = near_clipped_mesh;
    persistent_near_clipped.transform = near_clipped.transform;
    graphics.draw(persistent_near_clipped);
    graphics.present();
    require(graphics.snapshot().draw_calls ==
                before_dynamic_batch.draw_calls + 4u,
            "Ein semantischer Batch akzeptiert keinen dynamischen Draw-State.");
    graphics.destroy_mesh(near_clipped_mesh);
    graphics.repeat_present();

    const auto before_flat = graphics.snapshot();
    graphics.begin_frame(reciprocal_frame());
    auto flat_packet = screen_packet(reciprocal_vertices, 2u, 1u);
    flat_packet.rasterizer.shading = NativePortShadingMode::FlatLastVertex;
    graphics.draw(flat_packet);
    graphics.present();
    const auto draws_after_flat = graphics.snapshot().draw_calls;
    require(draws_after_flat == before_flat.draw_calls + 1u,
            "Flat-Last-Vertex umgeht die erforderliche Vorverarbeitung.");

    graphics.begin_frame(reciprocal_frame());
    auto rejected_small_triangle =
        screen_packet(reciprocal_vertices, 3u, 1u);
    rejected_small_triangle.rasterizer.small_triangle_area_threshold =
        1'000.0f;
    graphics.draw(rejected_small_triangle);
    graphics.present();
    require(graphics.snapshot().draw_calls == draws_after_flat,
            "Small-Triangle-Filter verliert seine Vorverarbeitung.");

    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 4u, 1u));
    auto changed_semantic = object_packet(object_vertices, 4u, 2u);
    changed_semantic.batch.semantic = NativePortDrawBatchClass::UiOverlay;
    require(submits_frame_throws(
                graphics,
                [&] { graphics.draw(changed_semantic); },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-batch-contract"),
            "Ein Batch akzeptiert einen wechselnden semantischen Layer.");
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 5u, 1u));
    require(submits_frame_throws(
                graphics,
                [&] {
                    graphics.draw(
                        screen_packet(reciprocal_vertices, 5u, 1u));
                },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-phase-order"),
            "Ein Batch akzeptiert eine doppelte Submission-Reihenfolge.");
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(screen_packet(reciprocal_vertices, 6u, 1u));
    require(submits_frame_throws(
                graphics,
                [&] {
                    graphics.draw(
                        screen_packet(reciprocal_vertices, 7u, 1u));
                },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-batch-order"),
            "Ein Frame akzeptiert mehrere Batches derselben Semantik.");
    graphics.begin_frame(reciprocal_frame());
    auto invalid_stable_translucent =
        screen_packet(reciprocal_vertices, 8u, 1u);
    invalid_stable_translucent.viewport = NativePortViewportTarget::Ui;
    invalid_stable_translucent.batch.semantic =
        NativePortDrawBatchClass::UiOverlay;
    invalid_stable_translucent.draw_class =
        NativePortDrawClass::Translucent;
    invalid_stable_translucent.translucency =
        NativePortTranslucencyPolicy::StableDepthSorted;
    invalid_stable_translucent.blend.enabled = true;
    invalid_stable_translucent.blend.source_color =
        NativePortBlendFactor::SourceAlpha;
    invalid_stable_translucent.blend.destination_color =
        NativePortBlendFactor::InverseSourceAlpha;
    invalid_stable_translucent.depth.test_enabled = true;
    invalid_stable_translucent.depth.write_enabled = true;
    require(submits_frame_throws(
                graphics,
                [&] { graphics.draw(invalid_stable_translucent); },
                NativePortGraphicsFailure::InvalidDraw,
                "draw-class-contract"),
            "StableDepthSorted akzeptiert einen schreibenden Depth-Pass.");
    auto stable_translucent = invalid_stable_translucent;
    stable_translucent.depth.write_enabled = false;
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(stable_translucent);
    graphics.present();

    // Type-2 is an explicit Scene3D contract.  Its two draws share one
    // semantic batch and are resolved at the boundary before presentation;
    // this also exercises the GPU-side PPLL resources and the opaque DSV
    // binding used by the capture pass.
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(type_two_packet(reciprocal_vertices, 9u, 1u));
    graphics.draw(type_two_packet(reciprocal_vertices, 9u, 2u));
    graphics.flush_type2_translucency();
    graphics.present();

    const auto expect_invalid_type_two = [&](
        const NativePortDrawPacket& packet,
        const std::string& message) {
        graphics.begin_frame(reciprocal_frame());
        require(submits_frame_throws(
                    graphics,
                    [&] { graphics.draw(packet); },
                    NativePortGraphicsFailure::InvalidDraw,
                    "draw-class-contract"),
                message);
    };

    auto invalid_type_two_operation =
        type_two_packet(reciprocal_vertices, 10u, 1u);
    invalid_type_two_operation.blend.color_operation =
        NativePortBlendOperation::ReverseSubtract;
    expect_invalid_type_two(
        invalid_type_two_operation,
        "Type2 akzeptiert eine nicht-additive Farboperation.");

    auto invalid_type_two_mask =
        type_two_packet(reciprocal_vertices, 11u, 1u);
    invalid_type_two_mask.blend.color_write_mask = 0x07u;
    expect_invalid_type_two(
        invalid_type_two_mask,
        "Type2 akzeptiert eine unvollstaendige Farbmaske.");

    auto invalid_type_two_disabled =
        type_two_packet(reciprocal_vertices, 12u, 1u);
    invalid_type_two_disabled.blend.enabled = false;
    expect_invalid_type_two(
        invalid_type_two_disabled,
        "Type2 akzeptiert deaktiviertes Blending.");

    // AuthoredUnsorted remains an independent normal translucent path; the
    // Type-2 validator must not accidentally broaden to it.
    auto authored_unsorted = type_two_packet(reciprocal_vertices, 13u, 1u);
    authored_unsorted.translucency =
        NativePortTranslucencyPolicy::AuthoredUnsorted;
    graphics.begin_frame(reciprocal_frame());
    graphics.draw(authored_unsorted);
    graphics.present();

    graphics.finish();
    const auto final_execution = graphics.snapshot();
    require(final_execution.requested_execution_mode == expected_mode &&
                final_execution.active_execution_mode == expected_mode,
            "Der Graphics-Modus wechselte waehrend der Session.");
    require(final_execution.recorded_commands ==
                    final_execution.consumed_commands &&
                final_execution.recorded_commands ==
                    final_execution.executed_commands +
                        final_execution.failed_commands +
                        final_execution.skipped_commands,
            "Recorded/consumed/executed/failed/skipped sind nicht exact-once.");
    require(final_execution.last_recorded_queue_sequence ==
                    final_execution.frame_queue_producer_position &&
                final_execution.last_consumed_queue_sequence ==
                    final_execution.frame_queue_consumer_position &&
                final_execution.frame_queue_producer_position ==
                    final_execution.frame_queue_consumer_position,
            "Queue-Sequenzen und Command-Counter sind auseinander gelaufen.");
    require(final_execution.producer_thread_identity != 0u &&
                final_execution.consumer_thread_identity != 0u &&
                (parallel_expected
                     ? final_execution.producer_thread_identity !=
                           final_execution.consumer_thread_identity
                     : final_execution.producer_thread_identity ==
                           final_execution.consumer_thread_identity),
            "Die Facade verlor ihre Thread-Domain-Bindung.");
    require_graphics_telemetry(telemetry.snapshot());

    return EXIT_SUCCESS;
#endif
}
