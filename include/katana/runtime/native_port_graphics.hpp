#pragma once

#include "katana/runtime/native_port.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_graphics_contract_version = 1u;

struct NativePortExtent final {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
};

struct NativePortAspectRatio final {
    std::uint32_t numerator = 16u;
    std::uint32_t denominator = 9u;
};

struct NativePortPixelRect final {
    std::uint32_t x = 0u;
    std::uint32_t y = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
};

enum class NativePortViewportPolicy : std::uint8_t {
    FullRender,
    FitAspect,
};

struct NativePortViewportConfig final {
    NativePortViewportPolicy policy = NativePortViewportPolicy::FullRender;
    NativePortAspectRatio aspect;
};

enum class NativePortCameraAspectPolicy : std::uint8_t {
    GameViewport,
    OutputSurface,
    Explicit,
};

struct NativePortGraphicsConfig final {
    std::uint32_t contract_version = native_port_graphics_contract_version;
    std::string_view title = "KatanaRecomp Native Port";
    // Output/window and internal render resolution are deliberately separate.
    // The 1920x1080 defaults are the v0.49.1 native-port baseline; a title can
    // select 4K or ultrawide output without changing render, camera or UI ABI.
    NativePortExtent output_extent{1'920u, 1'080u};
    NativePortExtent render_extent{1'920u, 1'080u};
    NativePortViewportConfig game_viewport{
        NativePortViewportPolicy::FullRender, {16u, 9u}};
    NativePortViewportConfig ui_viewport{
        NativePortViewportPolicy::FitAspect, {4u, 3u}};
    NativePortCameraAspectPolicy camera_aspect_policy =
        NativePortCameraAspectPolicy::GameViewport;
    NativePortAspectRatio explicit_camera_aspect{16u, 9u};
    bool initially_visible = true;
    bool resizable = true;
    bool synchronize_present = true;
    std::uint32_t maximum_textures = 4'096u;
    std::uint64_t maximum_texture_bytes = 1ull << 30u;
    std::uint32_t maximum_transient_vertices = 1'048'576u;
    std::uint32_t maximum_transient_indices = 3'145'728u;
};

struct NativePortGraphicsLayout final {
    NativePortExtent output_extent;
    NativePortExtent render_extent;
    NativePortPixelRect output_viewport;
    NativePortPixelRect game_viewport;
    NativePortPixelRect ui_viewport;
    float camera_aspect = 16.0f / 9.0f;
};

enum class NativePortGraphicsFailure : std::uint8_t {
    InvalidConfig,
    UnsupportedHost,
    WindowCreation,
    HardwareDeviceUnavailable,
    ShaderCompilation,
    ResourceCreation,
    ResourceLimit,
    InvalidResource,
    InvalidFrame,
    InvalidDraw,
    DeviceLost,
    ThreadViolation,
};

class NativePortGraphicsError final : public std::runtime_error {
  public:
    NativePortGraphicsError(NativePortGraphicsFailure failure,
                            std::uint32_t platform_error_code,
                            std::string_view operation);
    [[nodiscard]] NativePortGraphicsFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t platform_error_code() const noexcept;

  private:
    NativePortGraphicsFailure failure_;
    std::uint32_t platform_error_code_;
};

enum class NativePortTextureFormat : std::uint8_t {
    Rgba8Unorm,
    Bgra8Unorm,
};

struct NativePortTextureHandle final {
    std::uint64_t value = 0u;
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return value != 0u;
    }
    friend constexpr bool operator==(NativePortTextureHandle,
                                     NativePortTextureHandle) = default;
};

struct NativePortTextureConfig final {
    NativePortExtent extent;
    NativePortTextureFormat format = NativePortTextureFormat::Rgba8Unorm;
    bool shader_resource = true;
    bool dynamic = false;
};

struct NativePortImageView final {
    NativePortExtent extent;
    NativePortTextureFormat format = NativePortTextureFormat::Bgra8Unorm;
    std::uint32_t stride_bytes = 0u;
    bool bottom_up = false;
    std::span<const std::byte> pixels;
};

enum class NativePortViewportTarget : std::uint8_t {
    Game,
    Ui,
};

enum class NativePortImageFit : std::uint8_t {
    Contain,
    Cover,
    Stretch,
};

enum class NativePortPrimitiveTopology : std::uint8_t {
    TriangleList,
    TriangleStrip,
    LineList,
};

enum class NativePortBlendMode : std::uint8_t {
    Opaque,
    Alpha,
    Additive,
    Multiply,
};

enum class NativePortDepthMode : std::uint8_t {
    Disabled,
    ReadWrite,
    ReadOnly,
};

enum class NativePortCullMode : std::uint8_t {
    None,
    Front,
    Back,
};

enum class NativePortTextureFilter : std::uint8_t {
    Nearest,
    Linear,
};

enum class NativePortTextureAddress : std::uint8_t {
    Clamp,
    Wrap,
    Mirror,
};

struct NativePortVertex final {
    std::array<float, 3u> position{};
    std::array<float, 2u> texture_coordinate{};
    std::array<float, 4u> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct NativePortMatrix4x4 final {
    // Row-major. The shader evaluates row-vector position * transform.
    std::array<float, 16u> values{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

struct NativePortDrawPacket final {
    std::span<const NativePortVertex> vertices;
    std::span<const std::uint32_t> indices;
    NativePortMatrix4x4 transform;
    NativePortTextureHandle texture;
    NativePortViewportTarget viewport = NativePortViewportTarget::Game;
    NativePortPrimitiveTopology topology =
        NativePortPrimitiveTopology::TriangleList;
    NativePortBlendMode blend = NativePortBlendMode::Opaque;
    NativePortDepthMode depth = NativePortDepthMode::ReadWrite;
    NativePortCullMode cull = NativePortCullMode::Back;
    NativePortTextureFilter filter = NativePortTextureFilter::Linear;
    NativePortTextureAddress address = NativePortTextureAddress::Clamp;
};

struct NativePortFrameConfig final {
    std::array<float, 4u> clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;
};

struct NativePortGraphicsSnapshot final {
    std::uint64_t begun_frames = 0u;
    std::uint64_t presented_frames = 0u;
    std::uint64_t draw_calls = 0u;
    std::uint64_t uploaded_bytes = 0u;
    std::uint64_t swap_chain_resizes = 0u;
    std::uint32_t live_textures = 0u;
    std::uint32_t platform_error_code = 0u;
    bool hardware_accelerated = false;
    bool frame_open = false;
    bool occluded = false;
};

// Hardware-only native renderer used by title-side NINJA/Kamui/game-renderer
// adapters. It consumes native vertices, textures and draw state; it has no
// TA packet parser, PVR register model, framebuffer scanout or CPU rasterizer.
// All operations and destruction are confined to the construction thread.
class NativePortGraphicsDevice final {
  public:
    explicit NativePortGraphicsDevice(
        const NativePortGraphicsConfig& config = {});
    ~NativePortGraphicsDevice();

    NativePortGraphicsDevice(const NativePortGraphicsDevice&) = delete;
    NativePortGraphicsDevice& operator=(const NativePortGraphicsDevice&) = delete;
    NativePortGraphicsDevice(NativePortGraphicsDevice&&) = delete;
    NativePortGraphicsDevice& operator=(NativePortGraphicsDevice&&) = delete;

    void show();
    void poll_events();
    [[nodiscard]] NativePortLifecycleState lifecycle_state() const;
    [[nodiscard]] NativePortGraphicsLayout layout() const;

    [[nodiscard]] NativePortTextureHandle create_texture(
        const NativePortTextureConfig& config,
        const NativePortImageView* initial_pixels = nullptr);
    void update_texture(NativePortTextureHandle texture,
                        const NativePortImageView& pixels);
    void destroy_texture(NativePortTextureHandle texture);

    void begin_frame(const NativePortFrameConfig& config = {});
    void draw(const NativePortDrawPacket& packet);
    void present();

    // Native movie/UI convenience path. It uploads one verified host image,
    // draws it through the same GPU command path and presents it. No guest
    // framebuffer or synthetic scanout state is involved.
    void present_image(const NativePortImageView& image,
                       NativePortViewportTarget viewport =
                           NativePortViewportTarget::Game,
                       NativePortImageFit fit = NativePortImageFit::Contain);

    [[nodiscard]] NativePortGraphicsSnapshot snapshot() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Default desktop composition for generated native products. Future native
// input services share this window boundary; Dreamcast Maple state never does.
class NativePortDesktopHost final : public NativePortHostServices {
  public:
    explicit NativePortDesktopHost(
        const NativePortGraphicsConfig& graphics_config = {});
    ~NativePortDesktopHost() override;

    NativePortDesktopHost(const NativePortDesktopHost&) = delete;
    NativePortDesktopHost& operator=(const NativePortDesktopHost&) = delete;

    [[nodiscard]] std::uint64_t monotonic_time_nanoseconds()
        const noexcept override;
    [[nodiscard]] NativePortLifecycleState poll_lifecycle() override;
    void begin_frame(std::uint64_t frame_index) override;
    void present_frame(std::uint64_t frame_index) override;

    [[nodiscard]] NativePortGraphicsDevice& graphics() noexcept;
    [[nodiscard]] const NativePortGraphicsDevice& graphics() const noexcept;

  private:
    NativePortGraphicsDevice graphics_;
    std::uint64_t next_event_poll_nanoseconds_ = 0u;
};

} // namespace katana::runtime
