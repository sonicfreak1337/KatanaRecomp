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

inline constexpr std::uint32_t native_port_graphics_contract_version = 10u;
inline constexpr std::uint32_t native_port_frame_pacing_contract_version = 1u;

struct NativePortExtent final {
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;

    friend bool operator==(const NativePortExtent&,
                           const NativePortExtent&) = default;
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
    // Persistent meshes are immutable after creation. Geometry changes use a
    // destroy/create generation boundary so stale handles fail closed.
    std::uint32_t maximum_meshes = 65'536u;
    std::uint64_t maximum_mesh_bytes = 1ull << 30u;
    std::uint32_t maximum_transient_vertices = 1'048'576u;
    std::uint32_t maximum_transient_indices = 3'145'728u;
    // Shared upper bound for lazily materialized blend, depth, rasterizer and
    // sampler objects. The title supplies semantic state, never backend
    // handles; this budget keeps hostile or corrupt state churn fail-closed.
    std::uint32_t maximum_pipeline_states = 4'096u;
};

// Native game time and host presentation cadence are deliberately separate.
// One simulation frame advances title state; additional presentations only
// repeat the most recently completed GPU frame.  This permits 120/144-Hz
// output without making a 60-Hz title run two or more times too fast.
//
// The desktop host currently requires presentation_rate_hz to be at least
// simulation_rate_hz.  Lower output rates need an explicit frame-drop policy
// because silently blocking the simulation at the lower rate would change
// game speed.
struct NativePortFramePacingConfig final {
    std::uint32_t contract_version =
        native_port_frame_pacing_contract_version;
    std::uint32_t simulation_rate_hz = 60u;
    std::uint32_t presentation_rate_hz = 60u;
    bool enabled = true;
};

struct NativePortFramePacingSnapshot final {
    std::uint64_t simulation_frames = 0u;
    std::uint64_t presentation_frames = 0u;
    std::uint64_t repeated_presentations = 0u;
    std::uint64_t late_simulation_frames = 0u;
    std::uint64_t missed_presentation_deadlines = 0u;
    std::uint32_t simulation_rate_hz = 0u;
    std::uint32_t presentation_rate_hz = 0u;
    bool enabled = false;
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
    // Includes the top level. Every following level halves each dimension,
    // clamped to one pixel. Dynamic textures are intentionally single-level.
    std::uint32_t mip_levels = 1u;
    bool shader_resource = true;
    bool dynamic = false;
};

struct NativePortImageView final {
    NativePortExtent extent;
    NativePortTextureFormat format = NativePortTextureFormat::Bgra8Unorm;
    std::uint32_t stride_bytes = 0u;
    bool bottom_up = false;
    std::span<const std::byte> pixels;
    // Zero/zero uses extent.width:extent.height. Otherwise both values form
    // the intended display aspect for content with non-square source pixels.
    std::uint32_t display_aspect_numerator = 0u;
    std::uint32_t display_aspect_denominator = 0u;
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
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

enum class NativePortBlendFactor : std::uint8_t {
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    DestinationColor,
    InverseDestinationColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationAlpha,
    InverseDestinationAlpha,
};

enum class NativePortBlendOperation : std::uint8_t {
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
};

struct NativePortBlendState final {
    NativePortBlendFactor source_color = NativePortBlendFactor::One;
    NativePortBlendFactor destination_color = NativePortBlendFactor::Zero;
    NativePortBlendOperation color_operation = NativePortBlendOperation::Add;
    NativePortBlendFactor source_alpha = NativePortBlendFactor::One;
    NativePortBlendFactor destination_alpha = NativePortBlendFactor::Zero;
    NativePortBlendOperation alpha_operation = NativePortBlendOperation::Add;
    // RGBA bits 0..3. Other bits are invalid.
    std::uint8_t color_write_mask = 0x0Fu;
    bool enabled = false;
    friend bool operator==(const NativePortBlendState&,
                           const NativePortBlendState&) = default;
};

enum class NativePortCompareOperation : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

struct NativePortDepthState final {
    NativePortCompareOperation compare = NativePortCompareOperation::LessEqual;
    bool test_enabled = true;
    bool write_enabled = true;
    friend bool operator==(const NativePortDepthState&,
                           const NativePortDepthState&) = default;
};

// The coordinate space of NativePortVertex::position is part of the draw
// contract.  Adapters must not rely on a particular transform/depth-mode
// combination to imply whether vertices are local, already projected, or
// already homogeneous clip coordinates.
enum class NativePortVertexSpace : std::uint8_t {
    // position.xyz is local/object geometry.  The draw transform produces
    // homogeneous clip coordinates from float4(position, 1).
    ObjectHomogeneous,
    // position.xy is an already transformed logical raster coordinate and
    // depth_coordinate is a positive reciprocal depth.  The draw transform
    // performs viewport normalization only.
    PvrScreenReciprocal,
    // position.xyz plus position_w is already a homogeneous clip position.
    // The draw transform must be identity and is not applied by the shader.
    ClipHomogeneous,
};

// Semantic submission classes remain native renderer state.  They let title
// adapters preserve fixed-function list ownership and its special contracts
// without exposing a guest command stream or device protocol.
enum class NativePortDrawClass : std::uint8_t {
    Opaque,
    PunchThrough,
    Translucent,
    Overlay,
};

enum class NativePortInterpolationMode : std::uint8_t {
    // Ordinary GPU perspective-correct interpolation for homogeneous object
    // or clip geometry.
    PerspectiveCorrect,
    // Screen-space fixed-function Gouraud interpolation: attributes are
    // weighted by positive reciprocal depth and divided by the interpolated
    // reciprocal depth in the pixel stage.
    PvrScreenGouraud,
};

enum class NativePortDepthCoordinateMode : std::uint8_t {
    // Vertex position is ordinary homogeneous clip-space depth.
    ClipSpace,
    // A positive screen-space reciprocal-depth coordinate is interpolated
    // and logarithmically mapped in the pixel stage. This preserves useful
    // precision for native adapters whose source renderer emits 1/W after
    // transform, without exposing that renderer's device protocol.
    ReciprocalPositive,
    // The draw transform produces a genuine homogeneous clip-space position
    // whose W is positive view depth and whose Z encodes the native near
    // plane. The vertex stage derives reciprocal depth from clip W and passes
    // it explicitly without perspective interpolation. This avoids relying on
    // API-specific pixel-stage SV_Position.w semantics while still letting
    // host GPUs clip object geometry before rasterization, without forcing an
    // adapter to reproduce a guest renderer's CPU clipper.
    ReciprocalPositiveHomogeneousClip,
};

struct NativePortDepthMapping final {
    NativePortDepthCoordinateMode mode =
        NativePortDepthCoordinateMode::ClipSpace;
    float reciprocal_scale = 100'000.0f;
    float logarithm_divisor = 34.0f;
};

enum class NativePortDepthBufferConvention : std::uint8_t {
    // Ordinary D3D clip-space depth: near is zero, far is one.
    Forward,
    // Positive reciprocal depth: nearer fragments produce larger values.
    ReciprocalPositive,
};

enum class NativePortCullMode : std::uint8_t {
    None,
    Front,
    Back,
};

enum class NativePortFillMode : std::uint8_t {
    Solid,
    Wireframe,
};

enum class NativePortShadingMode : std::uint8_t {
    Smooth,
    // Fixed-function Dreamcast content uses the final submitted vertex as
    // the provoking vertex. Host APIs that use the first vertex therefore
    // need an explicit, backend-independent conversion rather than relying
    // on their native flat-shading convention.
    FlatLastVertex,
};

enum class NativePortTriangleAreaSpace : std::uint8_t {
    // The submitted vertex X/Y values already use the coordinate system in
    // which the threshold was defined.
    Submitted,
    // Apply the draw transform, perform the homogeneous divide, then map NDC
    // into small_triangle_reference_extent before measuring the determinant.
    // This lets fixed-function guests retain their logical raster threshold
    // independently of the host render/output resolution.
    LogicalViewportAfterTransform,
};

struct NativePortRasterizerState final {
    NativePortCullMode cull = NativePortCullMode::Back;
    NativePortFillMode fill = NativePortFillMode::Solid;
    NativePortShadingMode shading = NativePortShadingMode::Smooth;
    // Zero disables small-triangle rejection. A positive value rejects a
    // triangle when the absolute XY determinant in small_triangle_area_space
    // is below this threshold. Directional culling is applied independently
    // through cull.
    float small_triangle_area_threshold = 0.0f;
    NativePortTriangleAreaSpace small_triangle_area_space =
        NativePortTriangleAreaSpace::Submitted;
    // Required only for LogicalViewportAfterTransform. It describes the
    // guest renderer's logical raster surface, not the native output size.
    NativePortExtent small_triangle_reference_extent;
    bool front_counter_clockwise = false;
    bool depth_clip_enabled = true;
    friend bool operator==(const NativePortRasterizerState&,
                           const NativePortRasterizerState&) = default;
};

enum class NativePortTextureFilter : std::uint8_t {
    Point,
    Bilinear,
    Trilinear,
    Anisotropic,
};

enum class NativePortTextureAddress : std::uint8_t {
    Clamp,
    Wrap,
    Mirror,
};

struct NativePortSamplerState final {
    NativePortTextureFilter filter = NativePortTextureFilter::Bilinear;
    NativePortTextureAddress address_u = NativePortTextureAddress::Clamp;
    NativePortTextureAddress address_v = NativePortTextureAddress::Clamp;
    NativePortTextureAddress address_w = NativePortTextureAddress::Clamp;
    float mip_lod_bias = 0.0f;
    float minimum_lod = 0.0f;
    float maximum_lod = 1'000.0f;
    std::uint32_t maximum_anisotropy = 1u;
    friend bool operator==(const NativePortSamplerState&,
                           const NativePortSamplerState&) = default;
};

enum class NativePortTextureCombineMode : std::uint8_t {
    Modulate,
    Replace,
    Decal,
    Add,
    // RGB is modulated with the primary color while alpha is taken from the
    // texture. This is distinct from full modulation and is required by
    // fixed-function renderers whose shading instruction controls RGB and
    // alpha independently.
    ModulateTextureAlpha,
};

enum class NativePortTextureCoordinateSource : std::uint8_t {
    Vertex,
    NormalSphere,
};

struct NativePortMaterialState final {
    std::array<float, 4u> diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4u> ambient{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4u> specular{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4u> emission{0.0f, 0.0f, 0.0f, 0.0f};
    float specular_power = 1.0f;
    NativePortTextureCombineMode texture_combine =
        NativePortTextureCombineMode::Modulate;
    NativePortTextureCoordinateSource texture_coordinates =
        NativePortTextureCoordinateSource::Vertex;
    bool use_vertex_color = true;
    bool use_primary_alpha = true;
    bool use_texture_alpha = true;
    bool use_secondary_color = false;
    bool lighting_enabled = false;
    bool specular_enabled = false;
};

struct NativePortDirectionalLight final {
    // Direction from the shaded point towards the light, in the space
    // produced by normal_transform.
    std::array<float, 3u> direction{0.0f, 0.0f, 1.0f};
    std::array<float, 4u> color{1.0f, 1.0f, 1.0f, 1.0f};
};

inline constexpr std::size_t native_port_maximum_directional_lights = 4u;

struct NativePortLightingState final {
    std::array<float, 4u> ambient{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<NativePortDirectionalLight,
               native_port_maximum_directional_lights> lights{};
    std::uint32_t light_count = 0u;
};

enum class NativePortFogMode : std::uint8_t {
    Disabled,
    VertexFactor,
    Linear,
    Exponential,
    ExponentialSquared,
    // Bounded logarithmic lookup-table fog. The coordinate is multiplied by
    // density, mapped to one of 128 mantissa/exponent coefficients and
    // linearly interpolated. This retains a title renderer's authored fog
    // curve without exposing a guest register or device protocol.
    LookupTable,
    // The same bounded lookup is applied to the primary color before texture
    // combination instead of being blended into the final result.
    LookupTablePrimary,
};

inline constexpr std::size_t native_port_fog_table_entries = 128u;

struct NativePortFogState final {
    NativePortFogMode mode = NativePortFogMode::Disabled;
    std::array<float, 4u> color{0.0f, 0.0f, 0.0f, 1.0f};
    float start = 0.0f;
    float end = 1.0f;
    float density = 1.0f;
    std::array<float, native_port_fog_table_entries> lookup_table{};
};

// Fixed-function color clamping is an explicit RGBA stage.  It is applied
// after texture/offset composition and before ordinary fog, matching the
// authored renderer contract instead of relying on render-target saturation.
struct NativePortColorClampState final {
    std::array<float, 4u> minimum{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4u> maximum{1.0f, 1.0f, 1.0f, 1.0f};
    bool enabled = false;
};

struct NativePortAlphaTestState final {
    enum class Mode : std::uint8_t {
        // Compare the continuous shader alpha against reference.
        FloatingPoint,
        // Fixed-function punch-through coverage: quantize alpha to 8 bits,
        // compare GreaterEqual against reference_8bit, and force surviving
        // output alpha to one.
        Quantized8BitForceOpaque,
    };

    NativePortCompareOperation compare = NativePortCompareOperation::Always;
    float reference = 0.0f;
    bool enabled = false;
    Mode mode = Mode::FloatingPoint;
    std::uint8_t reference_8bit = 0u;
};

struct NativePortVertex final {
    std::array<float, 3u> position{};
    std::array<float, 2u> texture_coordinate{};
    std::array<float, 4u> color{1.0f, 1.0f, 1.0f, 1.0f};
    // Object-space by default. normal_transform places it into the lighting
    // and NormalSphere texture-coordinate space.
    std::array<float, 3u> normal{0.0f, 0.0f, 1.0f};
    std::array<float, 4u> secondary_color{0.0f, 0.0f, 0.0f, 0.0f};
    // VertexFactor consumes [0,1]; the other fog modes interpret this as a
    // non-negative distance/reciprocal-depth coordinate.
    float fog_coordinate = 0.0f;
    // Used only by ReciprocalPositive depth mapping. Kept separate from fog
    // so either semantic can be enabled independently.
    float depth_coordinate = 0.0f;
    // Used only by ClipHomogeneous.  It is deliberately separate and trailing
    // so existing object/screen-space aggregate initialization keeps W=1.
    float position_w = 1.0f;
};

struct NativePortMeshHandle final {
    std::uint64_t value = 0u;
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return value != 0u;
    }
    friend constexpr bool operator==(NativePortMeshHandle,
                                     NativePortMeshHandle) = default;
};

// Explicit immutable geometry ownership. Creation validates the complete
// source, performs only transform-independent flat-last/small-triangle
// conversion, and uploads persistent GPU buffers exactly once. Ordinary
// triangle strips remain strips when no conversion is required.
struct NativePortMeshConfig final {
    std::span<const NativePortVertex> vertices;
    std::span<const std::uint32_t> indices;
    NativePortPrimitiveTopology topology =
        NativePortPrimitiveTopology::TriangleList;
    NativePortShadingMode shading = NativePortShadingMode::Smooth;
    // Zero disables rejection. A positive threshold is measured only in the
    // submitted vertex XY space so it is independent of per-draw transforms.
    float small_triangle_area_threshold = 0.0f;
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
    // Mutually exclusive with vertices/indices. topology, shading and the
    // submitted-space small-triangle threshold must match the immutable mesh
    // creation contract; all remaining state stays dynamic per draw.
    NativePortMeshHandle mesh;
    NativePortVertexSpace vertex_space =
        NativePortVertexSpace::ObjectHomogeneous;
    NativePortDrawClass draw_class = NativePortDrawClass::Opaque;
    NativePortInterpolationMode interpolation =
        NativePortInterpolationMode::PerspectiveCorrect;
    NativePortMatrix4x4 transform;
    NativePortMatrix4x4 normal_transform;
    NativePortTextureHandle texture;
    NativePortViewportTarget viewport = NativePortViewportTarget::Game;
    NativePortPrimitiveTopology topology =
        NativePortPrimitiveTopology::TriangleList;
    NativePortBlendState blend;
    NativePortDepthState depth;
    NativePortDepthMapping depth_mapping;
    NativePortRasterizerState rasterizer;
    NativePortSamplerState sampler;
    NativePortMaterialState material;
    NativePortLightingState lighting;
    NativePortFogState fog;
    NativePortColorClampState color_clamp;
    NativePortAlphaTestState alpha_test;
};

struct NativePortFrameConfig final {
    std::array<float, 4u> clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;
    NativePortDepthBufferConvention depth_buffer =
        NativePortDepthBufferConvention::Forward;
};

struct NativePortGraphicsSnapshot final {
    std::uint64_t begun_frames = 0u;
    std::uint64_t presented_frames = 0u;
    std::uint64_t draw_calls = 0u;
    // All texture and geometry transfers combined.
    std::uint64_t uploaded_bytes = 0u;
    std::uint64_t transient_geometry_uploaded_bytes = 0u;
    std::uint64_t persistent_geometry_uploaded_bytes = 0u;
    std::uint64_t persistent_mesh_draw_calls = 0u;
    std::uint64_t swap_chain_resizes = 0u;
    std::uint64_t persistent_mesh_bytes = 0u;
    std::uint32_t live_textures = 0u;
    std::uint32_t live_meshes = 0u;
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
    [[nodiscard]] NativePortTextureHandle create_texture(
        const NativePortTextureConfig& config,
        std::span<const NativePortImageView> initial_mip_levels);
    void update_texture(NativePortTextureHandle texture,
                        const NativePortImageView& pixels);
    void update_texture(
        NativePortTextureHandle texture,
        std::span<const NativePortImageView> mip_levels);
    void destroy_texture(NativePortTextureHandle texture);

    [[nodiscard]] NativePortMeshHandle create_mesh(
        const NativePortMeshConfig& config);
    void destroy_mesh(NativePortMeshHandle mesh);

    void begin_frame(const NativePortFrameConfig& config = {});
    void draw(const NativePortDrawPacket& packet);
    void present();
    // Re-composite and present the last completed native GPU frame.  This is
    // presentation-only: it never opens a title frame or advances game state.
    void repeat_present();

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
        const NativePortGraphicsConfig& graphics_config = {},
        const NativePortFramePacingConfig& frame_pacing_config = {});
    ~NativePortDesktopHost() override;

    NativePortDesktopHost(const NativePortDesktopHost&) = delete;
    NativePortDesktopHost& operator=(const NativePortDesktopHost&) = delete;

    [[nodiscard]] std::uint64_t monotonic_time_nanoseconds()
        const noexcept override;
    [[nodiscard]] NativePortLifecycleState poll_lifecycle() override;
    void synchronize_simulation_boundary() override;
    void begin_frame(std::uint64_t frame_index) override;
    void present_frame(std::uint64_t frame_index) override;
    [[nodiscard]] std::uint64_t presented_frames()
        const noexcept override;
    [[nodiscard]] NativePortFramePacingSnapshot frame_pacing_snapshot()
        const noexcept;

    [[nodiscard]] NativePortGraphicsDevice& graphics() noexcept;
    [[nodiscard]] const NativePortGraphicsDevice& graphics() const noexcept;

  private:
    void paced_present();

    NativePortGraphicsDevice graphics_;
    NativePortFramePacingConfig frame_pacing_config_;
    NativePortFramePacingSnapshot frame_pacing_snapshot_;
    std::uint64_t next_event_poll_nanoseconds_ = 0u;
    std::uint64_t next_simulation_deadline_nanoseconds_ = 0u;
    std::uint64_t next_presentation_deadline_nanoseconds_ = 0u;
    std::uint64_t simulation_deadline_remainder_ = 0u;
    std::uint64_t presentation_deadline_remainder_ = 0u;
    bool frame_pacing_started_ = false;
};

} // namespace katana::runtime
