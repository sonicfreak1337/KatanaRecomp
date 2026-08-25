#include "katana/runtime/native_port_graphics.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <timeapi.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#endif

namespace katana::runtime {
namespace {

constexpr std::uint32_t maximum_graphics_dimension = 16'384u;
constexpr std::size_t maximum_graphics_title_bytes = 1'024u;
constexpr std::uint32_t maximum_native_frame_rate_hz = 1'000u;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000u;
constexpr std::uint32_t minimum_dynamic_vertex_buffer_bytes = 1u << 20u;
constexpr std::uint32_t minimum_dynamic_index_buffer_bytes = 1u << 18u;

[[nodiscard]] std::string copy_validated_graphics_title(
    const std::string_view title) {
    if (title.empty() || title.size() > maximum_graphics_title_bytes)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig, 0u, "title");
    return std::string(title);
}

[[nodiscard]] bool valid_extent(const NativePortExtent extent) noexcept {
    return extent.width != 0u && extent.height != 0u &&
           extent.width <= maximum_graphics_dimension &&
           extent.height <= maximum_graphics_dimension;
}

[[nodiscard]] bool valid_aspect(const NativePortAspectRatio aspect) noexcept {
    return aspect.numerator != 0u && aspect.denominator != 0u &&
           aspect.numerator <= 65'535u && aspect.denominator <= 65'535u;
}

[[nodiscard]] bool valid_viewport_policy(
    const NativePortViewportPolicy policy) noexcept {
    return policy == NativePortViewportPolicy::FullRender ||
           policy == NativePortViewportPolicy::FitAspect;
}

[[nodiscard]] bool valid_camera_policy(
    const NativePortCameraAspectPolicy policy) noexcept {
    return policy == NativePortCameraAspectPolicy::GameViewport ||
           policy == NativePortCameraAspectPolicy::OutputSurface ||
           policy == NativePortCameraAspectPolicy::Explicit;
}

[[nodiscard]] bool valid_texture_format(
    const NativePortTextureFormat format) noexcept {
    return format == NativePortTextureFormat::Rgba8Unorm ||
           format == NativePortTextureFormat::Bgra8Unorm;
}

[[nodiscard]] bool valid_viewport_target(
    const NativePortViewportTarget target) noexcept {
    return target == NativePortViewportTarget::Game ||
           target == NativePortViewportTarget::Ui;
}

[[nodiscard]] bool valid_image_fit(const NativePortImageFit fit) noexcept {
    return fit == NativePortImageFit::Contain ||
           fit == NativePortImageFit::Cover ||
           fit == NativePortImageFit::Stretch;
}

[[nodiscard]] bool valid_topology(
    const NativePortPrimitiveTopology topology) noexcept {
    return topology == NativePortPrimitiveTopology::PointList ||
           topology == NativePortPrimitiveTopology::LineList ||
           topology == NativePortPrimitiveTopology::LineStrip ||
           topology == NativePortPrimitiveTopology::TriangleList ||
           topology == NativePortPrimitiveTopology::TriangleStrip ||
           false;
}

template <std::size_t Size>
[[nodiscard]] bool finite_array(
    const std::array<float, Size>& values) noexcept {
    return std::all_of(values.begin(), values.end(), [](const float value) {
        return std::isfinite(value);
    });
}

[[nodiscard]] bool valid_blend_factor(
    const NativePortBlendFactor factor) noexcept {
    return factor == NativePortBlendFactor::Zero ||
           factor == NativePortBlendFactor::One ||
           factor == NativePortBlendFactor::SourceColor ||
           factor == NativePortBlendFactor::InverseSourceColor ||
           factor == NativePortBlendFactor::DestinationColor ||
           factor == NativePortBlendFactor::InverseDestinationColor ||
           factor == NativePortBlendFactor::SourceAlpha ||
           factor == NativePortBlendFactor::InverseSourceAlpha ||
           factor == NativePortBlendFactor::DestinationAlpha ||
           factor == NativePortBlendFactor::InverseDestinationAlpha;
}

[[nodiscard]] bool valid_blend_operation(
    const NativePortBlendOperation operation) noexcept {
    return operation == NativePortBlendOperation::Add ||
           operation == NativePortBlendOperation::Subtract ||
           operation == NativePortBlendOperation::ReverseSubtract ||
           operation == NativePortBlendOperation::Minimum ||
           operation == NativePortBlendOperation::Maximum;
}

[[nodiscard]] bool valid_blend(
    const NativePortBlendState& blend) noexcept {
    return valid_blend_factor(blend.source_color) &&
           valid_blend_factor(blend.destination_color) &&
           valid_blend_operation(blend.color_operation) &&
           valid_blend_factor(blend.source_alpha) &&
           valid_blend_factor(blend.destination_alpha) &&
           valid_blend_operation(blend.alpha_operation) &&
           (blend.color_write_mask & 0xF0u) == 0u;
}

[[nodiscard]] bool valid_compare(
    const NativePortCompareOperation compare) noexcept {
    return compare == NativePortCompareOperation::Never ||
           compare == NativePortCompareOperation::Less ||
           compare == NativePortCompareOperation::Equal ||
           compare == NativePortCompareOperation::LessEqual ||
           compare == NativePortCompareOperation::Greater ||
           compare == NativePortCompareOperation::NotEqual ||
           compare == NativePortCompareOperation::GreaterEqual ||
           compare == NativePortCompareOperation::Always;
}

[[nodiscard]] bool valid_depth(
    const NativePortDepthState& depth) noexcept {
    return valid_compare(depth.compare) &&
           (!depth.write_enabled || depth.test_enabled);
}

[[nodiscard]] bool valid_depth_mapping(
    const NativePortDepthMapping& mapping) noexcept {
    if (mapping.mode != NativePortDepthCoordinateMode::ClipSpace &&
        mapping.mode != NativePortDepthCoordinateMode::ReciprocalPositive &&
        mapping.mode !=
            NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip)
        return false;
    return std::isfinite(mapping.reciprocal_scale) &&
           std::isfinite(mapping.logarithm_divisor) &&
           mapping.reciprocal_scale > 0.0f &&
           mapping.logarithm_divisor > 0.0f;
}

[[nodiscard]] bool reciprocal_depth_mode(
    const NativePortDepthCoordinateMode mode) noexcept {
    return mode == NativePortDepthCoordinateMode::ReciprocalPositive ||
           mode ==
               NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip;
}

[[nodiscard]] bool valid_depth_buffer_convention(
    const NativePortDepthBufferConvention convention) noexcept {
    return convention == NativePortDepthBufferConvention::Forward ||
           convention ==
               NativePortDepthBufferConvention::ReciprocalPositive;
}

[[nodiscard]] bool compatible_depth_contract(
    const NativePortDepthBufferConvention convention,
    const NativePortDepthState& depth,
    const NativePortDepthMapping& mapping) noexcept {
    if (!depth.test_enabled) return true;
    const bool reciprocal = reciprocal_depth_mode(mapping.mode);
    if (reciprocal !=
        (convention ==
         NativePortDepthBufferConvention::ReciprocalPositive))
        return false;
    switch (depth.compare) {
    case NativePortCompareOperation::Less:
    case NativePortCompareOperation::LessEqual:
        return !reciprocal;
    case NativePortCompareOperation::Greater:
    case NativePortCompareOperation::GreaterEqual:
        return reciprocal;
    case NativePortCompareOperation::Never:
    case NativePortCompareOperation::Equal:
    case NativePortCompareOperation::NotEqual:
    case NativePortCompareOperation::Always:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_cull(const NativePortCullMode cull) noexcept {
    return cull == NativePortCullMode::None ||
           cull == NativePortCullMode::Front ||
           cull == NativePortCullMode::Back;
}

[[nodiscard]] bool valid_fill(const NativePortFillMode fill) noexcept {
    return fill == NativePortFillMode::Solid ||
           fill == NativePortFillMode::Wireframe;
}

[[nodiscard]] bool valid_shading(const NativePortShadingMode shading) noexcept {
    return shading == NativePortShadingMode::Smooth ||
           shading == NativePortShadingMode::FlatLastVertex;
}

[[nodiscard]] bool valid_triangle_area_space(
    const NativePortTriangleAreaSpace space) noexcept {
    return space == NativePortTriangleAreaSpace::Submitted ||
           space ==
               NativePortTriangleAreaSpace::LogicalViewportAfterTransform;
}

[[nodiscard]] bool valid_rasterizer(
    const NativePortRasterizerState& rasterizer) noexcept {
    return valid_cull(rasterizer.cull) && valid_fill(rasterizer.fill) &&
           valid_shading(rasterizer.shading) &&
           valid_triangle_area_space(rasterizer.small_triangle_area_space) &&
           std::isfinite(rasterizer.small_triangle_area_threshold) &&
           rasterizer.small_triangle_area_threshold >= 0.0f &&
           (rasterizer.small_triangle_area_threshold == 0.0f ||
            rasterizer.small_triangle_area_space ==
                NativePortTriangleAreaSpace::Submitted ||
            valid_extent(rasterizer.small_triangle_reference_extent));
}

[[nodiscard]] bool valid_filter(const NativePortTextureFilter filter) noexcept {
    return filter == NativePortTextureFilter::Point ||
           filter == NativePortTextureFilter::Bilinear ||
           filter == NativePortTextureFilter::Trilinear ||
           filter == NativePortTextureFilter::Anisotropic;
}

[[nodiscard]] bool valid_address(
    const NativePortTextureAddress address) noexcept {
    return address == NativePortTextureAddress::Clamp ||
           address == NativePortTextureAddress::Wrap ||
           address == NativePortTextureAddress::Mirror;
}

[[nodiscard]] bool valid_sampler(
    const NativePortSamplerState& sampler) noexcept {
    return valid_filter(sampler.filter) && valid_address(sampler.address_u) &&
           valid_address(sampler.address_v) &&
           valid_address(sampler.address_w) &&
           std::isfinite(sampler.mip_lod_bias) &&
           std::isfinite(sampler.minimum_lod) &&
           std::isfinite(sampler.maximum_lod) &&
           sampler.mip_lod_bias >= -16.0f &&
           sampler.mip_lod_bias <= 15.99f && sampler.minimum_lod >= 0.0f &&
           sampler.maximum_lod >= sampler.minimum_lod &&
           sampler.maximum_lod <= 65'536.0f &&
           sampler.maximum_anisotropy >= 1u &&
           sampler.maximum_anisotropy <= 16u &&
           (sampler.filter == NativePortTextureFilter::Anisotropic ||
            sampler.maximum_anisotropy == 1u);
}

[[nodiscard]] bool valid_texture_combine(
    const NativePortTextureCombineMode combine) noexcept {
    return combine == NativePortTextureCombineMode::Modulate ||
           combine == NativePortTextureCombineMode::Replace ||
           combine == NativePortTextureCombineMode::Decal ||
           combine == NativePortTextureCombineMode::Add ||
           combine == NativePortTextureCombineMode::ModulateTextureAlpha;
}

[[nodiscard]] bool valid_texture_coordinates(
    const NativePortTextureCoordinateSource source) noexcept {
    return source == NativePortTextureCoordinateSource::Vertex ||
           source == NativePortTextureCoordinateSource::NormalSphere;
}

[[nodiscard]] bool valid_material(
    const NativePortMaterialState& material) noexcept {
    return finite_array(material.diffuse) && finite_array(material.ambient) &&
           finite_array(material.specular) && finite_array(material.emission) &&
           std::isfinite(material.specular_power) &&
           material.specular_power >= 0.0f &&
           material.specular_power <= 65'536.0f &&
           valid_texture_combine(material.texture_combine) &&
           valid_texture_coordinates(material.texture_coordinates) &&
           (!material.specular_enabled || material.lighting_enabled);
}

[[nodiscard]] bool valid_lighting(
    const NativePortLightingState& lighting) noexcept {
    if (lighting.light_count > lighting.lights.size() ||
        !finite_array(lighting.ambient))
        return false;
    for (std::size_t index = 0u; index < lighting.light_count; ++index) {
        const auto& light = lighting.lights[index];
        if (!finite_array(light.direction) || !finite_array(light.color))
            return false;
        const auto length_squared =
            light.direction[0] * light.direction[0] +
            light.direction[1] * light.direction[1] +
            light.direction[2] * light.direction[2];
        if (!std::isfinite(length_squared) || length_squared <= 0.0f)
            return false;
    }
    return true;
}

[[nodiscard]] bool valid_fog_mode(const NativePortFogMode mode) noexcept {
    return mode == NativePortFogMode::Disabled ||
           mode == NativePortFogMode::VertexFactor ||
           mode == NativePortFogMode::Linear ||
           mode == NativePortFogMode::Exponential ||
           mode == NativePortFogMode::ExponentialSquared ||
           mode == NativePortFogMode::LookupTable ||
           mode == NativePortFogMode::LookupTablePrimary;
}

[[nodiscard]] bool valid_fog(const NativePortFogState& fog) noexcept {
    if (!valid_fog_mode(fog.mode) || !finite_array(fog.color) ||
        !std::isfinite(fog.start) || !std::isfinite(fog.end) ||
        !std::isfinite(fog.density) || fog.density < 0.0f ||
        !finite_array(fog.lookup_table))
        return false;
    if (fog.mode == NativePortFogMode::Linear && fog.end <= fog.start)
        return false;
    if (fog.mode != NativePortFogMode::LookupTable &&
        fog.mode != NativePortFogMode::LookupTablePrimary)
        return true;
    if (fog.density <= 0.0f) return false;
    return std::all_of(
        fog.lookup_table.begin(), fog.lookup_table.end(),
        [](const float value) { return value >= 0.0f && value <= 1.0f; });
}

[[nodiscard]] bool valid_alpha_test(
    const NativePortAlphaTestState& alpha_test) noexcept {
    return valid_compare(alpha_test.compare) &&
           std::isfinite(alpha_test.reference) &&
           alpha_test.reference >= 0.0f && alpha_test.reference <= 1.0f;
}

void validate_graphics_config(const NativePortGraphicsConfig& config) {
    if (config.contract_version != native_port_graphics_contract_version ||
        config.title.empty() ||
        config.title.size() > maximum_graphics_title_bytes ||
        !valid_extent(config.output_extent) ||
        !valid_extent(config.render_extent) ||
        !valid_viewport_policy(config.game_viewport.policy) ||
        !valid_viewport_policy(config.ui_viewport.policy) ||
        !valid_camera_policy(config.camera_aspect_policy) ||
        !valid_aspect(config.game_viewport.aspect) ||
        !valid_aspect(config.ui_viewport.aspect) ||
        !valid_aspect(config.explicit_camera_aspect) ||
        config.maximum_textures == 0u ||
        config.maximum_textures > 1'048'576u ||
        config.maximum_texture_bytes < 4u ||
        config.maximum_texture_bytes > 16ull * 1024u * 1024u * 1024u ||
        config.maximum_meshes == 0u ||
        config.maximum_meshes > 1'048'576u ||
        config.maximum_mesh_bytes < sizeof(NativePortVertex) ||
        config.maximum_mesh_bytes > 16ull * 1024u * 1024u * 1024u ||
        config.maximum_transient_vertices < 3u ||
        config.maximum_transient_indices < 3u ||
        config.maximum_pipeline_states == 0u ||
        config.maximum_pipeline_states > 65'536u ||
        config.maximum_transient_vertices >
            std::numeric_limits<std::uint32_t>::max() /
                sizeof(NativePortVertex) ||
        config.maximum_transient_indices >
            std::numeric_limits<std::uint32_t>::max() /
                sizeof(std::uint32_t))
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig, 0u, "config");
}

void validate_frame_pacing_config(
    const NativePortFramePacingConfig& config) {
    if (config.contract_version !=
            native_port_frame_pacing_contract_version ||
        config.simulation_rate_hz == 0u ||
        config.simulation_rate_hz > maximum_native_frame_rate_hz ||
        config.presentation_rate_hz < config.simulation_rate_hz ||
        config.presentation_rate_hz > maximum_native_frame_rate_hz)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig,
            0u,
            "frame-pacing-config");
}

[[nodiscard]] NativePortGraphicsConfig desktop_graphics_config(
    NativePortGraphicsConfig graphics,
    const NativePortFramePacingConfig& pacing) {
    validate_frame_pacing_config(pacing);
    // Software deadlines own cadence when pacing is active.  Combining them
    // with an unrelated monitor-vblank interval would double-throttle on
    // 50/60/120/144-Hz displays and make title time display-dependent.
    if (pacing.enabled) graphics.synchronize_present = false;
    return graphics;
}

void advance_frame_deadline(std::uint64_t& deadline,
                            std::uint64_t& remainder,
                            const std::uint32_t rate_hz) noexcept {
    const auto whole = nanoseconds_per_second / rate_hz;
    const auto fraction = nanoseconds_per_second % rate_hz;
    if (deadline > std::numeric_limits<std::uint64_t>::max() - whole) {
        deadline = std::numeric_limits<std::uint64_t>::max();
        remainder = 0u;
        return;
    }
    deadline += whole;
    remainder += fraction;
    if (remainder >= rate_hz) {
        remainder -= rate_hz;
        if (deadline != std::numeric_limits<std::uint64_t>::max()) ++deadline;
    }
}

void wait_until_monotonic_nanoseconds(const std::uint64_t deadline) {
    const auto maximum_count = static_cast<std::uint64_t>(
        std::numeric_limits<std::chrono::nanoseconds::rep>::max());
    const auto bounded = std::min(deadline, maximum_count);
    std::this_thread::sleep_until(
        std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(static_cast<
                std::chrono::nanoseconds::rep>(bounded))));
}

void acquire_frame_pacing_timer_resolution(
    const NativePortFramePacingConfig& config) {
#ifdef _WIN32
    if (config.enabled) {
        constexpr UINT requested_period_milliseconds = 1u;
        const auto result = timeBeginPeriod(requested_period_milliseconds);
        if (result != TIMERR_NOERROR)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::UnsupportedHost,
                static_cast<std::uint32_t>(result),
                "frame-pacing-timer-resolution");
    }
#else
    static_cast<void>(config);
#endif
}

void release_frame_pacing_timer_resolution(
    const NativePortFramePacingConfig& config) noexcept {
#ifdef _WIN32
    if (config.enabled) {
        constexpr UINT requested_period_milliseconds = 1u;
        static_cast<void>(timeEndPeriod(requested_period_milliseconds));
    }
#else
    static_cast<void>(config);
#endif
}

[[nodiscard]] NativePortPixelRect fit_aspect(
    const NativePortPixelRect outer,
    const NativePortAspectRatio aspect) noexcept {
    if (outer.width == 0u || outer.height == 0u ||
        !valid_aspect(aspect))
        return {};
    const auto available_scaled =
        static_cast<std::uint64_t>(outer.width) * aspect.denominator;
    const auto desired_scaled =
        static_cast<std::uint64_t>(outer.height) * aspect.numerator;
    NativePortPixelRect result = outer;
    if (available_scaled > desired_scaled) {
        result.width = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(
                1u,
                static_cast<std::uint64_t>(outer.height) *
                    aspect.numerator / aspect.denominator));
        result.x += (outer.width - result.width) / 2u;
    } else if (available_scaled < desired_scaled) {
        result.height = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(
                1u,
                static_cast<std::uint64_t>(outer.width) *
                    aspect.denominator / aspect.numerator));
        result.y += (outer.height - result.height) / 2u;
    }
    return result;
}

[[nodiscard]] NativePortPixelRect configured_viewport(
    const NativePortExtent render,
    const NativePortViewportConfig config) noexcept {
    const NativePortPixelRect full{0u, 0u, render.width, render.height};
    return config.policy == NativePortViewportPolicy::FullRender
               ? full
               : fit_aspect(full, config.aspect);
}

[[nodiscard]] NativePortExtent texture_mip_extent(
    const NativePortExtent extent,
    const std::uint32_t level) noexcept {
    return {std::max(extent.width >> level, 1u),
            std::max(extent.height >> level, 1u)};
}

[[nodiscard]] std::uint64_t checked_texture_bytes(
    const NativePortTextureConfig& config) {
    if (!valid_extent(config.extent) || config.mip_levels == 0u ||
        config.mip_levels > static_cast<std::uint32_t>(
                                std::bit_width(std::max(
                                    config.extent.width,
                                    config.extent.height))) ||
        (config.dynamic && config.mip_levels != 1u))
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource, 0u, "texture-extent");
    std::uint64_t result = 0u;
    for (std::uint32_t level = 0u; level < config.mip_levels; ++level) {
        const auto extent = texture_mip_extent(config.extent, level);
        const auto level_bytes =
            static_cast<std::uint64_t>(extent.width) * extent.height * 4u;
        if (level_bytes > std::numeric_limits<std::uint64_t>::max() - result)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::InvalidResource,
                0u,
                "texture-byte-size");
        result += level_bytes;
    }
    return result;
}

void validate_image(const NativePortImageView& image,
                    const NativePortExtent expected,
                    const NativePortTextureFormat expected_format) {
    if (!valid_extent(image.extent) || image.extent.width != expected.width ||
        image.extent.height != expected.height ||
        !valid_texture_format(image.format) ||
        !valid_texture_format(expected_format) ||
        image.format != expected_format)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource, 0u, "image-layout");
    const auto row_bytes = static_cast<std::uint64_t>(image.extent.width) * 4u;
    if (image.stride_bytes < row_bytes)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource, 0u, "image-stride");
    const auto required =
        static_cast<std::uint64_t>(image.stride_bytes) *
            (image.extent.height - 1u) +
        row_bytes;
    if (required > image.pixels.size())
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource, 0u, "image-bytes");
    const bool implicit_aspect = image.display_aspect_numerator == 0u &&
                                 image.display_aspect_denominator == 0u;
    const bool explicit_aspect = valid_aspect(
        {image.display_aspect_numerator,
         image.display_aspect_denominator});
    if (!implicit_aspect && !explicit_aspect)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource,
            0u,
            "image-display-aspect");
}

void validate_texture_images(
    const NativePortTextureConfig& config,
    const std::span<const NativePortImageView> images) {
    if (images.empty()) return;
    if (images.size() != config.mip_levels)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidResource,
            0u,
            "texture-mip-count");
    for (std::uint32_t level = 0u; level < config.mip_levels; ++level)
        validate_image(images[level],
                       texture_mip_extent(config.extent, level),
                       config.format);
}

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void saturating_add(std::uint64_t& value, const std::uint64_t addition) noexcept {
    value = addition > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + addition;
}

} // namespace

#ifdef _WIN32

namespace {

using Microsoft::WRL::ComPtr;

extern const wchar_t native_graphics_window_class[];

constexpr std::uint32_t draw_flag_vertex_color = 1u << 0u;
constexpr std::uint32_t draw_flag_secondary_color = 1u << 1u;
constexpr std::uint32_t draw_flag_lighting = 1u << 2u;
constexpr std::uint32_t draw_flag_specular = 1u << 3u;
constexpr std::uint32_t draw_flag_normal_sphere = 1u << 4u;
constexpr std::uint32_t draw_flag_reciprocal_depth = 1u << 5u;
constexpr std::uint32_t draw_flag_primary_alpha = 1u << 6u;
constexpr std::uint32_t draw_flag_texture_alpha = 1u << 7u;
constexpr std::uint32_t draw_texture_combine_shift = 8u;
constexpr std::uint32_t draw_alpha_test_enabled = 1u << 8u;
constexpr std::uint32_t draw_flag_homogeneous_reciprocal_clip = 1u << 16u;
constexpr std::uint32_t draw_flag_texture_present = 1u << 17u;
[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const char* entry,
                                               const char* target);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view value);
[[nodiscard]] DXGI_FORMAT texture_format(
    NativePortTextureFormat format) noexcept;
[[nodiscard]] D3D11_PRIMITIVE_TOPOLOGY primitive_topology(
    NativePortPrimitiveTopology topology) noexcept;
[[nodiscard]] D3D11_BLEND blend_factor(
    NativePortBlendFactor factor) noexcept;
[[nodiscard]] D3D11_BLEND_OP blend_operation(
    NativePortBlendOperation operation) noexcept;
[[nodiscard]] D3D11_COMPARISON_FUNC comparison_function(
    NativePortCompareOperation compare) noexcept;
[[nodiscard]] D3D11_TEXTURE_ADDRESS_MODE texture_address_mode(
    NativePortTextureAddress address) noexcept;
[[nodiscard]] D3D11_FILTER texture_filter(
    NativePortTextureFilter filter) noexcept;
[[nodiscard]] UINT next_buffer_capacity(UINT required) noexcept;

} // namespace

class NativePortGraphicsDevice::Impl final {
  private:
    struct GeometryCapabilities final {
        bool positive_depth_coordinates = true;
        bool nonnegative_fog_coordinates = true;
        bool nonzero_normals = true;
    };

    struct MeshSlot final {
        ComPtr<ID3D11Buffer> vertex_buffer;
        ComPtr<ID3D11Buffer> index_buffer;
        NativePortPrimitiveTopology source_topology =
            NativePortPrimitiveTopology::TriangleList;
        NativePortPrimitiveTopology gpu_topology =
            NativePortPrimitiveTopology::TriangleList;
        NativePortShadingMode source_shading =
            NativePortShadingMode::Smooth;
        float source_small_triangle_area_threshold = 0.0f;
        GeometryCapabilities capabilities;
        std::uint64_t byte_size = 0u;
        std::uint32_t vertex_count = 0u;
        std::uint32_t index_count = 0u;
        std::uint32_t generation = 0u;
        bool live = false;
    };

  public:
    explicit Impl(const NativePortGraphicsConfig& config)
        : title_storage_(copy_validated_graphics_title(config.title)),
          config_(config), owner_thread_(std::this_thread::get_id()) {
        config_.title = title_storage_;
        validate_graphics_config(config_);
        initialize_frame_capture();
        create_window();
        try {
            create_device();
            create_pipeline();
            create_render_surface();
            create_white_texture();
        } catch (...) {
            destroy_window();
            throw;
        }
        if (config_.initially_visible) show();
    }

    ~Impl() noexcept {
        if (std::this_thread::get_id() != owner_thread_) std::terminate();
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
        destroy_window();
    }

    void show() {
        require_owner_thread();
        if (window_ == nullptr)
            fail(NativePortGraphicsFailure::WindowCreation, 0u, "window-closed");
        ShowWindow(window_, SW_SHOWNORMAL);
        if (IsWindowVisible(window_) == FALSE)
            ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
    }

    void poll_events() {
        require_owner_thread();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        apply_pending_resize();
    }

    [[nodiscard]] NativePortLifecycleState lifecycle_state() const {
        require_owner_thread();
        if (close_requested_ || window_ == nullptr)
            return NativePortLifecycleState::Shutdown;
        if (minimized_) return NativePortLifecycleState::Paused;
        return NativePortLifecycleState::Running;
    }

    [[nodiscard]] NativePortGraphicsLayout layout() const {
        require_owner_thread();
        NativePortGraphicsLayout result;
        result.output_extent = output_extent_;
        result.render_extent = config_.render_extent;
        result.output_viewport = fit_aspect(
            {0u, 0u, output_extent_.width, output_extent_.height},
            {config_.render_extent.width, config_.render_extent.height});
        result.game_viewport =
            configured_viewport(config_.render_extent, config_.game_viewport);
        result.ui_viewport =
            configured_viewport(config_.render_extent, config_.ui_viewport);
        switch (config_.camera_aspect_policy) {
        case NativePortCameraAspectPolicy::GameViewport:
            result.camera_aspect =
                result.game_viewport.height != 0u
                    ? static_cast<float>(result.game_viewport.width) /
                          static_cast<float>(result.game_viewport.height)
                    : 1.0f;
            break;
        case NativePortCameraAspectPolicy::OutputSurface:
            result.camera_aspect =
                output_extent_.height != 0u
                    ? static_cast<float>(output_extent_.width) /
                          static_cast<float>(output_extent_.height)
                    : 1.0f;
            break;
        case NativePortCameraAspectPolicy::Explicit:
            result.camera_aspect =
                static_cast<float>(config_.explicit_camera_aspect.numerator) /
                static_cast<float>(config_.explicit_camera_aspect.denominator);
            break;
        }
        return result;
    }

    [[nodiscard]] NativePortTextureHandle create_texture(
        const NativePortTextureConfig& config,
        const std::span<const NativePortImageView> initial_mip_levels) {
        require_owner_thread();
        if (!valid_texture_format(config.format) || !config.shader_resource)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "texture-not-shader-resource");
        const auto byte_size = checked_texture_bytes(config);
        if (byte_size > config_.maximum_texture_bytes - texture_bytes_)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 "texture-byte-budget");
        validate_texture_images(config, initial_mip_levels);

        std::uint32_t index = 0u;
        if (!free_texture_slots_.empty()) {
            index = free_texture_slots_.back();
            free_texture_slots_.pop_back();
        } else {
            if (texture_slots_.size() >= config_.maximum_textures)
                fail(NativePortGraphicsFailure::ResourceLimit,
                     0u,
                     "texture-count-budget");
            index = static_cast<std::uint32_t>(texture_slots_.size());
            texture_slots_.emplace_back();
        }

        auto& slot = texture_slots_[index];
        try {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = config.extent.width;
            description.Height = config.extent.height;
            description.MipLevels = config.mip_levels;
            description.ArraySize = 1u;
            description.Format = texture_format(config.format);
            description.SampleDesc.Count = 1u;
            description.Usage = config.dynamic ? D3D11_USAGE_DYNAMIC
                                               : D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags =
                config.dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
            const auto result = device_->CreateTexture2D(
                &description, nullptr, slot.texture.GetAddressOf());
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     "texture-create");
            const auto view_result = device_->CreateShaderResourceView(
                slot.texture.Get(), nullptr, slot.view.GetAddressOf());
            if (FAILED(view_result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(view_result),
                     "texture-view");
            slot.config = config;
            slot.byte_size = byte_size;
            slot.live = true;
            if (slot.generation == 0u) slot.generation = 1u;
            for (std::uint32_t level = 0u;
                 level < initial_mip_levels.size(); ++level)
                upload_texture(slot, initial_mip_levels[level], level);
        } catch (...) {
            slot.texture.Reset();
            slot.view.Reset();
            slot.live = false;
            slot.byte_size = 0u;
            free_texture_slots_.push_back(index);
            throw;
        }
        texture_bytes_ += byte_size;
        ++live_textures_;
        return make_texture_handle(index, slot.generation);
    }

    void update_texture(const NativePortTextureHandle handle,
                        const NativePortImageView& pixels) {
        require_owner_thread();
        auto& slot = resolve_texture(handle);
        if (slot.config.mip_levels != 1u)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "texture-single-level-update");
        validate_image(pixels, slot.config.extent, slot.config.format);
        upload_texture(slot, pixels, 0u);
    }

    void update_texture(
        const NativePortTextureHandle handle,
        const std::span<const NativePortImageView> mip_levels) {
        require_owner_thread();
        auto& slot = resolve_texture(handle);
        validate_texture_images(slot.config, mip_levels);
        if (mip_levels.empty())
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "texture-empty-mip-update");
        for (std::uint32_t level = 0u; level < mip_levels.size(); ++level)
            upload_texture(slot, mip_levels[level], level);
    }

    void destroy_texture(const NativePortTextureHandle handle) {
        require_owner_thread();
        auto& slot = resolve_texture(handle);
        if (image_texture_ == handle) image_texture_ = {};
        const auto* const destroyed_view = slot.view.Get();
        if (bound_shader_resource_valid_ &&
            bound_shader_resource_ == destroyed_view) {
            ID3D11ShaderResourceView* no_view = nullptr;
            context_->PSSetShaderResources(0u, 1u, &no_view);
            bound_shader_resource_ = nullptr;
            bound_shader_resource_valid_ = false;
        }
        texture_bytes_ -= slot.byte_size;
        if (live_textures_ != 0u) --live_textures_;
        slot.texture.Reset();
        slot.view.Reset();
        slot.config = {};
        slot.byte_size = 0u;
        slot.live = false;
        ++slot.generation;
        if (slot.generation == 0u) ++slot.generation;
        const auto index = texture_handle_index(handle);
        free_texture_slots_.push_back(index);
    }

    [[nodiscard]] NativePortMeshHandle create_mesh(
        const NativePortMeshConfig& mesh) {
        require_owner_thread();
        if (!valid_shading(mesh.shading) ||
            !std::isfinite(mesh.small_triangle_area_threshold) ||
            mesh.small_triangle_area_threshold < 0.0f)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "mesh-preprocess-state");

        const auto remaining_mesh_bytes =
            config_.maximum_mesh_bytes - mesh_bytes_;
        const auto persistent_vertex_limit = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining_mesh_bytes,
                                    std::numeric_limits<UINT>::max()) /
            sizeof(NativePortVertex));
        const auto persistent_index_limit = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining_mesh_bytes,
                                    std::numeric_limits<UINT>::max()) /
            sizeof(std::uint32_t));

        static_cast<void>(validate_geometry(
            mesh.vertices,
            mesh.indices,
            mesh.topology,
            persistent_vertex_limit,
            persistent_index_limit,
            NativePortGraphicsFailure::InvalidResource,
            "mesh-layout",
            "mesh-index",
            "mesh-vertex",
            "mesh-topology"));

        auto vertices = mesh.vertices;
        auto indices = mesh.indices;
        auto topology = mesh.topology;
        std::vector<NativePortVertex> prepared;
        NativePortRasterizerState preprocessing;
        preprocessing.shading = mesh.shading;
        preprocessing.small_triangle_area_threshold =
            mesh.small_triangle_area_threshold;
        preprocessing.small_triangle_area_space =
            NativePortTriangleAreaSpace::Submitted;
        preprocess_geometry(vertices,
                            indices,
                            topology,
                            preprocessing,
                            NativePortMatrix4x4{},
                            persistent_vertex_limit,
                            prepared,
                            NativePortGraphicsFailure::InvalidResource,
                            "mesh-triangle-preprocess-topology",
                            "mesh-triangle-preprocess-budget");

        const auto capabilities = validate_geometry(
            vertices,
            indices,
            topology,
            persistent_vertex_limit,
            persistent_index_limit,
            NativePortGraphicsFailure::InvalidResource,
            "mesh-prepared-layout",
            "mesh-prepared-index",
            "mesh-prepared-vertex",
            "mesh-prepared-topology");

        const auto vertex_bytes =
            static_cast<std::uint64_t>(vertices.size_bytes());
        const auto index_bytes =
            static_cast<std::uint64_t>(indices.size_bytes());
        if (vertex_bytes > std::numeric_limits<std::uint64_t>::max() -
                               index_bytes)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 "mesh-byte-overflow");
        const auto byte_size = vertex_bytes + index_bytes;
        if (byte_size > config_.maximum_mesh_bytes - mesh_bytes_)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 "mesh-byte-budget");

        const bool reuse_slot = !free_mesh_slots_.empty();
        const auto index = reuse_slot
                               ? free_mesh_slots_.back()
                               : static_cast<std::uint32_t>(mesh_slots_.size());
        if (!reuse_slot && mesh_slots_.size() >= config_.maximum_meshes)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 "mesh-count-budget");
        const auto generation = reuse_slot ? mesh_slots_[index].generation : 1u;

        auto vertex_buffer = create_immutable_buffer(
            vertices.data(),
            vertices.size_bytes(),
            D3D11_BIND_VERTEX_BUFFER,
            "mesh-vertex-buffer");
        auto index_buffer = create_immutable_buffer(
            indices.data(),
            indices.size_bytes(),
            D3D11_BIND_INDEX_BUFFER,
            "mesh-index-buffer");

        if (reuse_slot)
            free_mesh_slots_.pop_back();
        else
            mesh_slots_.emplace_back();
        auto& slot = mesh_slots_[index];
        slot.vertex_buffer = std::move(vertex_buffer);
        slot.index_buffer = std::move(index_buffer);
        slot.source_topology = mesh.topology;
        slot.gpu_topology = topology;
        slot.source_shading = mesh.shading;
        slot.source_small_triangle_area_threshold =
            mesh.small_triangle_area_threshold;
        slot.capabilities = capabilities;
        slot.byte_size = byte_size;
        slot.vertex_count = static_cast<std::uint32_t>(vertices.size());
        slot.index_count = static_cast<std::uint32_t>(indices.size());
        slot.generation = generation;
        slot.live = true;
        mesh_bytes_ += byte_size;
        ++live_meshes_;
        saturating_add(snapshot_.uploaded_bytes, byte_size);
        saturating_add(snapshot_.persistent_geometry_uploaded_bytes,
                       byte_size);
        return make_mesh_handle(index, slot.generation);
    }

    void destroy_mesh(const NativePortMeshHandle handle) {
        require_owner_thread();
        auto& slot = resolve_mesh(handle);
        const auto index = mesh_handle_index(handle);
        const bool retire_generation =
            slot.generation == std::numeric_limits<std::uint32_t>::max();
        // Allocate free-list storage before invalidating a reusable resource
        // so an allocation failure leaves ownership unchanged. A slot whose
        // final generation was observed is retired permanently instead of
        // wrapping and making an ancient handle valid again.
        if (!retire_generation) free_mesh_slots_.push_back(index);
        mesh_bytes_ -= slot.byte_size;
        if (live_meshes_ != 0u) --live_meshes_;
        slot.vertex_buffer.Reset();
        slot.index_buffer.Reset();
        slot.source_topology = NativePortPrimitiveTopology::TriangleList;
        slot.gpu_topology = NativePortPrimitiveTopology::TriangleList;
        slot.source_shading = NativePortShadingMode::Smooth;
        slot.source_small_triangle_area_threshold = 0.0f;
        slot.capabilities = {};
        slot.byte_size = 0u;
        slot.vertex_count = 0u;
        slot.index_count = 0u;
        slot.live = false;
        if (!retire_generation) ++slot.generation;
    }

    void begin_frame(const NativePortFrameConfig& frame) {
        require_owner_thread();
        poll_events();
        if (close_requested_)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "window-closing");
        if (frame_open_)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "frame-already-open");
        if (!std::all_of(frame.clear_color.begin(),
                         frame.clear_color.end(),
                         [](const float value) { return std::isfinite(value); }) ||
            !std::isfinite(frame.clear_depth) || frame.clear_depth < 0.0f ||
            frame.clear_depth > 1.0f)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "frame-clear");
        if (!valid_depth_buffer_convention(frame.depth_buffer) ||
            (frame.depth_buffer == NativePortDepthBufferConvention::Forward
                 ? frame.clear_depth != 1.0f
                 : frame.clear_depth != 0.0f))
            fail(NativePortGraphicsFailure::InvalidFrame,
                 0u,
                 "frame-depth-contract");
        context_->OMSetRenderTargets(
            1u, render_target_.GetAddressOf(), depth_view_.Get());
        context_->ClearRenderTargetView(render_target_.Get(),
                                        frame.clear_color.data());
        context_->ClearDepthStencilView(
            depth_view_.Get(), D3D11_CLEAR_DEPTH, frame.clear_depth, 0u);
        vertex_buffer_write_offset_ = 0u;
        index_buffer_write_offset_ = 0u;
        vertex_buffer_discarded_ = false;
        index_buffer_discarded_ = false;
        invalidate_draw_state_shadow();
        frame_depth_buffer_ = frame.depth_buffer;
        frame_open_ = true;
        saturating_increment(snapshot_.begun_frames);
    }

    void draw(const NativePortDrawPacket& packet) {
        require_owner_thread();
        if (!frame_open_)
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-outside-frame");
        auto* const mesh_slot = packet.mesh != NativePortMeshHandle{}
                                    ? &resolve_mesh(packet.mesh)
                                    : nullptr;
        validate_draw(packet, mesh_slot);

        auto vertices = packet.vertices;
        auto indices = packet.indices;
        auto topology = packet.topology;
        if (mesh_slot != nullptr) {
            topology = mesh_slot->gpu_topology;
            require_geometry_capabilities(packet, mesh_slot->capabilities);
        } else {
            auto capabilities = validate_geometry(
                vertices,
                indices,
                topology,
                config_.maximum_transient_vertices,
                config_.maximum_transient_indices,
                NativePortGraphicsFailure::InvalidDraw,
                "draw-layout",
                "draw-index",
                "draw-vertex",
                "draw-topology");
            const bool preprocessing_required =
                packet.rasterizer.shading ==
                    NativePortShadingMode::FlatLastVertex ||
                packet.rasterizer.small_triangle_area_threshold > 0.0f;
            preprocess_geometry(vertices,
                                indices,
                                topology,
                                packet.rasterizer,
                                packet.transform,
                                config_.maximum_transient_vertices,
                                prepared_vertices_,
                                NativePortGraphicsFailure::InvalidDraw,
                                "draw-triangle-preprocess-topology",
                                "draw-triangle-preprocess-budget");
            if (vertices.empty()) {
                saturating_increment(snapshot_.draw_calls);
                return;
            }
            if (preprocessing_required) {
                capabilities = validate_geometry(
                    vertices,
                    indices,
                    topology,
                    config_.maximum_transient_vertices,
                    config_.maximum_transient_indices,
                    NativePortGraphicsFailure::InvalidDraw,
                    "draw-prepared-layout",
                    "draw-prepared-index",
                    "draw-prepared-vertex",
                    "draw-prepared-topology");
            }
            require_geometry_capabilities(packet, capabilities);
        }

        ID3D11Buffer* bound_vertex_buffer = nullptr;
        ID3D11Buffer* bound_index_buffer = nullptr;
        UINT vertex_buffer_offset = 0u;
        UINT index_buffer_offset = 0u;
        UINT vertex_count = 0u;
        UINT index_count = 0u;
        if (mesh_slot != nullptr) {
            bound_vertex_buffer = mesh_slot->vertex_buffer.Get();
            bound_index_buffer = mesh_slot->index_buffer.Get();
            vertex_count = mesh_slot->vertex_count;
            index_count = mesh_slot->index_count;
        } else {
            ensure_vertex_buffer(vertices.size_bytes());
            vertex_buffer_offset = upload_dynamic(
                vertex_buffer_.Get(), vertex_buffer_capacity_,
                vertex_buffer_write_offset_, vertex_buffer_discarded_,
                vertices.data(), vertices.size_bytes(), "vertex-buffer-map");
            bound_vertex_buffer = vertex_buffer_.Get();
            vertex_count = static_cast<UINT>(vertices.size());
            if (!indices.empty()) {
                ensure_index_buffer(indices.size_bytes());
                index_buffer_offset = upload_dynamic(
                    index_buffer_.Get(), index_buffer_capacity_,
                    index_buffer_write_offset_, index_buffer_discarded_,
                    indices.data(), indices.size_bytes(), "index-buffer-map");
                bound_index_buffer = index_buffer_.Get();
                index_count = static_cast<UINT>(indices.size());
            }
        }

        DrawConstants constants{};
        constants.transform = packet.transform.values;
        constants.normal_transform = packet.normal_transform.values;
        constants.material_diffuse = packet.material.diffuse;
        constants.material_ambient = packet.material.ambient;
        constants.material_specular = packet.material.specular;
        constants.material_emission = packet.material.emission;
        constants.scene_ambient = packet.lighting.ambient;
        constants.fog_color = packet.fog.color;
        if ((packet.fog.mode == NativePortFogMode::LookupTable ||
             packet.fog.mode == NativePortFogMode::LookupTablePrimary) &&
            (!fog_lookup_table_valid_ ||
             last_fog_lookup_table_ != packet.fog.lookup_table)) {
            FogTableConstants fog_constants{};
            std::memcpy(fog_constants.lookup_table.data(),
                        packet.fog.lookup_table.data(),
                        sizeof(packet.fog.lookup_table));
            context_->UpdateSubresource(fog_table_constants_.Get(),
                                        0u,
                                        nullptr,
                                        &fog_constants,
                                        0u,
                                        0u);
            last_fog_lookup_table_ = packet.fog.lookup_table;
            fog_lookup_table_valid_ = true;
        }
        constants.fog_parameters = {
            packet.fog.start, packet.fog.end, packet.fog.density, 0.0f};
        constants.depth_parameters = {
            packet.depth_mapping.reciprocal_scale,
            packet.depth_mapping.logarithm_divisor,
            0.0f,
            0.0f};
        constants.material_parameters = {
            packet.material.specular_power,
            packet.alpha_test.reference,
            0.0f,
            0.0f};
        std::uint32_t material_flags = 0u;
        if (packet.material.use_vertex_color)
            material_flags |= draw_flag_vertex_color;
        if (packet.material.use_secondary_color)
            material_flags |= draw_flag_secondary_color;
        if (packet.material.lighting_enabled)
            material_flags |= draw_flag_lighting;
        if (packet.material.specular_enabled)
            material_flags |= draw_flag_specular;
        if (packet.material.texture_coordinates ==
            NativePortTextureCoordinateSource::NormalSphere)
            material_flags |= draw_flag_normal_sphere;
        if (packet.depth_mapping.mode ==
                NativePortDepthCoordinateMode::ReciprocalPositive ||
            packet.depth_mapping.mode ==
                NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip)
            material_flags |= draw_flag_reciprocal_depth;
        if (packet.depth_mapping.mode ==
            NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip)
            material_flags |= draw_flag_homogeneous_reciprocal_clip;
        if (packet.material.use_primary_alpha)
            material_flags |= draw_flag_primary_alpha;
        if (packet.material.use_texture_alpha)
            material_flags |= draw_flag_texture_alpha;
        if (packet.texture)
            material_flags |= draw_flag_texture_present;
        material_flags |=
            static_cast<std::uint32_t>(packet.material.texture_combine)
            << draw_texture_combine_shift;
        constants.pipeline_flags = {
            material_flags,
            packet.lighting.light_count,
            static_cast<std::uint32_t>(packet.fog.mode),
            static_cast<std::uint32_t>(packet.alpha_test.compare) |
                (packet.alpha_test.enabled ? draw_alpha_test_enabled : 0u)};
        for (std::size_t index = 0u;
             index < packet.lighting.light_count; ++index) {
            const auto& light = packet.lighting.lights[index];
            constants.light_directions[index] = {
                light.direction[0], light.direction[1], light.direction[2],
                0.0f};
            constants.light_colors[index] = light.color;
        }
        if (!draw_constants_valid_ ||
            std::memcmp(&last_draw_constants_,
                        &constants,
                        sizeof(constants)) != 0) {
            context_->UpdateSubresource(
                draw_constants_.Get(), 0u, nullptr, &constants, 0u, 0u);
            last_draw_constants_ = constants;
            draw_constants_valid_ = true;
        }

        const auto current_layout = layout();
        const auto viewport_rect =
            packet.viewport == NativePortViewportTarget::Ui
                ? current_layout.ui_viewport
                : current_layout.game_viewport;
        set_viewport(viewport_rect);

        auto* const blend = resolve_blend_state(packet.blend);
        auto* const depth = resolve_depth_state(packet.depth);
        auto host_rasterizer = packet.rasterizer;
        // Flat-last-vertex and small-triangle semantics were resolved by the
        // bounded vertex preprocessing above. They do not create distinct
        // host rasterizer objects.
        host_rasterizer.shading = NativePortShadingMode::Smooth;
        host_rasterizer.small_triangle_area_threshold = 0.0f;
        host_rasterizer.small_triangle_area_space =
            NativePortTriangleAreaSpace::Submitted;
        host_rasterizer.small_triangle_reference_extent = {};
        auto* const rasterizer = resolve_rasterizer_state(host_rasterizer);
        auto* const sampler = resolve_sampler_state(packet.sampler);
        constexpr std::array blend_factor{0.0f, 0.0f, 0.0f, 0.0f};
        if (!bound_blend_valid_ || bound_blend_ != blend) {
            context_->OMSetBlendState(
                blend, blend_factor.data(), 0xFFFFFFFFu);
            bound_blend_ = blend;
            bound_blend_valid_ = true;
        }
        if (!bound_depth_valid_ || bound_depth_ != depth) {
            context_->OMSetDepthStencilState(depth, 0u);
            bound_depth_ = depth;
            bound_depth_valid_ = true;
        }
        if (!bound_rasterizer_valid_ || bound_rasterizer_ != rasterizer) {
            context_->RSSetState(rasterizer);
            bound_rasterizer_ = rasterizer;
            bound_rasterizer_valid_ = true;
        }
        if (!draw_pipeline_bound_) {
            context_->IASetInputLayout(input_layout_.Get());
            context_->VSSetShader(draw_vertex_shader_.Get(), nullptr, 0u);
            context_->VSSetConstantBuffers(
                0u, 1u, draw_constants_.GetAddressOf());
            context_->PSSetShader(draw_pixel_shader_.Get(), nullptr, 0u);
            const std::array<ID3D11Buffer*, 2u> pixel_constant_buffers{
                draw_constants_.Get(), fog_table_constants_.Get()};
            context_->PSSetConstantBuffers(
                0u,
                static_cast<UINT>(pixel_constant_buffers.size()),
                pixel_constant_buffers.data());
            draw_pipeline_bound_ = true;
        }
        const auto host_topology = primitive_topology(topology);
        if (!bound_topology_valid_ || bound_topology_ != host_topology) {
            context_->IASetPrimitiveTopology(host_topology);
            bound_topology_ = host_topology;
            bound_topology_valid_ = true;
        }
        const UINT stride = sizeof(NativePortVertex);
        if (!bound_vertex_buffer_valid_ ||
            bound_vertex_buffer_ != bound_vertex_buffer ||
            bound_vertex_buffer_offset_ != vertex_buffer_offset) {
            context_->IASetVertexBuffers(
                0u, 1u, &bound_vertex_buffer, &stride,
                &vertex_buffer_offset);
            bound_vertex_buffer_ = bound_vertex_buffer;
            bound_vertex_buffer_offset_ = vertex_buffer_offset;
            bound_vertex_buffer_valid_ = true;
        }
        if (index_count != 0u &&
            (!bound_index_buffer_valid_ ||
             bound_index_buffer_ != bound_index_buffer ||
             bound_index_buffer_offset_ != index_buffer_offset)) {
            context_->IASetIndexBuffer(
                bound_index_buffer, DXGI_FORMAT_R32_UINT,
                index_buffer_offset);
            bound_index_buffer_ = bound_index_buffer;
            bound_index_buffer_offset_ = index_buffer_offset;
            bound_index_buffer_valid_ = true;
        }
        if (!bound_sampler_valid_ || bound_sampler_ != sampler) {
            context_->PSSetSamplers(0u, 1u, &sampler);
            bound_sampler_ = sampler;
            bound_sampler_valid_ = true;
        }
        auto* view = packet.texture ? resolve_texture(packet.texture).view.Get()
                                    : white_view_.Get();
        if (!bound_shader_resource_valid_ ||
            bound_shader_resource_ != view) {
            context_->PSSetShaderResources(0u, 1u, &view);
            bound_shader_resource_ = view;
            bound_shader_resource_valid_ = true;
        }
        if (index_count == 0u)
            context_->Draw(vertex_count, 0u);
        else
            context_->DrawIndexed(index_count, 0u, 0);
        saturating_increment(snapshot_.draw_calls);
        if (mesh_slot != nullptr)
            saturating_increment(snapshot_.persistent_mesh_draw_calls);
    }

    void present() {
        require_owner_thread();
        if (!frame_open_)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "present-without-frame");
        frame_open_ = false;
        completed_frame_available_ = true;
        snapshot_.frame_open = false;
        present_completed_frame("present");
    }

    void repeat_present() {
        require_owner_thread();
        if (frame_open_ || !completed_frame_available_)
            fail(NativePortGraphicsFailure::InvalidFrame,
                 0u,
                 "repeat-without-completed-frame");
        present_completed_frame("repeat-present");
    }

  private:
    void present_completed_frame(const char* const operation) {
        poll_events();
        if (minimized_) {
            snapshot_.occluded = true;
            return;
        }
        context_->OMSetRenderTargets(
            1u, swap_chain_target_.GetAddressOf(), nullptr);
        constexpr std::array black{0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(swap_chain_target_.Get(), black.data());
        set_viewport(layout().output_viewport);
        constexpr std::array blend_factor{0.0f, 0.0f, 0.0f, 0.0f};
        NativePortBlendState composite_blend;
        NativePortDepthState composite_depth;
        composite_depth.test_enabled = false;
        composite_depth.write_enabled = false;
        NativePortRasterizerState composite_rasterizer;
        composite_rasterizer.cull = NativePortCullMode::None;
        NativePortSamplerState composite_sampler;
        context_->OMSetBlendState(
            resolve_blend_state(composite_blend), blend_factor.data(),
            0xFFFFFFFFu);
        context_->OMSetDepthStencilState(
            resolve_depth_state(composite_depth), 0u);
        context_->RSSetState(resolve_rasterizer_state(composite_rasterizer));
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(composite_vertex_shader_.Get(), nullptr, 0u);
        context_->PSSetShader(composite_pixel_shader_.Get(), nullptr, 0u);
        auto* const sampler = resolve_sampler_state(composite_sampler);
        context_->PSSetSamplers(0u, 1u, &sampler);
        context_->PSSetShaderResources(
            0u, 1u, render_view_.GetAddressOf());
        context_->Draw(3u, 0u);
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);
        invalidate_draw_state_shadow();
        const auto result = swap_chain_->Present(
            config_.synchronize_present ? 1u : 0u, 0u);
        if (result == DXGI_STATUS_OCCLUDED) {
            snapshot_.occluded = true;
            return;
        }
        if (FAILED(result)) {
            const auto removed = device_->GetDeviceRemovedReason();
            fail(NativePortGraphicsFailure::DeviceLost,
                 static_cast<std::uint32_t>(FAILED(removed) ? removed : result),
                 operation);
        }
        snapshot_.occluded = false;
        capture_completed_frame(snapshot_.presented_frames + 1u);
        saturating_increment(snapshot_.presented_frames);
    }

  public:

    void present_image(const NativePortImageView& image,
                       const NativePortViewportTarget viewport,
                       const NativePortImageFit fit) {
        require_owner_thread();
        if (!valid_viewport_target(viewport) || !valid_image_fit(fit))
            fail(NativePortGraphicsFailure::InvalidFrame,
                 0u,
                 "image-presentation-policy");
        validate_image(image, image.extent, image.format);
        if (!image_texture_ || image_texture_extent_.width != image.extent.width ||
            image_texture_extent_.height != image.extent.height ||
            image_texture_format_ != image.format) {
            if (image_texture_) destroy_texture(image_texture_);
            NativePortTextureConfig texture_config;
            texture_config.extent = image.extent;
            texture_config.format = image.format;
            texture_config.dynamic = true;
            image_texture_ = create_texture(
                texture_config, std::span<const NativePortImageView>{});
            image_texture_extent_ = image.extent;
            image_texture_format_ = image.format;
        }
        update_texture(image_texture_, image);
        begin_frame({});

        const auto current_layout = layout();
        const auto target = viewport == NativePortViewportTarget::Ui
                                ? current_layout.ui_viewport
                                : current_layout.game_viewport;
        auto destination = target;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
        const NativePortAspectRatio source_aspect =
            image.display_aspect_numerator != 0u
                ? NativePortAspectRatio{image.display_aspect_numerator,
                                        image.display_aspect_denominator}
                : NativePortAspectRatio{image.extent.width,
                                        image.extent.height};
        if (fit == NativePortImageFit::Contain) {
            destination = fit_aspect(target, source_aspect);
        } else if (fit == NativePortImageFit::Cover &&
                   target.width != 0u && target.height != 0u) {
            const auto target_ratio =
                static_cast<double>(target.width) / target.height;
            const auto source_ratio =
                static_cast<double>(source_aspect.numerator) /
                source_aspect.denominator;
            if (source_ratio > target_ratio) {
                const auto visible = static_cast<float>(target_ratio / source_ratio);
                u0 = (1.0f - visible) * 0.5f;
                u1 = 1.0f - u0;
            } else if (source_ratio < target_ratio) {
                const auto visible = static_cast<float>(source_ratio / target_ratio);
                v0 = (1.0f - visible) * 0.5f;
                v1 = 1.0f - v0;
            }
        }

        const auto relative_x = static_cast<float>(destination.x - target.x);
        const auto relative_y = static_cast<float>(destination.y - target.y);
        const auto left = -1.0f + 2.0f * relative_x / target.width;
        const auto right = -1.0f +
                           2.0f * (relative_x + destination.width) /
                               target.width;
        const auto top = 1.0f - 2.0f * relative_y / target.height;
        const auto bottom = 1.0f -
                            2.0f * (relative_y + destination.height) /
                                target.height;
        const std::array vertices{
            NativePortVertex{{left, top, 0.0f}, {u0, v0}},
            NativePortVertex{{right, top, 0.0f}, {u1, v0}},
            NativePortVertex{{right, bottom, 0.0f}, {u1, v1}},
            NativePortVertex{{left, bottom, 0.0f}, {u0, v1}},
        };
        constexpr std::array<std::uint32_t, 6u> indices{0u, 1u, 2u, 0u, 2u, 3u};
        NativePortDrawPacket packet;
        packet.vertices = vertices;
        packet.indices = indices;
        packet.texture = image_texture_;
        packet.viewport = viewport;
        packet.depth.test_enabled = false;
        packet.depth.write_enabled = false;
        packet.rasterizer.cull = NativePortCullMode::None;
        draw(packet);
        present();
    }

    [[nodiscard]] NativePortGraphicsSnapshot snapshot() const {
        require_owner_thread();
        auto result = snapshot_;
        result.live_textures = live_textures_;
        result.live_meshes = live_meshes_;
        result.persistent_mesh_bytes = mesh_bytes_;
        result.hardware_accelerated = device_ != nullptr;
        result.frame_open = frame_open_;
        return result;
    }

  private:
    struct DrawConstants final {
        std::array<float, 16u> transform{};
        std::array<float, 16u> normal_transform{};
        std::array<float, 4u> material_diffuse{};
        std::array<float, 4u> material_ambient{};
        std::array<float, 4u> material_specular{};
        std::array<float, 4u> material_emission{};
        std::array<float, 4u> scene_ambient{};
        std::array<float, 4u> fog_color{};
        std::array<std::array<float, 4u>,
                   native_port_maximum_directional_lights> light_directions{};
        std::array<std::array<float, 4u>,
                   native_port_maximum_directional_lights> light_colors{};
        std::array<float, 4u> fog_parameters{};
        std::array<float, 4u> depth_parameters{};
        std::array<float, 4u> material_parameters{};
        std::array<std::uint32_t, 4u> pipeline_flags{};
    };

    static_assert(sizeof(DrawConstants) % 16u == 0u);

    struct FogTableConstants final {
        std::array<std::array<float, 4u>,
                   native_port_fog_table_entries / 4u> lookup_table{};
    };

    static_assert(sizeof(FogTableConstants) % 16u == 0u);

    struct BlendStateSlot final {
        NativePortBlendState key;
        ComPtr<ID3D11BlendState> value;
    };

    struct DepthStateSlot final {
        NativePortDepthState key;
        ComPtr<ID3D11DepthStencilState> value;
    };

    struct RasterizerStateSlot final {
        NativePortRasterizerState key;
        ComPtr<ID3D11RasterizerState> value;
    };

    struct SamplerStateSlot final {
        NativePortSamplerState key;
        ComPtr<ID3D11SamplerState> value;
    };

    struct TextureSlot final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        NativePortTextureConfig config;
        std::uint64_t byte_size = 0u;
        std::uint32_t generation = 1u;
        bool live = false;
    };

    static LRESULT CALLBACK window_proc(HWND window,
                                        const UINT message,
                                        const WPARAM word,
                                        const LPARAM data) noexcept {
        auto* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(data);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) return DefWindowProcW(window, message, word, data);
        switch (message) {
        case WM_CLOSE:
            self->close_requested_ = true;
            return 0;
        case WM_SIZE:
            self->minimized_ = word == SIZE_MINIMIZED;
            if (!self->minimized_) {
                self->pending_output_extent_ = {
                    static_cast<std::uint32_t>(LOWORD(data)),
                    static_cast<std::uint32_t>(HIWORD(data))};
            }
            return 0;
        case WM_DESTROY:
            self->close_requested_ = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, word, data);
        }
    }

    void create_window() {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance, native_graphics_window_class, &existing) ==
            FALSE) {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &Impl::window_proc;
            window_class.hInstance = instance;
            window_class.hCursor =
                LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.lpszClassName = native_graphics_window_class;
            if (RegisterClassExW(&window_class) == 0u &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                fail(NativePortGraphicsFailure::WindowCreation,
                     GetLastError(),
                     "window-class");
        }
        RECT bounds{0,
                    0,
                    static_cast<LONG>(config_.output_extent.width),
                    static_cast<LONG>(config_.output_extent.height)};
        const auto style = config_.resizable
                               ? WS_OVERLAPPEDWINDOW
                               : WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                     WS_MINIMIZEBOX;
        if (AdjustWindowRect(&bounds, style, FALSE) == FALSE)
            fail(NativePortGraphicsFailure::WindowCreation,
                 GetLastError(),
                 "window-rect");
        const auto title = utf8_to_wide(config_.title);
        window_ = CreateWindowExW(0,
                                  native_graphics_window_class,
                                  title.c_str(),
                                  style,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  bounds.right - bounds.left,
                                  bounds.bottom - bounds.top,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  this);
        if (window_ == nullptr)
            fail(NativePortGraphicsFailure::WindowCreation,
                 GetLastError(),
                 "window-create");
        output_extent_ = config_.output_extent;
        pending_output_extent_ = output_extent_;
    }

    void destroy_window() noexcept {
        if (window_ == nullptr) return;
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
        window_ = nullptr;
    }

    void create_device() {
        constexpr std::array requested_levels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        const auto attempt = [&](DXGI_SWAP_EFFECT effect,
                                 const UINT buffer_count,
                                 const bool include_11_1) {
            swap_chain_.Reset();
            device_.Reset();
            context_.Reset();
            DXGI_SWAP_CHAIN_DESC description{};
            description.BufferDesc.Width = output_extent_.width;
            description.BufferDesc.Height = output_extent_.height;
            description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1u;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = buffer_count;
            description.OutputWindow = window_;
            description.Windowed = TRUE;
            description.SwapEffect = effect;
            D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
            const auto first = include_11_1 ? 0u : 1u;
            return D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_SINGLETHREADED |
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                requested_levels.data() + first,
                static_cast<UINT>(requested_levels.size() - first),
                D3D11_SDK_VERSION,
                &description,
                swap_chain_.GetAddressOf(),
                device_.GetAddressOf(),
                &selected,
                context_.GetAddressOf());
        };
        auto result = attempt(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2u, true);
        if (result == E_INVALIDARG)
            result = attempt(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2u, false);
        if (FAILED(result)) {
            swap_chain_.Reset();
            device_.Reset();
            context_.Reset();
            result = attempt(DXGI_SWAP_EFFECT_DISCARD, 1u, true);
            if (result == E_INVALIDARG)
                result = attempt(DXGI_SWAP_EFFECT_DISCARD, 1u, false);
        }
        if (FAILED(result))
            fail(NativePortGraphicsFailure::HardwareDeviceUnavailable,
                 static_cast<std::uint32_t>(result),
                 "d3d11-device");
        ComPtr<IDXGIDevice1> dxgi_device;
        if (SUCCEEDED(device_.As(&dxgi_device)))
            static_cast<void>(dxgi_device->SetMaximumFrameLatency(1u));
        create_swap_chain_target();
    }

    void create_swap_chain_target() {
        ComPtr<ID3D11Texture2D> back_buffer;
        const auto result = swap_chain_->GetBuffer(
            0u,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(back_buffer.GetAddressOf()));
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "swap-chain-buffer");
        const auto view_result = device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, swap_chain_target_.GetAddressOf());
        if (FAILED(view_result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(view_result),
                 "swap-chain-target");
    }

    void create_pipeline() {
        const auto draw_vertex_bytecode =
            compile_shader("draw_vertex_main", "vs_4_0");
        const auto draw_pixel_bytecode =
            compile_shader("draw_pixel_main", "ps_4_0");
        const auto composite_vertex_bytecode =
            compile_shader("composite_vertex_main", "vs_4_0");
        const auto composite_pixel_bytecode =
            compile_shader("composite_pixel_main", "ps_4_0");
        auto check = [&](const HRESULT result, const char* operation) {
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     operation);
        };
        check(device_->CreateVertexShader(
                  draw_vertex_bytecode->GetBufferPointer(),
                  draw_vertex_bytecode->GetBufferSize(),
                  nullptr,
                  draw_vertex_shader_.GetAddressOf()),
              "draw-vertex-shader");
        check(device_->CreatePixelShader(
                  draw_pixel_bytecode->GetBufferPointer(),
                  draw_pixel_bytecode->GetBufferSize(),
                  nullptr,
                  draw_pixel_shader_.GetAddressOf()),
              "draw-pixel-shader");
        check(device_->CreateVertexShader(
                  composite_vertex_bytecode->GetBufferPointer(),
                  composite_vertex_bytecode->GetBufferSize(),
                  nullptr,
                  composite_vertex_shader_.GetAddressOf()),
              "composite-vertex-shader");
        check(device_->CreatePixelShader(
                  composite_pixel_bytecode->GetBufferPointer(),
                  composite_pixel_bytecode->GetBufferSize(),
                  nullptr,
                  composite_pixel_shader_.GetAddressOf()),
              "composite-pixel-shader");

        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 7u> elements{
            D3D11_INPUT_ELEMENT_DESC{
                "POSITION", 0u, DXGI_FORMAT_R32G32B32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, position)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 0u, DXGI_FORMAT_R32G32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, texture_coordinate)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "COLOR", 0u, DXGI_FORMAT_R32G32B32A32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, color)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "NORMAL", 0u, DXGI_FORMAT_R32G32B32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, normal)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "COLOR", 1u, DXGI_FORMAT_R32G32B32A32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, secondary_color)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 1u, DXGI_FORMAT_R32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, fog_coordinate)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 2u, DXGI_FORMAT_R32_FLOAT, 0u,
                static_cast<UINT>(offsetof(NativePortVertex, depth_coordinate)),
                D3D11_INPUT_PER_VERTEX_DATA, 0u},
        };
        check(device_->CreateInputLayout(
                  elements.data(),
                  static_cast<UINT>(elements.size()),
                  draw_vertex_bytecode->GetBufferPointer(),
                  draw_vertex_bytecode->GetBufferSize(),
                  input_layout_.GetAddressOf()),
              "draw-input-layout");

        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = sizeof(DrawConstants);
        constant_description.Usage = D3D11_USAGE_DEFAULT;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(
                  &constant_description, nullptr, draw_constants_.GetAddressOf()),
              "draw-constants");
        FogTableConstants initial_fog_constants{};
        D3D11_SUBRESOURCE_DATA initial_fog_data{};
        initial_fog_data.pSysMem = &initial_fog_constants;
        constant_description.ByteWidth = sizeof(FogTableConstants);
        check(device_->CreateBuffer(&constant_description,
                                    &initial_fog_data,
                                    fog_table_constants_.GetAddressOf()),
              "fog-table-constants");
    }

    [[nodiscard]] std::size_t pipeline_state_count() const noexcept {
        return blend_states_.size() + depth_states_.size() +
               rasterizer_states_.size() + sampler_states_.size();
    }

    void require_pipeline_state_budget(const char* const operation) {
        if (pipeline_state_count() >= config_.maximum_pipeline_states)
            fail(NativePortGraphicsFailure::ResourceLimit, 0u, operation);
    }

    [[nodiscard]] ID3D11BlendState* resolve_blend_state(
        const NativePortBlendState& key) {
        if (last_blend_key_valid_ && last_blend_key_ == key)
            return last_blend_state_;
        const auto existing = std::find_if(
            blend_states_.begin(), blend_states_.end(),
            [&](const BlendStateSlot& slot) { return slot.key == key; });
        if (existing != blend_states_.end()) {
            last_blend_key_ = key;
            last_blend_key_valid_ = true;
            last_blend_state_ = existing->value.Get();
            return last_blend_state_;
        }
        require_pipeline_state_budget("blend-state-budget");
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = key.enabled ? TRUE : FALSE;
        target.SrcBlend = blend_factor(key.source_color);
        target.DestBlend = blend_factor(key.destination_color);
        target.BlendOp = blend_operation(key.color_operation);
        target.SrcBlendAlpha = blend_factor(key.source_alpha);
        target.DestBlendAlpha = blend_factor(key.destination_alpha);
        target.BlendOpAlpha = blend_operation(key.alpha_operation);
        target.RenderTargetWriteMask =
            ((key.color_write_mask & 0x01u) != 0u
                 ? D3D11_COLOR_WRITE_ENABLE_RED
                 : 0u) |
            ((key.color_write_mask & 0x02u) != 0u
                 ? D3D11_COLOR_WRITE_ENABLE_GREEN
                 : 0u) |
            ((key.color_write_mask & 0x04u) != 0u
                 ? D3D11_COLOR_WRITE_ENABLE_BLUE
                 : 0u) |
            ((key.color_write_mask & 0x08u) != 0u
                 ? D3D11_COLOR_WRITE_ENABLE_ALPHA
                 : 0u);
        ComPtr<ID3D11BlendState> state;
        const auto result =
            device_->CreateBlendState(&description, state.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result), "blend-state");
        blend_states_.push_back({key, std::move(state)});
        last_blend_key_ = key;
        last_blend_key_valid_ = true;
        last_blend_state_ = blend_states_.back().value.Get();
        return last_blend_state_;
    }

    [[nodiscard]] ID3D11DepthStencilState* resolve_depth_state(
        const NativePortDepthState& key) {
        if (last_depth_key_valid_ && last_depth_key_ == key)
            return last_depth_state_;
        const auto existing = std::find_if(
            depth_states_.begin(), depth_states_.end(),
            [&](const DepthStateSlot& slot) { return slot.key == key; });
        if (existing != depth_states_.end()) {
            last_depth_key_ = key;
            last_depth_key_valid_ = true;
            last_depth_state_ = existing->value.Get();
            return last_depth_state_;
        }
        require_pipeline_state_budget("depth-state-budget");
        D3D11_DEPTH_STENCIL_DESC description{};
        description.DepthEnable = key.test_enabled ? TRUE : FALSE;
        description.DepthWriteMask = key.write_enabled
                                         ? D3D11_DEPTH_WRITE_MASK_ALL
                                         : D3D11_DEPTH_WRITE_MASK_ZERO;
        description.DepthFunc = comparison_function(key.compare);
        ComPtr<ID3D11DepthStencilState> state;
        const auto result = device_->CreateDepthStencilState(
            &description, state.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result), "depth-state");
        depth_states_.push_back({key, std::move(state)});
        last_depth_key_ = key;
        last_depth_key_valid_ = true;
        last_depth_state_ = depth_states_.back().value.Get();
        return last_depth_state_;
    }

    [[nodiscard]] ID3D11RasterizerState* resolve_rasterizer_state(
        const NativePortRasterizerState& key) {
        if (last_rasterizer_key_valid_ && last_rasterizer_key_ == key)
            return last_rasterizer_state_;
        const auto existing = std::find_if(
            rasterizer_states_.begin(), rasterizer_states_.end(),
            [&](const RasterizerStateSlot& slot) { return slot.key == key; });
        if (existing != rasterizer_states_.end()) {
            last_rasterizer_key_ = key;
            last_rasterizer_key_valid_ = true;
            last_rasterizer_state_ = existing->value.Get();
            return last_rasterizer_state_;
        }
        require_pipeline_state_budget("rasterizer-state-budget");
        D3D11_RASTERIZER_DESC description{};
        description.FillMode = key.fill == NativePortFillMode::Wireframe
                                   ? D3D11_FILL_WIREFRAME
                                   : D3D11_FILL_SOLID;
        description.CullMode = key.cull == NativePortCullMode::Front
                                   ? D3D11_CULL_FRONT
                                   : key.cull == NativePortCullMode::Back
                                         ? D3D11_CULL_BACK
                                         : D3D11_CULL_NONE;
        description.FrontCounterClockwise =
            key.front_counter_clockwise ? TRUE : FALSE;
        description.DepthClipEnable = key.depth_clip_enabled ? TRUE : FALSE;
        description.ScissorEnable = TRUE;
        ComPtr<ID3D11RasterizerState> state;
        const auto result = device_->CreateRasterizerState(
            &description, state.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result), "rasterizer-state");
        rasterizer_states_.push_back({key, std::move(state)});
        last_rasterizer_key_ = key;
        last_rasterizer_key_valid_ = true;
        last_rasterizer_state_ = rasterizer_states_.back().value.Get();
        return last_rasterizer_state_;
    }

    [[nodiscard]] ID3D11SamplerState* resolve_sampler_state(
        const NativePortSamplerState& key) {
        if (last_sampler_key_valid_ && last_sampler_key_ == key)
            return last_sampler_state_;
        const auto existing = std::find_if(
            sampler_states_.begin(), sampler_states_.end(),
            [&](const SamplerStateSlot& slot) { return slot.key == key; });
        if (existing != sampler_states_.end()) {
            last_sampler_key_ = key;
            last_sampler_key_valid_ = true;
            last_sampler_state_ = existing->value.Get();
            return last_sampler_state_;
        }
        require_pipeline_state_budget("sampler-state-budget");
        D3D11_SAMPLER_DESC description{};
        description.Filter = texture_filter(key.filter);
        description.AddressU = texture_address_mode(key.address_u);
        description.AddressV = texture_address_mode(key.address_v);
        description.AddressW = texture_address_mode(key.address_w);
        description.MipLODBias = key.mip_lod_bias;
        description.MaxAnisotropy = key.maximum_anisotropy;
        description.ComparisonFunc = D3D11_COMPARISON_NEVER;
        description.MinLOD = key.minimum_lod;
        description.MaxLOD = key.maximum_lod;
        ComPtr<ID3D11SamplerState> state;
        const auto result = device_->CreateSamplerState(
            &description, state.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result), "sampler-state");
        sampler_states_.push_back({key, std::move(state)});
        last_sampler_key_ = key;
        last_sampler_key_valid_ = true;
        last_sampler_state_ = sampler_states_.back().value.Get();
        return last_sampler_state_;
    }

    void create_render_surface() {
        D3D11_TEXTURE2D_DESC color_description{};
        color_description.Width = config_.render_extent.width;
        color_description.Height = config_.render_extent.height;
        color_description.MipLevels = 1u;
        color_description.ArraySize = 1u;
        color_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        color_description.SampleDesc.Count = 1u;
        color_description.Usage = D3D11_USAGE_DEFAULT;
        color_description.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto result = device_->CreateTexture2D(
            &color_description, nullptr, render_texture_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "render-texture");
        result = device_->CreateRenderTargetView(
            render_texture_.Get(), nullptr, render_target_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "render-target");
        result = device_->CreateShaderResourceView(
            render_texture_.Get(), nullptr, render_view_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "render-view");

        D3D11_TEXTURE2D_DESC depth_description{};
        depth_description.Width = config_.render_extent.width;
        depth_description.Height = config_.render_extent.height;
        depth_description.MipLevels = 1u;
        depth_description.ArraySize = 1u;
        depth_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_description.SampleDesc.Count = 1u;
        depth_description.Usage = D3D11_USAGE_DEFAULT;
        depth_description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        result = device_->CreateTexture2D(
            &depth_description, nullptr, depth_texture_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "depth-texture");
        result = device_->CreateDepthStencilView(
            depth_texture_.Get(), nullptr, depth_view_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "depth-view");
    }

    void create_white_texture() {
        constexpr std::uint32_t white = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 1u;
        description.Height = 1u;
        description.MipLevels = 1u;
        description.ArraySize = 1u;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1u;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = &white;
        data.SysMemPitch = sizeof(white);
        auto result = device_->CreateTexture2D(
            &description, &data, white_texture_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "white-texture");
        result = device_->CreateShaderResourceView(
            white_texture_.Get(), nullptr, white_view_.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "white-view");
    }

    void apply_pending_resize() {
        if (pending_output_extent_.width == 0u ||
            pending_output_extent_.height == 0u ||
            (pending_output_extent_.width == output_extent_.width &&
             pending_output_extent_.height == output_extent_.height))
            return;
        if (!valid_extent(pending_output_extent_))
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 "swap-chain-resize-extent");
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);
        context_->OMSetRenderTargets(0u, nullptr, nullptr);
        invalidate_draw_state_shadow();
        swap_chain_target_.Reset();
        const auto result = swap_chain_->ResizeBuffers(
            0u,
            pending_output_extent_.width,
            pending_output_extent_.height,
            DXGI_FORMAT_UNKNOWN,
            0u);
        if (FAILED(result))
            fail(NativePortGraphicsFailure::DeviceLost,
                 static_cast<std::uint32_t>(result),
                 "swap-chain-resize");
        output_extent_ = pending_output_extent_;
        create_swap_chain_target();
        // poll_events() is a public lifecycle boundary and may observe a
        // resize between begin_frame() and draw(). ResizeBuffers requires all
        // targets to be unbound, so restore the unchanged native render
        // surface when that frame is still open.
        if (frame_open_)
            context_->OMSetRenderTargets(
                1u, render_target_.GetAddressOf(), depth_view_.Get());
        saturating_increment(snapshot_.swap_chain_resizes);
    }

    [[nodiscard]] GeometryCapabilities validate_geometry(
        const std::span<const NativePortVertex> vertices,
        const std::span<const std::uint32_t> indices,
        const NativePortPrimitiveTopology topology,
        const std::size_t maximum_vertices,
        const std::size_t maximum_indices,
        const NativePortGraphicsFailure failure,
        const char* const layout_operation,
        const char* const index_operation,
        const char* const vertex_operation,
        const char* const topology_operation) {
        if (!valid_topology(topology) || vertices.empty() ||
            vertices.size() > maximum_vertices ||
            indices.size() > maximum_indices ||
            vertices.size_bytes() > std::numeric_limits<UINT>::max() ||
            indices.size_bytes() > std::numeric_limits<UINT>::max())
            fail(failure, 0u, layout_operation);
        for (const auto index : indices) {
            if (index >= vertices.size()) fail(failure, 0u, index_operation);
        }

        GeometryCapabilities capabilities;
        for (const auto& vertex : vertices) {
            if (!finite_array(vertex.position) ||
                !finite_array(vertex.texture_coordinate) ||
                !finite_array(vertex.color) || !finite_array(vertex.normal) ||
                !finite_array(vertex.secondary_color) ||
                !std::isfinite(vertex.fog_coordinate) ||
                !std::isfinite(vertex.depth_coordinate))
                fail(failure, 0u, vertex_operation);
            capabilities.positive_depth_coordinates =
                capabilities.positive_depth_coordinates &&
                vertex.depth_coordinate > 0.0f;
            capabilities.nonnegative_fog_coordinates =
                capabilities.nonnegative_fog_coordinates &&
                vertex.fog_coordinate >= 0.0f;
            const auto length_squared =
                vertex.normal[0] * vertex.normal[0] +
                vertex.normal[1] * vertex.normal[1] +
                vertex.normal[2] * vertex.normal[2];
            capabilities.nonzero_normals =
                capabilities.nonzero_normals &&
                std::isfinite(length_squared) && length_squared > 0.0f;
        }

        const auto element_count =
            indices.empty() ? vertices.size() : indices.size();
        const bool valid_count = [&] {
            switch (topology) {
            case NativePortPrimitiveTopology::PointList:
                return element_count >= 1u;
            case NativePortPrimitiveTopology::LineList:
                return element_count >= 2u && element_count % 2u == 0u;
            case NativePortPrimitiveTopology::LineStrip:
                return element_count >= 2u;
            case NativePortPrimitiveTopology::TriangleList:
                return element_count >= 3u && element_count % 3u == 0u;
            case NativePortPrimitiveTopology::TriangleStrip:
                return element_count >= 3u;
            }
            return false;
        }();
        if (!valid_count) fail(failure, 0u, topology_operation);
        return capabilities;
    }

    void require_geometry_capabilities(
        const NativePortDrawPacket& packet,
        const GeometryCapabilities& capabilities) {
        if ((packet.fog.mode != NativePortFogMode::Disabled &&
             packet.depth_mapping.mode !=
                 NativePortDepthCoordinateMode::
                     ReciprocalPositiveHomogeneousClip &&
             !capabilities.nonnegative_fog_coordinates) ||
            (packet.depth_mapping.mode ==
                 NativePortDepthCoordinateMode::ReciprocalPositive &&
             (packet.depth.test_enabled || packet.texture) &&
             !capabilities.positive_depth_coordinates))
            fail(NativePortGraphicsFailure::InvalidDraw,
                 0u,
                 "draw-vertex");
        if ((packet.material.lighting_enabled ||
             packet.material.texture_coordinates ==
                 NativePortTextureCoordinateSource::NormalSphere) &&
            !capabilities.nonzero_normals)
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-normal");
    }

    void validate_draw(const NativePortDrawPacket& packet,
                       const MeshSlot* const mesh_slot) {
        if (!valid_viewport_target(packet.viewport) ||
            !valid_topology(packet.topology) ||
            !valid_blend(packet.blend) || !valid_depth(packet.depth) ||
            !valid_depth_mapping(packet.depth_mapping) ||
            !valid_rasterizer(packet.rasterizer) ||
            !valid_sampler(packet.sampler) ||
            !valid_material(packet.material) ||
            !valid_lighting(packet.lighting) || !valid_fog(packet.fog) ||
            !valid_alpha_test(packet.alpha_test) ||
            !finite_array(packet.transform.values) ||
            !finite_array(packet.normal_transform.values))
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-layout");
        if (!compatible_depth_contract(
                frame_depth_buffer_, packet.depth, packet.depth_mapping))
            fail(NativePortGraphicsFailure::InvalidDraw,
                 0u,
                 "draw-depth-contract");

        if (mesh_slot != nullptr) {
            if (!packet.vertices.empty() || !packet.indices.empty() ||
                packet.topology != mesh_slot->source_topology ||
                packet.rasterizer.shading != mesh_slot->source_shading ||
                packet.rasterizer.small_triangle_area_threshold !=
                    mesh_slot->source_small_triangle_area_threshold ||
                packet.rasterizer.small_triangle_area_space !=
                    NativePortTriangleAreaSpace::Submitted ||
                packet.rasterizer.small_triangle_reference_extent !=
                    NativePortExtent{})
                fail(NativePortGraphicsFailure::InvalidDraw,
                     0u,
                     "draw-mesh-contract");
        }
        if (packet.texture) static_cast<void>(resolve_texture(packet.texture));
    }

    void preprocess_geometry(
        std::span<const NativePortVertex>& vertices,
        std::span<const std::uint32_t>& indices,
        NativePortPrimitiveTopology& topology,
        const NativePortRasterizerState& rasterizer,
        const NativePortMatrix4x4& transform,
        const std::size_t maximum_output_vertices,
        std::vector<NativePortVertex>& destination,
        const NativePortGraphicsFailure topology_failure,
        const char* const topology_operation,
        const char* const budget_operation) {
        const bool preprocess_triangles =
            rasterizer.shading == NativePortShadingMode::FlatLastVertex ||
            rasterizer.small_triangle_area_threshold > 0.0f;
        if (!preprocess_triangles) return;
        if (topology != NativePortPrimitiveTopology::TriangleList &&
            topology != NativePortPrimitiveTopology::TriangleStrip)
            fail(topology_failure, 0u, topology_operation);
        const auto element_count =
            indices.empty() ? vertices.size() : indices.size();
        const auto triangle_count =
            topology == NativePortPrimitiveTopology::TriangleList
                ? element_count / 3u
                : element_count - 2u;
        if (triangle_count > maximum_output_vertices / 3u)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 0u,
                 budget_operation);
        destination.clear();
        destination.reserve(triangle_count * 3u);
        const auto source_index = [&](const std::size_t element) {
            return indices.empty() ? static_cast<std::uint32_t>(element)
                                   : indices[element];
        };
        const auto triangle_area_position =
            [&](const NativePortVertex& vertex,
                std::array<double, 2u>& result) {
                if (rasterizer.small_triangle_area_space ==
                    NativePortTriangleAreaSpace::Submitted) {
                    result = {vertex.position[0], vertex.position[1]};
                    return true;
                }

                std::array<double, 4u> clip{};
                const std::array<double, 4u> source{
                    vertex.position[0], vertex.position[1],
                    vertex.position[2], 1.0};
                for (std::size_t column = 0u; column < 4u; ++column) {
                    for (std::size_t row = 0u; row < 4u; ++row) {
                        clip[column] +=
                            source[row] * transform.values[row * 4u + column];
                    }
                }
                if (!std::ranges::all_of(
                        clip,
                        [](const double value) {
                            return std::isfinite(value);
                        }) ||
                    clip[3u] <= 0.0)
                    return false;

                const auto ndc_x = clip[0u] / clip[3u];
                const auto ndc_y = clip[1u] / clip[3u];
                result = {
                    (ndc_x + 1.0) * 0.5 *
                        rasterizer.small_triangle_reference_extent.width,
                    (1.0 - ndc_y) * 0.5 *
                        rasterizer.small_triangle_reference_extent.height};
                return std::ranges::all_of(
                    result,
                    [](const double value) { return std::isfinite(value); });
            };
        const auto append_triangle =
            [&](const std::size_t first,
                const std::size_t second,
                const std::size_t third,
                const std::size_t provoking) {
                NativePortVertex a = vertices[source_index(first)];
                NativePortVertex b = vertices[source_index(second)];
                NativePortVertex c = vertices[source_index(third)];
                std::array<double, 2u> area_a{};
                std::array<double, 2u> area_b{};
                std::array<double, 2u> area_c{};
                if (rasterizer.small_triangle_area_threshold > 0.0f &&
                    triangle_area_position(a, area_a) &&
                    triangle_area_position(b, area_b) &&
                    triangle_area_position(c, area_c)) {
                    const auto determinant =
                        (area_b[0u] - area_a[0u]) *
                            (area_c[1u] - area_a[1u]) -
                        (area_b[1u] - area_a[1u]) *
                            (area_c[0u] - area_a[0u]);
                    if (std::abs(determinant) <
                        rasterizer.small_triangle_area_threshold)
                        return;
                }
                if (rasterizer.shading ==
                    NativePortShadingMode::FlatLastVertex) {
                    const auto& flat = vertices[source_index(provoking)];
                    a.color = b.color = c.color = flat.color;
                    a.secondary_color = b.secondary_color =
                        c.secondary_color = flat.secondary_color;
                    a.normal = b.normal = c.normal = flat.normal;
                }
                destination.push_back(a);
                destination.push_back(b);
                destination.push_back(c);
            };
        if (topology == NativePortPrimitiveTopology::TriangleList) {
            for (std::size_t element = 0u; element < element_count;
                 element += 3u)
                append_triangle(
                    element, element + 1u, element + 2u, element + 2u);
        } else {
            for (std::size_t element = 0u; element + 2u < element_count;
                 ++element) {
                if ((element & 1u) == 0u)
                    append_triangle(element,
                                    element + 1u,
                                    element + 2u,
                                    element + 2u);
                else
                    append_triangle(element + 1u,
                                    element,
                                    element + 2u,
                                    element + 2u);
            }
        }
        vertices = destination;
        indices = {};
        topology = NativePortPrimitiveTopology::TriangleList;
    }

    [[nodiscard]] ComPtr<ID3D11Buffer> create_immutable_buffer(
        const void* const source,
        const std::size_t byte_size,
        const UINT bind_flags,
        const char* const operation) {
        if (byte_size == 0u) return {};
        if (source == nullptr || byte_size > std::numeric_limits<UINT>::max())
            fail(NativePortGraphicsFailure::InvalidResource, 0u, operation);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byte_size);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = bind_flags;
        D3D11_SUBRESOURCE_DATA initial_data{};
        initial_data.pSysMem = source;
        ComPtr<ID3D11Buffer> buffer;
        const auto result = device_->CreateBuffer(
            &description, &initial_data, buffer.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 operation);
        return buffer;
    }

    void ensure_vertex_buffer(const std::size_t byte_size) {
        if (byte_size <= vertex_buffer_capacity_) return;
        const auto required = static_cast<UINT>(byte_size);
        const auto maximum = static_cast<UINT>(
            config_.maximum_transient_vertices * sizeof(NativePortVertex));
        const auto preferred = std::min(maximum,
                                        minimum_dynamic_vertex_buffer_bytes);
        const auto capacity = next_buffer_capacity(std::max(required, preferred));
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = capacity;
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> replacement;
        const auto result = device_->CreateBuffer(
            &description, nullptr, replacement.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "vertex-buffer");
        vertex_buffer_ = std::move(replacement);
        vertex_buffer_capacity_ = capacity;
        vertex_buffer_write_offset_ = 0u;
        vertex_buffer_discarded_ = false;
    }

    void ensure_index_buffer(const std::size_t byte_size) {
        if (byte_size <= index_buffer_capacity_) return;
        const auto required = static_cast<UINT>(byte_size);
        const auto maximum = static_cast<UINT>(
            config_.maximum_transient_indices * sizeof(std::uint32_t));
        const auto preferred = std::min(maximum,
                                        minimum_dynamic_index_buffer_bytes);
        const auto capacity = next_buffer_capacity(std::max(required, preferred));
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = capacity;
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_INDEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> replacement;
        const auto result = device_->CreateBuffer(
            &description, nullptr, replacement.GetAddressOf());
        if (FAILED(result))
            fail(NativePortGraphicsFailure::ResourceCreation,
                 static_cast<std::uint32_t>(result),
                 "index-buffer");
        index_buffer_ = std::move(replacement);
        index_buffer_capacity_ = capacity;
        index_buffer_write_offset_ = 0u;
        index_buffer_discarded_ = false;
    }

    [[nodiscard]] UINT upload_dynamic(ID3D11Buffer* const buffer,
                                      const UINT capacity,
                                      UINT& write_offset,
                                      bool& discarded,
                                      const void* const source,
                                      const std::size_t byte_size,
                                      const char* const operation) {
        const auto required = static_cast<UINT>(byte_size);
        if (required > capacity || write_offset > capacity)
            fail(NativePortGraphicsFailure::ResourceLimit, 0u, operation);

        D3D11_MAP map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
        UINT upload_offset = write_offset;
        if (!discarded || required > capacity - write_offset) {
            map_type = D3D11_MAP_WRITE_DISCARD;
            upload_offset = 0u;
            write_offset = 0u;
            discarded = true;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result = context_->Map(
            buffer, 0u, map_type, 0u, &mapped);
        if (FAILED(result))
            fail(NativePortGraphicsFailure::DeviceLost,
                  static_cast<std::uint32_t>(result),
                  operation);
        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + upload_offset,
                    source,
                    byte_size);
        context_->Unmap(buffer, 0u);
        saturating_add(snapshot_.uploaded_bytes, byte_size);
        saturating_add(snapshot_.transient_geometry_uploaded_bytes,
                       byte_size);
        write_offset = upload_offset + required;
        return upload_offset;
    }

    void upload_texture(TextureSlot& slot,
                        const NativePortImageView& image,
                        const std::uint32_t mip_level) {
        const auto row_bytes =
            static_cast<std::size_t>(image.extent.width) * 4u;
        const auto subresource = D3D11CalcSubresource(
            mip_level, 0u, slot.config.mip_levels);
        if (slot.config.dynamic) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto result = context_->Map(
                slot.texture.Get(),
                subresource,
                D3D11_MAP_WRITE_DISCARD,
                0u,
                &mapped);
            if (FAILED(result))
                fail(NativePortGraphicsFailure::DeviceLost,
                     static_cast<std::uint32_t>(result),
                     "texture-map");
            if (mapped.RowPitch < row_bytes) {
                context_->Unmap(slot.texture.Get(), subresource);
                fail(NativePortGraphicsFailure::InvalidResource,
                     0u,
                     "texture-row-pitch");
            }
            for (std::uint32_t row = 0u; row < image.extent.height; ++row) {
                const auto source_row =
                    image.bottom_up ? image.extent.height - 1u - row : row;
                std::memcpy(
                    static_cast<std::byte*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    image.pixels.data() +
                        static_cast<std::size_t>(source_row) * image.stride_bytes,
                    row_bytes);
            }
            context_->Unmap(slot.texture.Get(), subresource);
        } else if (!image.bottom_up) {
            context_->UpdateSubresource(slot.texture.Get(),
                                        subresource,
                                        nullptr,
                                        image.pixels.data(),
                                        image.stride_bytes,
                                        0u);
        } else {
            std::vector<std::byte> normalized(
                row_bytes * image.extent.height);
            for (std::uint32_t row = 0u; row < image.extent.height; ++row) {
                const auto source_row = image.extent.height - 1u - row;
                std::memcpy(normalized.data() +
                                static_cast<std::size_t>(row) * row_bytes,
                            image.pixels.data() +
                                static_cast<std::size_t>(source_row) *
                                    image.stride_bytes,
                            row_bytes);
            }
            context_->UpdateSubresource(slot.texture.Get(),
                                        subresource,
                                        nullptr,
                                        normalized.data(),
                                        static_cast<UINT>(row_bytes),
                                        0u);
        }
        saturating_add(snapshot_.uploaded_bytes,
                       static_cast<std::uint64_t>(row_bytes) *
                           image.extent.height);
    }

    void invalidate_draw_state_shadow() noexcept {
        bound_viewport_valid_ = false;
        bound_blend_valid_ = false;
        bound_depth_valid_ = false;
        bound_rasterizer_valid_ = false;
        bound_topology_valid_ = false;
        bound_vertex_buffer_valid_ = false;
        bound_index_buffer_valid_ = false;
        bound_sampler_valid_ = false;
        bound_shader_resource_valid_ = false;
        draw_pipeline_bound_ = false;
    }

    void set_viewport(const NativePortPixelRect rect) {
        if (rect.width == 0u || rect.height == 0u)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "viewport-empty");
        if (bound_viewport_valid_ && bound_viewport_.x == rect.x &&
            bound_viewport_.y == rect.y &&
            bound_viewport_.width == rect.width &&
            bound_viewport_.height == rect.height)
            return;
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(rect.x);
        viewport.TopLeftY = static_cast<float>(rect.y);
        viewport.Width = static_cast<float>(rect.width);
        viewport.Height = static_cast<float>(rect.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1u, &viewport);
        const D3D11_RECT scissor{
            static_cast<LONG>(rect.x),
            static_cast<LONG>(rect.y),
            static_cast<LONG>(rect.x + rect.width),
            static_cast<LONG>(rect.y + rect.height)};
        context_->RSSetScissorRects(1u, &scissor);
        bound_viewport_ = rect;
        bound_viewport_valid_ = true;
    }

    [[nodiscard]] std::wstring environment_value(
        const wchar_t* const name) const {
        SetLastError(ERROR_SUCCESS);
        const auto required = GetEnvironmentVariableW(name, nullptr, 0u);
        if (required == 0u) {
            const auto error = GetLastError();
            if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND)
                return {};
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::InvalidConfig,
                static_cast<std::uint32_t>(error),
                "graphics-capture-environment");
        }
        std::wstring value(required, L'\0');
        const auto written =
            GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0u || written >= required)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::InvalidConfig,
                static_cast<std::uint32_t>(GetLastError()),
                "graphics-capture-environment");
        value.resize(written);
        return value;
    }

    [[nodiscard]] std::uint64_t environment_unsigned(
        const wchar_t* const name,
        const std::uint64_t fallback,
        const char* const operation) const {
        const auto value = environment_value(name);
        if (value.empty()) return fallback;
        std::uint64_t result = 0u;
        for (const auto character : value) {
            if (character < L'0' || character > L'9')
                throw NativePortGraphicsError(
                    NativePortGraphicsFailure::InvalidConfig,
                    1u,
                    operation);
            const auto digit =
                static_cast<std::uint64_t>(character - L'0');
            if (result >
                (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
                throw NativePortGraphicsError(
                    NativePortGraphicsFailure::InvalidConfig,
                    1u,
                    operation);
            result = result * 10u + digit;
        }
        return result;
    }

    void initialize_frame_capture() {
        const auto directory = environment_value(
            L"KATANA_NATIVE_GRAPHICS_CAPTURE_DIRECTORY");
        if (directory.empty()) return;
        capture_directory_ = std::filesystem::path(directory);
        capture_start_frame_ = environment_unsigned(
            L"KATANA_NATIVE_GRAPHICS_CAPTURE_START_FRAME",
            1u,
            "graphics-capture-start");
        capture_end_frame_ = environment_unsigned(
            L"KATANA_NATIVE_GRAPHICS_CAPTURE_END_FRAME",
            std::numeric_limits<std::uint64_t>::max(),
            "graphics-capture-end");
        capture_interval_ = environment_unsigned(
            L"KATANA_NATIVE_GRAPHICS_CAPTURE_INTERVAL",
            60u,
            "graphics-capture-interval");
        if (capture_start_frame_ == 0u || capture_interval_ == 0u ||
            capture_interval_ > 1'000'000u ||
            capture_end_frame_ < capture_start_frame_)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::InvalidConfig,
                1u,
                "graphics-capture-range");
        std::error_code error;
        std::filesystem::create_directories(capture_directory_, error);
        if (error)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::ResourceCreation,
                static_cast<std::uint32_t>(error.value()),
                "graphics-capture-directory");
        capture_enabled_ = true;
    }

    [[nodiscard]] bool should_capture_frame(
        const std::uint64_t frame) const noexcept {
        return capture_enabled_ && frame >= capture_start_frame_ &&
               frame <= capture_end_frame_ &&
               (frame - capture_start_frame_) % capture_interval_ == 0u;
    }

    void capture_completed_frame(const std::uint64_t frame) {
        if (!should_capture_frame(frame)) return;
        if (!capture_readback_) {
            D3D11_TEXTURE2D_DESC description{};
            render_texture_->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0u;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0u;
            const auto result = device_->CreateTexture2D(
                &description, nullptr, capture_readback_.GetAddressOf());
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     "graphics-capture-readback");
        }

        context_->CopyResource(capture_readback_.Get(), render_texture_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto map_result = context_->Map(
            capture_readback_.Get(), 0u, D3D11_MAP_READ, 0u, &mapped);
        if (FAILED(map_result))
            fail(NativePortGraphicsFailure::DeviceLost,
                 static_cast<std::uint32_t>(map_result),
                 "graphics-capture-map");
        struct Unmap final {
            ID3D11DeviceContext* context = nullptr;
            ID3D11Resource* resource = nullptr;
            ~Unmap() {
                if (context != nullptr && resource != nullptr)
                    context->Unmap(resource, 0u);
            }
        } unmap{context_.Get(), capture_readback_.Get()};

        const auto width = config_.render_extent.width;
        const auto height = config_.render_extent.height;
        const auto row_bytes = static_cast<std::size_t>(width) * 4u;
        const auto pixel_bytes = row_bytes * static_cast<std::size_t>(height);
        capture_pixels_.resize(pixel_bytes);
        const auto* const source =
            static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t output_y = 0u; output_y < height; ++output_y) {
            const auto source_y = height - output_y - 1u;
            const auto* const source_row =
                source + static_cast<std::size_t>(source_y) * mapped.RowPitch;
            auto* const output_row =
                capture_pixels_.data() +
                static_cast<std::size_t>(output_y) * row_bytes;
            for (std::uint32_t x = 0u; x < width; ++x) {
                output_row[x * 4u + 0u] = source_row[x * 4u + 2u];
                output_row[x * 4u + 1u] = source_row[x * 4u + 1u];
                output_row[x * 4u + 2u] = source_row[x * 4u + 0u];
                output_row[x * 4u + 3u] = source_row[x * 4u + 3u];
            }
        }
        unmap.context->Unmap(unmap.resource, 0u);
        unmap.context = nullptr;
        unmap.resource = nullptr;

        constexpr std::uint32_t header_bytes = 54u;
        if (pixel_bytes >
            std::numeric_limits<std::uint32_t>::max() - header_bytes)
            fail(NativePortGraphicsFailure::ResourceLimit,
                 1u,
                 "graphics-capture-size");
        std::array<std::uint8_t, header_bytes> header{};
        const auto store_u16 = [&](const std::size_t offset,
                                   const std::uint16_t value) {
            header[offset] = static_cast<std::uint8_t>(value);
            header[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };
        const auto store_u32 = [&](const std::size_t offset,
                                   const std::uint32_t value) {
            for (std::size_t byte = 0u; byte < 4u; ++byte)
                header[offset + byte] = static_cast<std::uint8_t>(
                    value >> static_cast<unsigned>(byte * 8u));
        };
        header[0] = static_cast<std::uint8_t>('B');
        header[1] = static_cast<std::uint8_t>('M');
        store_u32(2u, header_bytes + static_cast<std::uint32_t>(pixel_bytes));
        store_u32(10u, header_bytes);
        store_u32(14u, 40u);
        store_u32(18u, width);
        store_u32(22u, height);
        store_u16(26u, 1u);
        store_u16(28u, 32u);
        store_u32(34u, static_cast<std::uint32_t>(pixel_bytes));

        const auto output_path = capture_directory_ /
            (L"frame-" + std::to_wstring(frame) + L".bmp");
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output)
            fail(NativePortGraphicsFailure::ResourceCreation,
                 1u,
                 "graphics-capture-open");
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        output.write(reinterpret_cast<const char*>(capture_pixels_.data()),
                     static_cast<std::streamsize>(capture_pixels_.size()));
        if (!output)
            fail(NativePortGraphicsFailure::ResourceCreation,
                 1u,
                 "graphics-capture-write");
    }

    [[nodiscard]] static NativePortTextureHandle make_texture_handle(
        const std::uint32_t index,
        const std::uint32_t generation) noexcept {
        return {static_cast<std::uint64_t>(generation) << 32u |
                (static_cast<std::uint64_t>(index) + 1u)};
    }

    [[nodiscard]] static std::uint32_t texture_handle_index(
        const NativePortTextureHandle handle) noexcept {
        return static_cast<std::uint32_t>(handle.value) - 1u;
    }

    [[nodiscard]] TextureSlot& resolve_texture(
        const NativePortTextureHandle handle) {
        if (!handle || static_cast<std::uint32_t>(handle.value) == 0u)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "texture-handle");
        const auto index = texture_handle_index(handle);
        const auto generation = static_cast<std::uint32_t>(handle.value >> 32u);
        if (index >= texture_slots_.size() || generation == 0u ||
            !texture_slots_[index].live ||
            texture_slots_[index].generation != generation)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "texture-stale");
        return texture_slots_[index];
    }

    [[nodiscard]] static NativePortMeshHandle make_mesh_handle(
        const std::uint32_t index,
        const std::uint32_t generation) noexcept {
        return {static_cast<std::uint64_t>(generation) << 32u |
                (static_cast<std::uint64_t>(index) + 1u)};
    }

    [[nodiscard]] static std::uint32_t mesh_handle_index(
        const NativePortMeshHandle handle) noexcept {
        return static_cast<std::uint32_t>(handle.value) - 1u;
    }

    [[nodiscard]] MeshSlot& resolve_mesh(const NativePortMeshHandle handle) {
        if (!handle || static_cast<std::uint32_t>(handle.value) == 0u)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "mesh-handle");
        const auto index = mesh_handle_index(handle);
        const auto generation = static_cast<std::uint32_t>(handle.value >> 32u);
        if (index >= mesh_slots_.size() || generation == 0u ||
            !mesh_slots_[index].live ||
            mesh_slots_[index].generation != generation)
            fail(NativePortGraphicsFailure::InvalidResource,
                 0u,
                 "mesh-stale");
        return mesh_slots_[index];
    }

    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_)
            throw NativePortGraphicsError(
                NativePortGraphicsFailure::ThreadViolation,
                0u,
                "thread");
    }

    [[noreturn]] void fail(const NativePortGraphicsFailure failure,
                           const std::uint32_t code,
                           const char* const operation) {
        snapshot_.platform_error_code = code == 0u ? 1u : code;
        throw NativePortGraphicsError(
            failure, snapshot_.platform_error_code, operation);
    }

    std::string title_storage_;
    NativePortGraphicsConfig config_;
    std::thread::id owner_thread_;
    HWND window_ = nullptr;
    NativePortExtent output_extent_;
    NativePortExtent pending_output_extent_;
    bool close_requested_ = false;
    bool minimized_ = false;
    bool frame_open_ = false;
    bool completed_frame_available_ = false;
    NativePortDepthBufferConvention frame_depth_buffer_ =
        NativePortDepthBufferConvention::Forward;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> swap_chain_target_;
    ComPtr<ID3D11Texture2D> render_texture_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11ShaderResourceView> render_view_;
    ComPtr<ID3D11Texture2D> capture_readback_;
    ComPtr<ID3D11Texture2D> depth_texture_;
    ComPtr<ID3D11DepthStencilView> depth_view_;
    ComPtr<ID3D11VertexShader> draw_vertex_shader_;
    ComPtr<ID3D11PixelShader> draw_pixel_shader_;
    ComPtr<ID3D11VertexShader> composite_vertex_shader_;
    ComPtr<ID3D11PixelShader> composite_pixel_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11Buffer> draw_constants_;
    ComPtr<ID3D11Buffer> fog_table_constants_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> index_buffer_;
    UINT vertex_buffer_capacity_ = 0u;
    UINT index_buffer_capacity_ = 0u;
    UINT vertex_buffer_write_offset_ = 0u;
    UINT index_buffer_write_offset_ = 0u;
    bool vertex_buffer_discarded_ = false;
    bool index_buffer_discarded_ = false;
    std::array<float, native_port_fog_table_entries> last_fog_lookup_table_{};
    bool fog_lookup_table_valid_ = false;
    DrawConstants last_draw_constants_{};
    bool draw_constants_valid_ = false;
    NativePortBlendState last_blend_key_{};
    NativePortDepthState last_depth_key_{};
    NativePortRasterizerState last_rasterizer_key_{};
    NativePortSamplerState last_sampler_key_{};
    ID3D11BlendState* last_blend_state_ = nullptr;
    ID3D11DepthStencilState* last_depth_state_ = nullptr;
    ID3D11RasterizerState* last_rasterizer_state_ = nullptr;
    ID3D11SamplerState* last_sampler_state_ = nullptr;
    bool last_blend_key_valid_ = false;
    bool last_depth_key_valid_ = false;
    bool last_rasterizer_key_valid_ = false;
    bool last_sampler_key_valid_ = false;
    NativePortPixelRect bound_viewport_{};
    ID3D11BlendState* bound_blend_ = nullptr;
    ID3D11DepthStencilState* bound_depth_ = nullptr;
    ID3D11RasterizerState* bound_rasterizer_ = nullptr;
    ID3D11SamplerState* bound_sampler_ = nullptr;
    ID3D11Buffer* bound_vertex_buffer_ = nullptr;
    ID3D11Buffer* bound_index_buffer_ = nullptr;
    ID3D11ShaderResourceView* bound_shader_resource_ = nullptr;
    UINT bound_vertex_buffer_offset_ = 0u;
    UINT bound_index_buffer_offset_ = 0u;
    D3D11_PRIMITIVE_TOPOLOGY bound_topology_ =
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool bound_viewport_valid_ = false;
    bool bound_blend_valid_ = false;
    bool bound_depth_valid_ = false;
    bool bound_rasterizer_valid_ = false;
    bool bound_sampler_valid_ = false;
    bool bound_vertex_buffer_valid_ = false;
    bool bound_index_buffer_valid_ = false;
    bool bound_shader_resource_valid_ = false;
    bool bound_topology_valid_ = false;
    bool draw_pipeline_bound_ = false;
    std::vector<NativePortVertex> prepared_vertices_;
    std::vector<BlendStateSlot> blend_states_;
    std::vector<DepthStateSlot> depth_states_;
    std::vector<RasterizerStateSlot> rasterizer_states_;
    std::vector<SamplerStateSlot> sampler_states_;
    ComPtr<ID3D11Texture2D> white_texture_;
    ComPtr<ID3D11ShaderResourceView> white_view_;

    std::vector<TextureSlot> texture_slots_;
    std::vector<std::uint32_t> free_texture_slots_;
    std::vector<MeshSlot> mesh_slots_;
    std::vector<std::uint32_t> free_mesh_slots_;
    std::filesystem::path capture_directory_;
    std::vector<std::uint8_t> capture_pixels_;
    std::uint64_t capture_start_frame_ = 1u;
    std::uint64_t capture_end_frame_ =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t capture_interval_ = 60u;
    bool capture_enabled_ = false;
    std::uint64_t texture_bytes_ = 0u;
    std::uint64_t mesh_bytes_ = 0u;
    std::uint32_t live_textures_ = 0u;
    std::uint32_t live_meshes_ = 0u;
    NativePortTextureHandle image_texture_;
    NativePortExtent image_texture_extent_;
    NativePortTextureFormat image_texture_format_ =
        NativePortTextureFormat::Bgra8Unorm;
    NativePortGraphicsSnapshot snapshot_;
};

#else

class NativePortGraphicsDevice::Impl final {
  public:
    explicit Impl(const NativePortGraphicsConfig& config) {
        validate_graphics_config(config);
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::UnsupportedHost,
            1u,
            "unsupported-host");
    }

    void show() {}
    void poll_events() {}
    [[nodiscard]] NativePortLifecycleState lifecycle_state() const {
        return NativePortLifecycleState::Shutdown;
    }
    [[nodiscard]] NativePortGraphicsLayout layout() const {
        return {};
    }
    [[nodiscard]] NativePortTextureHandle create_texture(
        const NativePortTextureConfig&,
        std::span<const NativePortImageView>) {
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::UnsupportedHost, 1u, "unsupported-host");
    }
    void update_texture(NativePortTextureHandle, const NativePortImageView&) {}
    void update_texture(NativePortTextureHandle,
                        std::span<const NativePortImageView>) {}
    void destroy_texture(NativePortTextureHandle) {}
    [[nodiscard]] NativePortMeshHandle create_mesh(
        const NativePortMeshConfig&) {
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::UnsupportedHost, 1u, "unsupported-host");
    }
    void destroy_mesh(NativePortMeshHandle) {}
    void begin_frame(const NativePortFrameConfig&) {}
    void draw(const NativePortDrawPacket&) {}
    void present() {}
    void repeat_present() {}
    void present_image(const NativePortImageView&,
                       NativePortViewportTarget,
                       NativePortImageFit) {}
    [[nodiscard]] NativePortGraphicsSnapshot snapshot() const {
        return {};
    }
};

#endif

NativePortGraphicsDevice::NativePortGraphicsDevice(
    const NativePortGraphicsConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

NativePortGraphicsDevice::~NativePortGraphicsDevice() = default;

void NativePortGraphicsDevice::show() {
    impl_->show();
}

void NativePortGraphicsDevice::poll_events() {
    impl_->poll_events();
}

NativePortLifecycleState
NativePortGraphicsDevice::lifecycle_state() const {
    return impl_->lifecycle_state();
}

NativePortGraphicsLayout NativePortGraphicsDevice::layout() const {
    return impl_->layout();
}

NativePortTextureHandle NativePortGraphicsDevice::create_texture(
    const NativePortTextureConfig& config,
    const NativePortImageView* const initial_pixels) {
    return initial_pixels != nullptr
               ? impl_->create_texture(
                     config,
                     std::span<const NativePortImageView>(initial_pixels, 1u))
               : impl_->create_texture(
                     config, std::span<const NativePortImageView>{});
}

NativePortTextureHandle NativePortGraphicsDevice::create_texture(
    const NativePortTextureConfig& config,
    const std::span<const NativePortImageView> initial_mip_levels) {
    return impl_->create_texture(config, initial_mip_levels);
}

void NativePortGraphicsDevice::update_texture(
    const NativePortTextureHandle texture,
    const NativePortImageView& pixels) {
    impl_->update_texture(texture, pixels);
}

void NativePortGraphicsDevice::update_texture(
    const NativePortTextureHandle texture,
    const std::span<const NativePortImageView> mip_levels) {
    impl_->update_texture(texture, mip_levels);
}

void NativePortGraphicsDevice::destroy_texture(
    const NativePortTextureHandle texture) {
    impl_->destroy_texture(texture);
}

NativePortMeshHandle NativePortGraphicsDevice::create_mesh(
    const NativePortMeshConfig& config) {
    return impl_->create_mesh(config);
}

void NativePortGraphicsDevice::destroy_mesh(
    const NativePortMeshHandle mesh) {
    impl_->destroy_mesh(mesh);
}

void NativePortGraphicsDevice::begin_frame(
    const NativePortFrameConfig& config) {
    impl_->begin_frame(config);
}

void NativePortGraphicsDevice::draw(const NativePortDrawPacket& packet) {
    impl_->draw(packet);
}

void NativePortGraphicsDevice::present() {
    impl_->present();
}

void NativePortGraphicsDevice::repeat_present() {
    impl_->repeat_present();
}

void NativePortGraphicsDevice::present_image(
    const NativePortImageView& image,
    const NativePortViewportTarget viewport,
    const NativePortImageFit fit) {
    impl_->present_image(image, viewport, fit);
}

NativePortGraphicsSnapshot NativePortGraphicsDevice::snapshot() const {
    return impl_->snapshot();
}

NativePortDesktopHost::NativePortDesktopHost(
    const NativePortGraphicsConfig& graphics_config,
    const NativePortFramePacingConfig& frame_pacing_config)
    : graphics_(desktop_graphics_config(graphics_config,
                                        frame_pacing_config)),
      frame_pacing_config_(frame_pacing_config) {
    acquire_frame_pacing_timer_resolution(frame_pacing_config_);
    frame_pacing_snapshot_.simulation_rate_hz =
        frame_pacing_config_.simulation_rate_hz;
    frame_pacing_snapshot_.presentation_rate_hz =
        frame_pacing_config_.presentation_rate_hz;
    frame_pacing_snapshot_.enabled = frame_pacing_config_.enabled;
}

NativePortDesktopHost::~NativePortDesktopHost() {
    release_frame_pacing_timer_resolution(frame_pacing_config_);
}

std::uint64_t NativePortDesktopHost::monotonic_time_nanoseconds()
    const noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           elapsed)
                           .count();
    return count <= 0 ? 0u : static_cast<std::uint64_t>(count);
}

NativePortLifecycleState NativePortDesktopHost::poll_lifecycle() {
    // AOT safepoints can ask for lifecycle state far more often than a host
    // event loop needs to enter User32. Bound message-pump latency to 1 ms
    // without turning every statically chained SH-4 block into a host syscall.
    constexpr std::uint64_t event_poll_interval_nanoseconds = 1'000'000u;
    const auto now = monotonic_time_nanoseconds();
    if (now >= next_event_poll_nanoseconds_) {
        graphics_.poll_events();
        next_event_poll_nanoseconds_ =
            now > std::numeric_limits<std::uint64_t>::max() -
                      event_poll_interval_nanoseconds
                ? std::numeric_limits<std::uint64_t>::max()
                : now + event_poll_interval_nanoseconds;
    }
    return graphics_.lifecycle_state();
}

void NativePortDesktopHost::synchronize_simulation_boundary() {
    if (!frame_pacing_config_.enabled || !frame_pacing_started_)
        return;
    if (monotonic_time_nanoseconds() <
        next_simulation_deadline_nanoseconds_)
        wait_until_monotonic_nanoseconds(
            next_simulation_deadline_nanoseconds_);
}

void NativePortDesktopHost::begin_frame(const std::uint64_t frame_index) {
    static_cast<void>(frame_index);
    graphics_.begin_frame();
}

void NativePortDesktopHost::present_frame(const std::uint64_t frame_index) {
    static_cast<void>(frame_index);
    paced_present();
}

std::uint64_t NativePortDesktopHost::presented_frames() const noexcept {
    return graphics_.snapshot().presented_frames;
}

NativePortFramePacingSnapshot
NativePortDesktopHost::frame_pacing_snapshot() const noexcept {
    return frame_pacing_snapshot_;
}

void NativePortDesktopHost::paced_present() {
    const auto present_and_record = [this](const bool repeated) {
        const auto before = graphics_.snapshot().presented_frames;
        const bool repeat_completed_frame =
            repeated || !graphics_.snapshot().frame_open;
        if (repeat_completed_frame)
            graphics_.repeat_present();
        else
            graphics_.present();
        const auto after = graphics_.snapshot().presented_frames;
        if (after > before) {
            const auto presented = after - before;
            saturating_add(
                frame_pacing_snapshot_.presentation_frames, presented);
            if (repeat_completed_frame)
                saturating_add(
                    frame_pacing_snapshot_.repeated_presentations,
                    presented);
        }
    };

    if (!frame_pacing_config_.enabled) {
        present_and_record(false);
        saturating_increment(frame_pacing_snapshot_.simulation_frames);
        return;
    }

    auto now = monotonic_time_nanoseconds();
    if (!frame_pacing_started_) {
        // The first completed frame establishes both native epochs and is
        // visible immediately. Subsequent title updates run while the first
        // simulation interval elapses and are gated at this boundary.
        present_and_record(false);
        saturating_increment(frame_pacing_snapshot_.simulation_frames);
        now = monotonic_time_nanoseconds();
        next_simulation_deadline_nanoseconds_ = now;
        next_presentation_deadline_nanoseconds_ = now;
        advance_frame_deadline(
            next_simulation_deadline_nanoseconds_,
            simulation_deadline_remainder_,
            frame_pacing_config_.simulation_rate_hz);
        advance_frame_deadline(
            next_presentation_deadline_nanoseconds_,
            presentation_deadline_remainder_,
            frame_pacing_config_.presentation_rate_hz);
        frame_pacing_started_ = true;
        return;
    }

    const bool simulation_late =
        now > next_simulation_deadline_nanoseconds_;
    if (now >= next_simulation_deadline_nanoseconds_) {
        // A normal host wait resumes a little after its deadline. Preserve the
        // phase for that sub-frame lateness instead of accumulating scheduler
        // overshoot into title time. Only a complete additional missed
        // simulation interval reanchors the clocks; catch-up updates are never
        // issued.
        present_and_record(false);
        saturating_increment(frame_pacing_snapshot_.simulation_frames);
        now = monotonic_time_nanoseconds();
        advance_frame_deadline(
            next_simulation_deadline_nanoseconds_,
            simulation_deadline_remainder_,
            frame_pacing_config_.simulation_rate_hz);
        if (next_presentation_deadline_nanoseconds_ <= now)
            advance_frame_deadline(
                next_presentation_deadline_nanoseconds_,
                presentation_deadline_remainder_,
                frame_pacing_config_.presentation_rate_hz);
        while (next_presentation_deadline_nanoseconds_ <= now) {
            saturating_increment(
                frame_pacing_snapshot_.missed_presentation_deadlines);
            advance_frame_deadline(
                next_presentation_deadline_nanoseconds_,
                presentation_deadline_remainder_,
                frame_pacing_config_.presentation_rate_hz);
        }
        if (simulation_late)
            saturating_increment(
                frame_pacing_snapshot_.late_simulation_frames);
        if (next_simulation_deadline_nanoseconds_ <= now) {
            next_simulation_deadline_nanoseconds_ = now;
            next_presentation_deadline_nanoseconds_ = now;
            simulation_deadline_remainder_ = 0u;
            presentation_deadline_remainder_ = 0u;
            advance_frame_deadline(
                next_simulation_deadline_nanoseconds_,
                simulation_deadline_remainder_,
                frame_pacing_config_.simulation_rate_hz);
            advance_frame_deadline(
                next_presentation_deadline_nanoseconds_,
                presentation_deadline_remainder_,
                frame_pacing_config_.presentation_rate_hz);
        }
        return;
    }

    // A new title frame is ready before its next simulation boundary. Present
    // it on the next output deadline, then repeat only that completed GPU
    // frame on additional output deadlines. Title state does not advance
    // again until the simulation boundary below is reached.
    if (now > next_presentation_deadline_nanoseconds_) {
        saturating_increment(
            frame_pacing_snapshot_.missed_presentation_deadlines);
        present_and_record(false);
        now = monotonic_time_nanoseconds();
        advance_frame_deadline(
            next_presentation_deadline_nanoseconds_,
            presentation_deadline_remainder_,
            frame_pacing_config_.presentation_rate_hz);
        while (next_presentation_deadline_nanoseconds_ <= now) {
            saturating_increment(
                frame_pacing_snapshot_.missed_presentation_deadlines);
            advance_frame_deadline(
                next_presentation_deadline_nanoseconds_,
                presentation_deadline_remainder_,
                frame_pacing_config_.presentation_rate_hz);
        }
    } else {
        wait_until_monotonic_nanoseconds(
            next_presentation_deadline_nanoseconds_);
        present_and_record(false);
        advance_frame_deadline(
            next_presentation_deadline_nanoseconds_,
            presentation_deadline_remainder_,
            frame_pacing_config_.presentation_rate_hz);
    }
    saturating_increment(frame_pacing_snapshot_.simulation_frames);

    for (;;) {
        now = monotonic_time_nanoseconds();
        while (next_presentation_deadline_nanoseconds_ <= now &&
               next_presentation_deadline_nanoseconds_ <=
                   next_simulation_deadline_nanoseconds_) {
            saturating_increment(
                frame_pacing_snapshot_.missed_presentation_deadlines);
            advance_frame_deadline(
                next_presentation_deadline_nanoseconds_,
                presentation_deadline_remainder_,
                frame_pacing_config_.presentation_rate_hz);
        }
        if (next_presentation_deadline_nanoseconds_ >
            next_simulation_deadline_nanoseconds_)
            break;
        wait_until_monotonic_nanoseconds(
            next_presentation_deadline_nanoseconds_);
        present_and_record(true);
        advance_frame_deadline(
            next_presentation_deadline_nanoseconds_,
            presentation_deadline_remainder_,
            frame_pacing_config_.presentation_rate_hz);
    }

    if (monotonic_time_nanoseconds() <
        next_simulation_deadline_nanoseconds_) {
        wait_until_monotonic_nanoseconds(
            next_simulation_deadline_nanoseconds_);
    }
    advance_frame_deadline(
        next_simulation_deadline_nanoseconds_,
        simulation_deadline_remainder_,
        frame_pacing_config_.simulation_rate_hz);
    now = monotonic_time_nanoseconds();
    if (next_simulation_deadline_nanoseconds_ <= now) {
        // Presentation itself consumed a complete additional simulation
        // interval. Resynchronise once; never run multiple updates to catch up.
        saturating_increment(
            frame_pacing_snapshot_.late_simulation_frames);
        next_simulation_deadline_nanoseconds_ = now;
        simulation_deadline_remainder_ = 0u;
        advance_frame_deadline(
            next_simulation_deadline_nanoseconds_,
            simulation_deadline_remainder_,
            frame_pacing_config_.simulation_rate_hz);
    }
}

NativePortGraphicsDevice& NativePortDesktopHost::graphics() noexcept {
    return graphics_;
}

const NativePortGraphicsDevice&
NativePortDesktopHost::graphics() const noexcept {
    return graphics_;
}

NativePortGraphicsError::NativePortGraphicsError(
    const NativePortGraphicsFailure failure,
    const std::uint32_t platform_error_code,
    const std::string_view operation)
    : std::runtime_error(
          "native-port-graphics-" + std::string(operation) + ":" +
          std::to_string(platform_error_code)),
      failure_(failure), platform_error_code_(platform_error_code) {}

NativePortGraphicsFailure NativePortGraphicsError::failure() const noexcept {
    return failure_;
}

std::uint32_t NativePortGraphicsError::platform_error_code() const noexcept {
    return platform_error_code_;
}

#ifdef _WIN32

namespace {

const wchar_t native_graphics_window_class[] =
    L"KatanaRecompNativeGraphicsV1";

constexpr char native_graphics_shader_source[] = R"(
cbuffer DrawConstants : register(b0) {
    row_major float4x4 draw_transform;
    row_major float4x4 draw_normal_transform;
    float4 material_diffuse;
    float4 material_ambient;
    float4 material_specular;
    float4 material_emission;
    float4 scene_ambient;
    float4 fog_color;
    float4 light_directions[4];
    float4 light_colors[4];
    float4 fog_parameters;
    float4 depth_parameters;
    float4 material_parameters;
    uint4 pipeline_flags;
};

cbuffer FogTableConstants : register(b1) {
    float4 fog_lookup_table[32];
};

struct DrawVertexInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR0;
    float3 normal : NORMAL0;
    float4 secondary_color : COLOR1;
    float fog_coordinate : TEXCOORD1;
    float depth_coordinate : TEXCOORD2;
};

struct DrawVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    noperspective float4 color : COLOR0;
    float3 normal : NORMAL0;
    noperspective float4 secondary_color : COLOR1;
    noperspective float fog_coordinate : TEXCOORD1;
    noperspective float depth_coordinate : TEXCOORD2;
    noperspective float2 reciprocal_texcoord : TEXCOORD3;
};

DrawVertexOutput draw_vertex_main(DrawVertexInput input) {
    DrawVertexOutput output;
    output.position = mul(float4(input.position, 1.0), draw_transform);
    if ((pipeline_flags.x & 0x20u) != 0u &&
        (pipeline_flags.x & 0x10000u) == 0u)
        output.position.z = 0.0;
    output.normal = normalize(
        mul(float4(input.normal, 0.0), draw_normal_transform).xyz);
    output.texcoord = (pipeline_flags.x & 0x10u) != 0u
        ? float2(-0.5 * output.normal.x + 0.5,
                  0.5 * output.normal.y + 0.5)
        : input.texcoord;
    output.color = input.color;
    output.secondary_color = input.secondary_color;
    output.fog_coordinate = input.fog_coordinate;
    output.depth_coordinate = input.depth_coordinate;
    output.reciprocal_texcoord =
        output.texcoord * input.depth_coordinate;
    return output;
}

Texture2D draw_texture : register(t0);
SamplerState draw_sampler : register(s0);

bool alpha_test_passes(float alpha, uint operation, float reference) {
    if (operation == 0u) return false;
    if (operation == 1u) return alpha < reference;
    if (operation == 2u) return alpha == reference;
    if (operation == 3u) return alpha <= reference;
    if (operation == 4u) return alpha > reference;
    if (operation == 5u) return alpha != reference;
    if (operation == 6u) return alpha >= reference;
    return true;
}

float fog_lookup_value(uint index) {
    const uint bounded_index = min(index, 127u);
    const uint vector_index = bounded_index >> 2u;
    const uint component = bounded_index & 3u;
    const float4 coefficients = fog_lookup_table[vector_index];
    if (component == 0u) return coefficients.x;
    if (component == 1u) return coefficients.y;
    if (component == 2u) return coefficients.z;
    return coefficients.w;
}

float lookup_table_fog(float coordinate, float density) {
    const float z = clamp(density * coordinate, 1.0, 255.9999);
    const float exponent = floor(log2(z));
    const float mantissa = z * 16.0 / exp2(exponent) - 16.0;
    const uint index = (uint)floor(mantissa + exponent * 16.0);
    const float fraction = mantissa - floor(mantissa);
    return saturate(lerp(fog_lookup_value(index),
                         fog_lookup_value(index + 1u),
                         fraction));
}

struct DrawPixelOutput {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

DrawPixelOutput draw_pixel_main(DrawVertexOutput input) {
    const uint flags = pipeline_flags.x;
    const bool homogeneous_reciprocal_clip = (flags & 0x10000u) != 0u;
    const bool screen_space_reciprocal =
        (flags & 0x20u) != 0u && !homogeneous_reciprocal_clip;
    const float reciprocal_coordinate = max(
        homogeneous_reciprocal_clip
            ? input.position.w
            : input.depth_coordinate,
        0.0);
    const bool vertex_color_enabled = (flags & 0x01u) != 0u;
    const bool secondary_color_enabled = (flags & 0x02u) != 0u;
    const bool lighting_enabled = (flags & 0x04u) != 0u;
    const bool specular_enabled = (flags & 0x08u) != 0u;
    const bool primary_alpha_enabled = (flags & 0x40u) != 0u;
    const bool texture_alpha_enabled = (flags & 0x80u) != 0u;
    const bool texture_present = (flags & 0x20000u) != 0u;
    float4 primary = material_diffuse;
    float3 post_color = material_emission.rgb;
    if (lighting_enabled) {
        const float3 normal = normalize(input.normal);
        float3 diffuse_light = scene_ambient.rgb * material_ambient.rgb;
        float3 specular_light = 0.0;
        [unroll]
        for (uint index = 0u; index < 4u; ++index) {
            if (index >= pipeline_flags.y) break;
            const float3 direction = normalize(light_directions[index].xyz);
            const float diffuse_amount = saturate(dot(normal, direction));
            diffuse_light += light_colors[index].rgb * diffuse_amount;
            if (specular_enabled && diffuse_amount > 0.0) {
                const float3 half_vector =
                    normalize(direction + float3(0.0, 0.0, 1.0));
                const float specular_amount = pow(
                    saturate(dot(normal, half_vector)),
                    max(material_parameters.x, 0.0001));
                specular_light += light_colors[index].rgb *
                                  material_specular.rgb * specular_amount;
            }
        }
        primary.rgb = material_diffuse.rgb * diffuse_light;
        post_color += specular_light;
    }
    if (vertex_color_enabled) primary *= input.color;
    if (!primary_alpha_enabled) primary.a = 1.0;

    if (pipeline_flags.z == 6u) {
        const float primary_fog = lookup_table_fog(
            homogeneous_reciprocal_clip
                ? reciprocal_coordinate
                : input.fog_coordinate,
            fog_parameters.z);
        primary = float4(fog_color.rgb, primary_fog);
    }

    float4 result = primary;
    if (texture_present) {
        const float2 texture_coordinate = screen_space_reciprocal
            ? input.reciprocal_texcoord / input.depth_coordinate
            : input.texcoord;
        float4 texture_color =
            draw_texture.Sample(draw_sampler, texture_coordinate);
        if (!texture_alpha_enabled) texture_color.a = 1.0;
        const uint texture_combine = (flags >> 8u) & 0xffu;
        result = primary * texture_color;
        if (texture_combine == 1u) {
            result = texture_color;
        } else if (texture_combine == 2u) {
            result.rgb = lerp(primary.rgb, texture_color.rgb, texture_color.a);
            result.a = primary.a;
        } else if (texture_combine == 3u) {
            result = primary + texture_color;
        } else if (texture_combine == 4u) {
            result.rgb = primary.rgb * texture_color.rgb;
            result.a = texture_color.a;
        }
    }
    result.rgb += post_color;
    if (secondary_color_enabled)
        result.rgb = saturate(result.rgb + input.secondary_color.rgb);

    const uint alpha_test = pipeline_flags.w;
    if ((alpha_test & 0x100u) != 0u &&
        !alpha_test_passes(result.a, alpha_test & 0xffu,
                           material_parameters.y))
        discard;

    float fog_amount = 0.0;
    if (pipeline_flags.z == 1u) {
        fog_amount = saturate(input.fog_coordinate);
    } else if (pipeline_flags.z == 2u) {
        fog_amount = saturate(
            (input.fog_coordinate - fog_parameters.x) /
            (fog_parameters.y - fog_parameters.x));
    } else if (pipeline_flags.z == 3u) {
        fog_amount = saturate(
            1.0 - exp(-fog_parameters.z * input.fog_coordinate));
    } else if (pipeline_flags.z == 4u) {
        const float fog_distance =
            fog_parameters.z * input.fog_coordinate;
        fog_amount = saturate(1.0 - exp(-(fog_distance * fog_distance)));
    } else if (pipeline_flags.z == 5u) {
        fog_amount = lookup_table_fog(
            homogeneous_reciprocal_clip
                ? reciprocal_coordinate
                : input.fog_coordinate,
            fog_parameters.z);
    }
    result.rgb = lerp(result.rgb, fog_color.rgb, fog_amount);
    DrawPixelOutput output;
    output.color = result;
    output.depth = input.position.z;
    if ((flags & 0x20u) != 0u) {
        output.depth = saturate(
            log2(1.0 + depth_parameters.x * reciprocal_coordinate) /
            depth_parameters.y);
    }
    return output;
}

struct CompositeVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

CompositeVertexOutput composite_vertex_main(uint vertex_id : SV_VertexID) {
    static const float2 positions[3] = {
        float2(-1.0,  1.0),
        float2( 3.0,  1.0),
        float2(-1.0, -3.0)
    };
    static const float2 texcoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };
    CompositeVertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.texcoord = texcoords[vertex_id];
    return output;
}

Texture2D composite_texture : register(t0);
SamplerState composite_sampler : register(s0);

float4 composite_pixel_main(CompositeVertexOutput input) : SV_Target {
    return composite_texture.Sample(composite_sampler, input.texcoord);
}
)";

struct ShaderCompilerApi final {
    HMODULE module = nullptr;
    decltype(&D3DCompile) compile = nullptr;
};

[[nodiscard]] const ShaderCompilerApi& shader_compiler_api() noexcept {
    static const auto api = []() noexcept {
        constexpr std::array names{
            L"d3dcompiler_47.dll",
            L"d3dcompiler_46.dll",
            L"d3dcompiler_43.dll",
        };
        for (const auto* name : names) {
            const auto module = LoadLibraryW(name);
            if (module == nullptr) continue;
            const auto compile = reinterpret_cast<decltype(&D3DCompile)>(
                GetProcAddress(module, "D3DCompile"));
            if (compile != nullptr) return ShaderCompilerApi{module, compile};
            FreeLibrary(module);
        }
        return ShaderCompilerApi{};
    }();
    return api;
}

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(
    const char* entry,
    const char* target) {
    const auto compile = shader_compiler_api().compile;
    if (compile == nullptr)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::ShaderCompilation,
            static_cast<std::uint32_t>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)),
            "shader-compiler");
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const auto result = compile(native_graphics_shader_source,
                                sizeof(native_graphics_shader_source) - 1u,
                                "katana-native-port-graphics",
                                nullptr,
                                nullptr,
                                entry,
                                target,
                                D3DCOMPILE_ENABLE_STRICTNESS |
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0u,
                                bytecode.GetAddressOf(),
                                diagnostics.GetAddressOf());
    if (FAILED(result))
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::ShaderCompilation,
            static_cast<std::uint32_t>(result),
            entry);
    return bytecode;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>(
                           std::numeric_limits<int>::max()))
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig, 0u, "title");
    const auto count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig,
            GetLastError(),
            "title-utf8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            count) != count)
        throw NativePortGraphicsError(
            NativePortGraphicsFailure::InvalidConfig,
            GetLastError(),
            "title-convert");
    return result;
}

[[nodiscard]] DXGI_FORMAT texture_format(
    const NativePortTextureFormat format) noexcept {
    return format == NativePortTextureFormat::Bgra8Unorm
               ? DXGI_FORMAT_B8G8R8A8_UNORM
               : DXGI_FORMAT_R8G8B8A8_UNORM;
}

[[nodiscard]] D3D11_PRIMITIVE_TOPOLOGY primitive_topology(
    const NativePortPrimitiveTopology topology) noexcept {
    switch (topology) {
    case NativePortPrimitiveTopology::PointList:
        return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case NativePortPrimitiveTopology::LineList:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case NativePortPrimitiveTopology::LineStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case NativePortPrimitiveTopology::TriangleList:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case NativePortPrimitiveTopology::TriangleStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

[[nodiscard]] D3D11_BLEND blend_factor(
    const NativePortBlendFactor factor) noexcept {
    switch (factor) {
    case NativePortBlendFactor::Zero: return D3D11_BLEND_ZERO;
    case NativePortBlendFactor::One: return D3D11_BLEND_ONE;
    case NativePortBlendFactor::SourceColor: return D3D11_BLEND_SRC_COLOR;
    case NativePortBlendFactor::InverseSourceColor:
        return D3D11_BLEND_INV_SRC_COLOR;
    case NativePortBlendFactor::DestinationColor:
        return D3D11_BLEND_DEST_COLOR;
    case NativePortBlendFactor::InverseDestinationColor:
        return D3D11_BLEND_INV_DEST_COLOR;
    case NativePortBlendFactor::SourceAlpha: return D3D11_BLEND_SRC_ALPHA;
    case NativePortBlendFactor::InverseSourceAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case NativePortBlendFactor::DestinationAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case NativePortBlendFactor::InverseDestinationAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    }
    return D3D11_BLEND_ZERO;
}

[[nodiscard]] D3D11_BLEND_OP blend_operation(
    const NativePortBlendOperation operation) noexcept {
    switch (operation) {
    case NativePortBlendOperation::Add: return D3D11_BLEND_OP_ADD;
    case NativePortBlendOperation::Subtract: return D3D11_BLEND_OP_SUBTRACT;
    case NativePortBlendOperation::ReverseSubtract:
        return D3D11_BLEND_OP_REV_SUBTRACT;
    case NativePortBlendOperation::Minimum: return D3D11_BLEND_OP_MIN;
    case NativePortBlendOperation::Maximum: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

[[nodiscard]] D3D11_COMPARISON_FUNC comparison_function(
    const NativePortCompareOperation compare) noexcept {
    switch (compare) {
    case NativePortCompareOperation::Never: return D3D11_COMPARISON_NEVER;
    case NativePortCompareOperation::Less: return D3D11_COMPARISON_LESS;
    case NativePortCompareOperation::Equal: return D3D11_COMPARISON_EQUAL;
    case NativePortCompareOperation::LessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case NativePortCompareOperation::Greater: return D3D11_COMPARISON_GREATER;
    case NativePortCompareOperation::NotEqual:
        return D3D11_COMPARISON_NOT_EQUAL;
    case NativePortCompareOperation::GreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case NativePortCompareOperation::Always: return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_NEVER;
}

[[nodiscard]] D3D11_TEXTURE_ADDRESS_MODE texture_address_mode(
    const NativePortTextureAddress address) noexcept {
    switch (address) {
    case NativePortTextureAddress::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case NativePortTextureAddress::Wrap: return D3D11_TEXTURE_ADDRESS_WRAP;
    case NativePortTextureAddress::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
    }
    return D3D11_TEXTURE_ADDRESS_CLAMP;
}

[[nodiscard]] D3D11_FILTER texture_filter(
    const NativePortTextureFilter filter) noexcept {
    switch (filter) {
    case NativePortTextureFilter::Point:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case NativePortTextureFilter::Bilinear:
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case NativePortTextureFilter::Trilinear:
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    case NativePortTextureFilter::Anisotropic:
        return D3D11_FILTER_ANISOTROPIC;
    }
    return D3D11_FILTER_MIN_MAG_MIP_POINT;
}

[[nodiscard]] UINT next_buffer_capacity(const UINT required) noexcept {
    if (required == 0u) return 0u;
    constexpr auto highest_power_of_two = UINT{1u} << 31u;
    if (required > highest_power_of_two) return required;
    const auto rounded = std::bit_ceil(required);
    return rounded >= required ? rounded : required;
}

} // namespace

#endif

} // namespace katana::runtime
