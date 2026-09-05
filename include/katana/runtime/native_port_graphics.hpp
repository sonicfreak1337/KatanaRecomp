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

// The texture provenance record carries the decoded-payload identity used by
// the cheap graphics diagnostics.  Keep this ABI version in lockstep with
// that public record so older producers cannot silently omit the identity.
inline constexpr std::uint32_t native_port_graphics_contract_version = 20u;
inline constexpr std::uint32_t native_port_frame_pacing_contract_version = 2u;
// Type-2 translucent packets are admitted only when the adapter and renderer
// agree on this small, address-agnostic contract.  The renderer owns the
// per-pixel ordering subpass, including the fixed-function blend factors of
// every fragment; the adapter identifies one authenticated auto-sorted PVR
// translucent list.  Host semantic batches and viewports may both contribute
// to that one fixed-function list and therefore must not create extra resolves.
inline constexpr std::uint32_t
    native_port_type2_autosort_contract_version = 3u;

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
    // Upper bound for the transient Type-2 per-pixel fragment pool. It is
    // allocated lazily only when an authenticated Type2AutoSorted list
    // arrives. The compact 24-byte nodes and this default consume no more than
    // Flycast's 512-MiB reference A-buffer budget. Capacity exhaustion is a
    // bounded quality limit: later fragments are discarded, never promoted to
    // a process-fatal graphics error.
    std::uint32_t maximum_type2_fragment_nodes = 22'369'621u;
    // The render-thread command stream is atomic at command boundaries: an
    // upload or draw is never split, redirected through a host pointer, or
    // silently executed on the producer. These explicit budgets size both
    // preallocated depth-2 arenas and must admit the largest command used by a
    // title configuration. The default covers the maximum default transient
    // vertex/index packet plus fixed command state.
    std::uint32_t maximum_render_commands_per_frame = 32'768u;
    std::uint32_t maximum_render_payload_bytes_per_frame =
        128u * 1024u * 1024u;
    // Resource uploads remain atomic and owning. This separate cap prevents a
    // title-wide texture/mesh budget from forcing gigabyte frame arenas.
    std::uint32_t maximum_render_resource_payload_bytes_per_command =
        64u * 1024u * 1024u;
    // Stable host directory used only as the initial location for the
    // development Save State / Load State dialogs. The title simulation owns
    // serialization and receives the selected path through a bounded mailbox.
    std::string_view development_state_directory;
    // Optional host-only aggregate telemetry owner. It must outlive the
    // device; the backend owner thread creates and retains its own writer.
    NativePortTelemetry* telemetry = nullptr;
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
    // Runtime presentation choices are bounded by the authenticated title
    // contract. Changing this value never changes the simulation cadence.
    std::uint32_t maximum_presentation_rate_hz = 1'000u;
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
    MissingRequiredTexture,
    RenderThreadContract,
};

[[nodiscard]] constexpr std::uint64_t native_port_graphics_operation_id(
    const std::string_view operation) noexcept {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto character : operation) {
        value ^= static_cast<std::uint8_t>(character);
        value *= 1099511628211ull;
    }
    return value;
}

class NativePortGraphicsError final : public std::runtime_error {
  public:
    NativePortGraphicsError(NativePortGraphicsFailure failure,
                             std::uint32_t platform_error_code,
                             std::string_view operation);
    NativePortGraphicsError(NativePortGraphicsFailure failure,
                            std::uint32_t platform_error_code,
                            std::uint64_t operation_id);
    [[nodiscard]] NativePortGraphicsFailure failure() const noexcept;
    [[nodiscard]] std::uint32_t platform_error_code() const noexcept;
    [[nodiscard]] std::uint64_t operation_id() const noexcept;

  private:
    NativePortGraphicsFailure failure_;
    std::uint32_t platform_error_code_;
    std::uint64_t operation_id_;
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

// A null texture handle is meaningful only for an authored untextured draw.
// RequiredResolved makes the adapter's texture obligation explicit, so a
// failed lookup cannot silently become the renderer's white fallback.
enum class NativePortTextureStage : std::uint8_t {
    Disabled,
    RequiredResolved,
};

using NativePortTexturePayloadSha256 = std::array<std::uint8_t, 32u>;

// Bounded diagnostic provenance for a host texture.  The graphics core never
// interprets this as a lookup key: resource admission remains the owner's
// responsibility.  It exists so a captured draw can be tied back to an exact
// content object without exposing a guest address or retaining a string.
struct NativePortTextureProvenance final {
    std::array<std::uint8_t, 32u> content_sha256{};
    // SHA-256 of a versioned canonical stream containing the source formats,
    // archive ordinal, decoded extents/mip chain and RGBA8 top/mip pixels.
    // It is computed once while an asset is decoded/materialized; draw paths
    // only copy or integer-mix this already-stored value.
    NativePortTexturePayloadSha256 decoded_rgba8_sha256{};
    std::uint64_t generation = 0u;
    std::uint32_t archive_ordinal = 0u;
    std::uint32_t global_index = 0u;
    std::uint8_t source_pixel_format = 0u;
    std::uint8_t source_data_format = 0u;
    NativePortExtent decoded_extent;
    std::uint32_t decoded_mip_levels = 0u;
    bool content_identity_bound = false;
    bool global_index_bound = false;
    bool decoded_payload_identity_bound = false;
};

struct NativePortTextureConfig final {
    NativePortExtent extent;
    NativePortTextureFormat format = NativePortTextureFormat::Rgba8Unorm;
    // Includes the top level. Every following level halves each dimension,
    // clamped to one pixel. Dynamic textures are intentionally single-level.
    std::uint32_t mip_levels = 1u;
    bool shader_resource = true;
    bool dynamic = false;
    NativePortTextureProvenance provenance;
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

// One fixed-function list phase contains at most one batch for each semantic
// layer, in this order.  Phases are submitted Opaque -> PunchThrough ->
// Translucent -> Overlay, and the semantic order restarts at each phase.  This
// matches PVR list composition while keeping host scene/UI ownership explicit.
enum class NativePortDrawBatchClass : std::uint8_t {
    Scene3D,
    GameOverlay,
    UiOverlay,
    FontOverlay,
};

enum class NativePortTranslucencyPolicy : std::uint8_t {
    NotApplicable,
    // Preserve the owner's authored order and explicit depth state.
    AuthoredUnsorted,
    // The adapter has already produced stable depth order.  The host color
    // pass therefore uses GEQUAL against opaque/punch depth and preserves the
    // packet's authored ZWriteDis state.
    StableDepthSorted,
    // Dreamcast/PVR Type-2 translucent list semantics.  The renderer gathers
    // qualifying fragments and resolves them per pixel from far to near.
    Type2AutoSorted,
};

struct NativePortType2AutosortContract final {
    std::uint32_t contract_version =
        native_port_type2_autosort_contract_version;
    // Zero is the only executable value: it means the source list is not
    // presorted and the renderer must perform Type-2 per-pixel autosort.
    // Other values are reserved and fail closed.
    std::uint32_t presort = 0u;
    friend bool operator==(const NativePortType2AutosortContract&,
                           const NativePortType2AutosortContract&) = default;
};

struct NativePortDrawBatch final {
    // Non-zero and stable for all draws in this semantic batch.  Batches from
    // different semantic layers may deliberately share an identity when an
    // authenticated fixed-function pass (notably one Type-2 list) owns both.
    std::uint64_t identity = 0u;
    // Strictly increasing within one draw class in the batch.  It preserves
    // ties without requiring the graphics device to guess submission order.
    std::uint32_t submission_order = 0u;
    NativePortDrawBatchClass semantic = NativePortDrawBatchClass::Scene3D;
};

enum class NativePortDrawLogicalUse : std::uint8_t {
    Unspecified,
    SceneMaterial,
    Interface,
    Font,
    Presentation,
};

// Optional, bounded causal metadata for graphics bring-up.  Adapters expose
// opaque identities rather than guest addresses; the renderer records these
// values but never uses them for admission or resource lookup.
enum class NativePortDrawOriginKind : std::uint8_t {
    Unspecified,
    Immediate,
    ModelMesh,
    Sprite,
    Font,
    Movie,
    Presentation,
};

enum class NativePortDrawIntent : std::uint8_t {
    Unspecified,
    SceneObject,
    Shadow,
    Interface,
    Font,
    Movie,
    Presentation,
};

enum class NativePortTextureResolverKind : std::uint8_t {
    Unspecified,
    NativeDescriptor,
    DynamicSurface,
    CachedArchive,
    ExactArchiveNames,
    ExactArchiveLayouts,
    PartialArchive,
    Checkpoint,
    RegisteredTexture,
    IdentityBoundOverride,
};

enum class NativePortTextureBindingWriterKind : std::uint8_t {
    Unspecified,
    TextureListBind,
    TextureNumberSelect,
    RegisteredTextureSelect,
    IdentityBoundOverride,
};

struct NativePortTextureBindingDiagnostics final {
    // Opaque adapter-local identities.  They are intentionally not lookup
    // keys and must not encode a public title address contract.
    std::uint64_t texture_list_identity = 0u;
    std::uint64_t texture_list_epoch = 0u;
    std::uint64_t last_writer_identity = 0u;
    std::uint64_t last_writer_sequence = 0u;
    std::uint64_t expected_asset_identity = 0u;
    std::uint64_t resolved_asset_identity = 0u;
    std::uint32_t texture_list_count = 0u;
    NativePortTextureResolverKind resolver =
        NativePortTextureResolverKind::Unspecified;
    NativePortTextureBindingWriterKind last_writer =
        NativePortTextureBindingWriterKind::Unspecified;
    bool texture_list_bound = false;
    bool texture_list_epoch_bound = false;
    bool last_writer_bound = false;
    bool expected_asset_bound = false;
    bool resolved_asset_bound = false;
};

// Numeric, bounded capture metadata only.  These values never participate in
// graphics admission or resource lookup and therefore cannot turn a title
// address into a public-core identity.
struct NativePortDrawDiagnostics final {
    std::uint64_t material_identity = 0u;
    std::uint64_t origin_identity = 0u;
    std::uint64_t model_identity = 0u;
    std::uint32_t texture_list_index = 0u;
    std::uint32_t mesh_index = 0u;
    std::uint32_t primitive_index = 0u;
    NativePortDrawLogicalUse logical_use =
        NativePortDrawLogicalUse::Unspecified;
    NativePortDrawOriginKind origin = NativePortDrawOriginKind::Unspecified;
    NativePortDrawIntent intent = NativePortDrawIntent::Unspecified;
    NativePortTextureBindingDiagnostics texture_binding;
    bool enabled = false;
    bool material_identity_bound = false;
    bool origin_identity_bound = false;
    bool model_identity_bound = false;
    bool texture_list_index_bound = false;
    bool mesh_index_bound = false;
    bool primitive_index_bound = false;
};

// Last fail-closed draw-contract violation observed by the renderer. This is
// deliberately a compact numeric witness rather than a drawstream: callers
// can attach it to crash/repro summaries even when validation stops the draw
// before texture resolution and normal graphics diagnostics begin.
struct NativePortGraphicsContractFailureWitness final {
    std::uint64_t frame = 0u;
    std::uint64_t draw_sequence = 0u;
    std::uint64_t batch_identity = 0u;
    NativePortDrawDiagnostics diagnostics;
    std::uint32_t submission_order = 0u;
    NativePortGraphicsFailure failure =
        NativePortGraphicsFailure::InvalidDraw;
    NativePortDrawBatchClass batch_semantic =
        NativePortDrawBatchClass::Scene3D;
    NativePortDrawClass draw_class = NativePortDrawClass::Opaque;
    bool valid = false;
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
    // plane. The vertex stage passes clip W as an ordinary perspective
    // interpolant and the pixel stage derives reciprocal depth from the
    // reconstructed value. This avoids relying on API-specific pixel-stage
    // SV_Position.w semantics while still letting host GPUs clip object
    // geometry before rasterization, without forcing an adapter to reproduce
    // a guest renderer's CPU clipper. Submitted vertices outside the
    // homogeneous clip volume, including vertices with W <= 0, are therefore
    // valid; only surviving fragments require positive W.
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
    // Persistent geometry keeps the complete authored primitive set. A
    // transform-dependent small-triangle decision belongs to a draw's logical
    // raster contract and therefore cannot be baked into this immutable mesh.
    // The only currently valid value is zero.
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
    NativePortDrawBatch batch;
    NativePortTranslucencyPolicy translucency =
        NativePortTranslucencyPolicy::NotApplicable;
    NativePortType2AutosortContract type2_autosort;
    NativePortDrawDiagnostics diagnostics;
    NativePortInterpolationMode interpolation =
        NativePortInterpolationMode::PerspectiveCorrect;
    NativePortMatrix4x4 transform;
    NativePortMatrix4x4 normal_transform;
    NativePortTextureHandle texture;
    NativePortTextureStage texture_stage = NativePortTextureStage::Disabled;
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

enum class NativePortGraphicsDiagnosticMode : std::uint8_t {
    Off,
    Digest,
    Breadcrumbs,
    ArmedCapture,
};

enum class NativePortGraphicsExecutionMode : std::uint8_t {
    Parallel,
    SerialReference,
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
    std::uint64_t diagnostic_digest = 0u;
    std::uint64_t diagnostic_draws = 0u;
    std::uint64_t diagnostic_drops = 0u;
    std::uint64_t contract_failures = 0u;
    NativePortGraphicsContractFailureWitness last_contract_failure;
    std::uint32_t live_textures = 0u;
    std::uint32_t live_meshes = 0u;
    std::uint32_t platform_error_code = 0u;
    NativePortGraphicsDiagnosticMode diagnostic_mode =
        NativePortGraphicsDiagnosticMode::Off;
    bool hardware_accelerated = false;
    bool frame_open = false;
    bool occluded = false;
    NativePortGraphicsExecutionMode requested_execution_mode =
        NativePortGraphicsExecutionMode::Parallel;
    NativePortGraphicsExecutionMode active_execution_mode =
        NativePortGraphicsExecutionMode::Parallel;
    std::uint64_t producer_thread_identity = 0u;
    std::uint64_t consumer_thread_identity = 0u;
    std::uint64_t recorded_commands = 0u;
    std::uint64_t consumed_commands = 0u;
    std::uint64_t executed_commands = 0u;
    std::uint64_t failed_commands = 0u;
    std::uint64_t skipped_commands = 0u;
    std::uint64_t last_recorded_queue_sequence = 0u;
    std::uint64_t last_consumed_queue_sequence = 0u;
    std::uint64_t last_executed_queue_sequence = 0u;
    std::uint64_t last_failed_queue_sequence = 0u;
    std::uint32_t last_failed_command_ordinal = 0u;
    // Sequence of the backend-owned snapshot/layout/lifecycle reply most
    // recently acquired by the producer. Zero denotes the startup snapshot.
    std::uint64_t backend_reply_sequence = 0u;
    std::uint64_t frame_queue_producer_position = 0u;
    std::uint64_t frame_queue_consumer_position = 0u;
    // Cumulative producer-side blocking. These are measured at real queue,
    // reply and open-frame resource fences; unavailable telemetry is never
    // represented as nominal work.
    std::uint64_t render_producer_wait_ns = 0u;
    std::uint64_t render_resource_fence_wait_ns = 0u;
    std::uint64_t resource_fence_count = 0u;
    std::uint64_t frame_prefix_publications = 0u;
};

// Hardware-only native renderer used by title-side NINJA/Kamui/game-renderer
// adapters. It consumes native vertices, textures and draw state; it has no
// TA packet parser, PVR register model, framebuffer scanout or CPU rasterizer.
// The public facade is confined to its construction/simulation-owner thread.
// In Parallel mode the backend is constructed, operated and destroyed only
// on the dedicated consumer thread; SerialReference uses the same codec on
// the facade owner as a diagnostic kill-switch.
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
    // Resolve and composite the currently queued Type-2 Scene3D packets.
    // The call is a scene boundary: after flushing, another Type-2 packet
    // must use a new batch identity. present() performs the same flush before
    // closing the frame.
    void flush_type2_translucency();
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

    // Explicit drain boundary for successful product termination. It rejects
    // an unsealed producer frame, retires every published reply, propagates
    // the final typed backend failure and acquires the final backend state.
    // The noexcept destructor remains an emergency cleanup fallback only.
    void finish();

    [[nodiscard]] NativePortGraphicsSnapshot snapshot() const;

    // Renderer-command completion of successful new frames that issued draws,
    // including offscreen/occluded completion. Not GPU-fence retirement or
    // visible display; excludes repeats, empty frames and aborted frames.
    // Nonblocking acquire of the last published consumer count.
    [[nodiscard]] std::uint64_t
    completed_drawn_frames_nonblocking() const noexcept;

  private:
    friend class NativePortDesktopHost;
    void configure_runtime_options(
        std::uint32_t simulation_rate_hz,
        std::uint32_t presentation_rate_hz,
        std::uint32_t maximum_presentation_rate_hz,
        bool frame_pacing_enabled);
    void record_simulation_frame_nonblocking() noexcept;
    [[nodiscard]] std::uint32_t
    requested_presentation_rate_nonblocking() const noexcept;
    void acknowledge_presentation_rate_nonblocking(
        std::uint32_t presentation_rate_hz) noexcept;
    void repeat_present_async();
    [[nodiscard]] std::uint64_t
    presented_frames_nonblocking() const noexcept;
    [[nodiscard]] std::uint64_t
    repeated_presentations_nonblocking() const noexcept;
    [[nodiscard]] bool frame_recording_open_nonblocking() const noexcept;
    [[nodiscard]] std::optional<NativePortDevelopmentStateRequest>
    take_development_state_request();
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
    [[nodiscard]] std::optional<NativePortDevelopmentStateRequest>
    take_development_state_request() override;
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
    void apply_runtime_presentation_rate();
    void paced_present();
    void reconcile_presentations() const noexcept;

    NativePortGraphicsDevice graphics_;
    NativePortFramePacingConfig frame_pacing_config_;
    mutable NativePortFramePacingSnapshot frame_pacing_snapshot_;
    mutable std::uint64_t accounted_presented_frames_ = 0u;
    mutable std::uint64_t accounted_repeated_presentations_ = 0u;
    std::uint64_t next_event_poll_nanoseconds_ = 0u;
    std::uint64_t next_simulation_deadline_nanoseconds_ = 0u;
    std::uint64_t next_presentation_deadline_nanoseconds_ = 0u;
    std::uint64_t simulation_deadline_remainder_ = 0u;
    std::uint64_t presentation_deadline_remainder_ = 0u;
    bool frame_pacing_started_ = false;
};

} // namespace katana::runtime
