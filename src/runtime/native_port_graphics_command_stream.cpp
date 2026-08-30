#include "native_port_graphics_command_stream.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::size_t max_u32 =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

template <typename T>
[[nodiscard]] bool checked_add(const T left,
                               const T right,
                               T& result) noexcept {
    if (right > std::numeric_limits<T>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_align_up(const std::size_t value,
                                    const std::size_t alignment,
                                    std::size_t& result) noexcept {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return false;
    const auto mask = alignment - 1u;
    if (value > std::numeric_limits<std::size_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

[[nodiscard]] bool checked_u32(const std::size_t value,
                               std::uint32_t& result) noexcept {
    if (value > max_u32) return false;
    result = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool valid_extent(const NativePortExtent extent) noexcept {
    return extent.width != 0u && extent.height != 0u &&
           extent.width <= native_port_graphics_command_stream_max_dimension &&
           extent.height <= native_port_graphics_command_stream_max_dimension;
}

[[nodiscard]] bool valid_texture_format(
    const NativePortTextureFormat format) noexcept {
    return format == NativePortTextureFormat::Rgba8Unorm ||
           format == NativePortTextureFormat::Bgra8Unorm;
}

[[nodiscard]] bool valid_topology(
    const NativePortPrimitiveTopology topology) noexcept {
    switch (topology) {
    case NativePortPrimitiveTopology::PointList:
    case NativePortPrimitiveTopology::LineList:
    case NativePortPrimitiveTopology::LineStrip:
    case NativePortPrimitiveTopology::TriangleList:
    case NativePortPrimitiveTopology::TriangleStrip:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_shading(
    const NativePortShadingMode shading) noexcept {
    return shading == NativePortShadingMode::Smooth ||
           shading == NativePortShadingMode::FlatLastVertex;
}

[[nodiscard]] bool valid_viewport(
    const NativePortViewportTarget viewport) noexcept {
    return viewport == NativePortViewportTarget::Game ||
           viewport == NativePortViewportTarget::Ui;
}

[[nodiscard]] bool valid_image_fit(const NativePortImageFit fit) noexcept {
    return fit == NativePortImageFit::Contain ||
           fit == NativePortImageFit::Cover || fit == NativePortImageFit::Stretch;
}

[[nodiscard]] bool valid_image_aspect(const NativePortImageView& image) noexcept {
    const bool implicit = image.display_aspect_numerator == 0u &&
                          image.display_aspect_denominator == 0u;
    const bool explicit_aspect = image.display_aspect_numerator != 0u &&
                                 image.display_aspect_denominator != 0u &&
                                 image.display_aspect_numerator <= 65'535u &&
                                 image.display_aspect_denominator <= 65'535u;
    return implicit || explicit_aspect;
}

[[nodiscard]] bool valid_image(const NativePortImageView& image,
                               const NativePortExtent* expected_extent = nullptr,
                               const NativePortTextureFormat* expected_format =
                                   nullptr) noexcept {
    static_cast<void>(expected_extent);
    static_cast<void>(expected_format);
    // The codec owns only address-free transport. Extent, format, stride,
    // aspect and required-pixel semantics remain backend contracts so their
    // typed NativePortGraphicsError is returned with the command ordinal.
    return image.pixels.size() <= max_u32;
}

[[nodiscard]] NativePortExtent texture_mip_extent(
    const NativePortExtent extent,
    const std::uint32_t level) noexcept {
    return {std::max(extent.width >> level, 1u),
            std::max(extent.height >> level, 1u)};
}

[[nodiscard]] bool valid_texture_provenance(
    const NativePortTextureProvenance& provenance,
    const NativePortTextureConfig& config) noexcept {
    const auto zero_content = std::all_of(
        provenance.content_sha256.begin(), provenance.content_sha256.end(),
        [](const std::uint8_t value) { return value == 0u; });
    const auto zero_decoded = std::all_of(
        provenance.decoded_rgba8_sha256.begin(),
        provenance.decoded_rgba8_sha256.end(),
        [](const std::uint8_t value) { return value == 0u; });
    if (provenance.content_identity_bound
            ? provenance.generation == 0u || zero_content
            : provenance.generation != 0u || provenance.archive_ordinal != 0u ||
                  !zero_content || provenance.global_index_bound ||
                  provenance.global_index != 0u) {
        return false;
    }
    if (!provenance.global_index_bound && provenance.global_index != 0u)
        return false;
    if (provenance.decoded_payload_identity_bound
            ? zero_decoded || provenance.decoded_extent != config.extent ||
                  provenance.decoded_mip_levels != config.mip_levels
            : !zero_decoded || provenance.source_pixel_format != 0u ||
                  provenance.source_data_format != 0u ||
                  provenance.decoded_extent != NativePortExtent{} ||
                  provenance.decoded_mip_levels != 0u) {
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_texture_config(
    const NativePortTextureConfig& config) noexcept {
    if (!valid_extent(config.extent) || !valid_texture_format(config.format) ||
        !config.shader_resource || config.mip_levels == 0u ||
        config.mip_levels > native_port_graphics_command_stream_max_mip_levels ||
        config.dynamic && config.mip_levels != 1u ||
        config.mip_levels >
            static_cast<std::uint32_t>(std::bit_width(
                std::max(config.extent.width, config.extent.height)))) {
        return false;
    }
    return valid_texture_provenance(config.provenance, config);
}

template <std::size_t Size>
[[nodiscard]] bool finite_array(const std::array<float, Size>& values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] bool valid_blend_factor(
    const NativePortBlendFactor factor) noexcept {
    switch (factor) {
    case NativePortBlendFactor::Zero:
    case NativePortBlendFactor::One:
    case NativePortBlendFactor::SourceColor:
    case NativePortBlendFactor::InverseSourceColor:
    case NativePortBlendFactor::DestinationColor:
    case NativePortBlendFactor::InverseDestinationColor:
    case NativePortBlendFactor::SourceAlpha:
    case NativePortBlendFactor::InverseSourceAlpha:
    case NativePortBlendFactor::DestinationAlpha:
    case NativePortBlendFactor::InverseDestinationAlpha:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_blend_operation(
    const NativePortBlendOperation operation) noexcept {
    switch (operation) {
    case NativePortBlendOperation::Add:
    case NativePortBlendOperation::Subtract:
    case NativePortBlendOperation::ReverseSubtract:
    case NativePortBlendOperation::Minimum:
    case NativePortBlendOperation::Maximum:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_blend(const NativePortBlendState& blend) noexcept {
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
    switch (compare) {
    case NativePortCompareOperation::Never:
    case NativePortCompareOperation::Less:
    case NativePortCompareOperation::Equal:
    case NativePortCompareOperation::LessEqual:
    case NativePortCompareOperation::Greater:
    case NativePortCompareOperation::NotEqual:
    case NativePortCompareOperation::GreaterEqual:
    case NativePortCompareOperation::Always:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_depth(const NativePortDepthState& depth) noexcept {
    return valid_compare(depth.compare) &&
           (!depth.write_enabled || depth.test_enabled);
}

[[nodiscard]] bool valid_interpolation(
    const NativePortInterpolationMode interpolation) noexcept {
    return interpolation == NativePortInterpolationMode::PerspectiveCorrect ||
           interpolation == NativePortInterpolationMode::PvrScreenGouraud;
}

[[nodiscard]] bool valid_depth_mapping(
    const NativePortDepthMapping& mapping) noexcept {
    switch (mapping.mode) {
    case NativePortDepthCoordinateMode::ClipSpace:
    case NativePortDepthCoordinateMode::ReciprocalPositive:
    case NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip:
        break;
    default:
        return false;
    }
    return std::isfinite(mapping.reciprocal_scale) &&
           std::isfinite(mapping.logarithm_divisor) &&
           mapping.reciprocal_scale > 0.0f &&
           mapping.logarithm_divisor > 0.0f;
}

[[nodiscard]] bool valid_texture_stage(
    const NativePortTextureStage stage) noexcept {
    return stage == NativePortTextureStage::Disabled ||
           stage == NativePortTextureStage::RequiredResolved;
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
           valid_address(sampler.address_v) && valid_address(sampler.address_w) &&
           std::isfinite(sampler.mip_lod_bias) &&
           std::isfinite(sampler.minimum_lod) &&
           std::isfinite(sampler.maximum_lod) &&
           sampler.mip_lod_bias >= -16.0f && sampler.mip_lod_bias <= 15.99f &&
           sampler.minimum_lod >= 0.0f &&
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
        !std::isfinite(fog.density) || fog.density < 0.0f)
        return false;
    if (fog.mode == NativePortFogMode::Linear && fog.end <= fog.start)
        return false;
    if (fog.mode != NativePortFogMode::LookupTable &&
        fog.mode != NativePortFogMode::LookupTablePrimary)
        return true;
    if (fog.density <= 0.0f || !finite_array(fog.lookup_table)) return false;
    return std::all_of(
        fog.lookup_table.begin(), fog.lookup_table.end(),
        [](const float value) { return value >= 0.0f && value <= 1.0f; });
}

[[nodiscard]] bool valid_color_clamp(
    const NativePortColorClampState& color_clamp) noexcept {
    if (!finite_array(color_clamp.minimum) ||
        !finite_array(color_clamp.maximum))
        return false;
    for (std::size_t component = 0u;
         component < color_clamp.minimum.size(); ++component) {
        if (color_clamp.minimum[component] < 0.0f ||
            color_clamp.maximum[component] > 1.0f ||
            color_clamp.minimum[component] > color_clamp.maximum[component])
            return false;
    }
    return true;
}

[[nodiscard]] bool valid_alpha_test(
    const NativePortAlphaTestState& alpha_test) noexcept {
    using Mode = NativePortAlphaTestState::Mode;
    if (!valid_compare(alpha_test.compare) ||
        !std::isfinite(alpha_test.reference) || alpha_test.reference < 0.0f ||
        alpha_test.reference > 1.0f ||
        (alpha_test.mode != Mode::FloatingPoint &&
         alpha_test.mode != Mode::Quantized8BitForceOpaque))
        return false;
    if (alpha_test.mode == Mode::Quantized8BitForceOpaque) {
        const float canonical_reference =
            static_cast<float>(alpha_test.reference_8bit) / 255.0f;
        return alpha_test.enabled &&
               alpha_test.compare == NativePortCompareOperation::GreaterEqual &&
               alpha_test.reference == canonical_reference;
    }
    return alpha_test.reference_8bit == 0u;
}

[[nodiscard]] bool valid_draw_logical_use(
    const NativePortDrawLogicalUse use) noexcept {
    return use == NativePortDrawLogicalUse::Unspecified ||
           use == NativePortDrawLogicalUse::SceneMaterial ||
           use == NativePortDrawLogicalUse::Interface ||
           use == NativePortDrawLogicalUse::Font ||
           use == NativePortDrawLogicalUse::Presentation;
}

[[nodiscard]] bool valid_draw_origin_kind(
    const NativePortDrawOriginKind origin) noexcept {
    return origin == NativePortDrawOriginKind::Unspecified ||
           origin == NativePortDrawOriginKind::Immediate ||
           origin == NativePortDrawOriginKind::ModelMesh ||
           origin == NativePortDrawOriginKind::Sprite ||
           origin == NativePortDrawOriginKind::Font ||
           origin == NativePortDrawOriginKind::Movie ||
           origin == NativePortDrawOriginKind::Presentation;
}

[[nodiscard]] bool valid_draw_intent(
    const NativePortDrawIntent intent) noexcept {
    return intent == NativePortDrawIntent::Unspecified ||
           intent == NativePortDrawIntent::SceneObject ||
           intent == NativePortDrawIntent::Shadow ||
           intent == NativePortDrawIntent::Interface ||
           intent == NativePortDrawIntent::Font ||
           intent == NativePortDrawIntent::Movie ||
           intent == NativePortDrawIntent::Presentation;
}

[[nodiscard]] bool valid_texture_resolver_kind(
    const NativePortTextureResolverKind resolver) noexcept {
    return resolver == NativePortTextureResolverKind::Unspecified ||
           resolver == NativePortTextureResolverKind::NativeDescriptor ||
           resolver == NativePortTextureResolverKind::DynamicSurface ||
           resolver == NativePortTextureResolverKind::CachedArchive ||
           resolver == NativePortTextureResolverKind::ExactArchiveNames ||
           resolver == NativePortTextureResolverKind::ExactArchiveLayouts ||
           resolver == NativePortTextureResolverKind::PartialArchive ||
           resolver == NativePortTextureResolverKind::Checkpoint ||
           resolver == NativePortTextureResolverKind::RegisteredTexture ||
           resolver == NativePortTextureResolverKind::IdentityBoundOverride;
}

[[nodiscard]] bool valid_texture_writer_kind(
    const NativePortTextureBindingWriterKind writer) noexcept {
    return writer == NativePortTextureBindingWriterKind::Unspecified ||
           writer == NativePortTextureBindingWriterKind::TextureListBind ||
           writer == NativePortTextureBindingWriterKind::TextureNumberSelect ||
           writer == NativePortTextureBindingWriterKind::RegisteredTextureSelect ||
           writer == NativePortTextureBindingWriterKind::IdentityBoundOverride;
}

[[nodiscard]] bool valid_draw_diagnostics(
    const NativePortDrawDiagnostics& diagnostics) noexcept {
    if (!diagnostics.enabled) return true;
    const auto& binding = diagnostics.texture_binding;
    if (!valid_draw_logical_use(diagnostics.logical_use) ||
        !valid_draw_origin_kind(diagnostics.origin) ||
        !valid_draw_intent(diagnostics.intent) ||
        !valid_texture_resolver_kind(binding.resolver) ||
        !valid_texture_writer_kind(binding.last_writer) ||
        diagnostics.material_identity_bound !=
            (diagnostics.material_identity != 0u) ||
        diagnostics.origin_identity_bound !=
            (diagnostics.origin_identity != 0u) ||
        diagnostics.model_identity_bound !=
            (diagnostics.model_identity != 0u) ||
        (!diagnostics.texture_list_index_bound &&
         diagnostics.texture_list_index != 0u) ||
        (!diagnostics.mesh_index_bound && diagnostics.mesh_index != 0u) ||
        (!diagnostics.primitive_index_bound &&
         diagnostics.primitive_index != 0u) ||
        binding.last_writer_bound !=
            (binding.last_writer_identity != 0u &&
             binding.last_writer_sequence != 0u &&
             binding.last_writer !=
                 NativePortTextureBindingWriterKind::Unspecified) ||
        binding.expected_asset_bound !=
            (binding.expected_asset_identity != 0u) ||
        binding.resolved_asset_bound !=
            (binding.resolved_asset_identity != 0u))
        return false;
    if (!binding.texture_list_bound &&
        (binding.texture_list_identity != 0u ||
         binding.texture_list_count != 0u))
        return false;
    if (binding.texture_list_bound &&
        (binding.texture_list_identity == 0u ||
         binding.texture_list_count == 0u))
        return false;
    if (binding.texture_list_epoch_bound !=
        (binding.texture_list_bound && binding.texture_list_epoch != 0u))
        return false;
    return !(binding.texture_list_bound && diagnostics.texture_list_index_bound &&
             diagnostics.texture_list_index >= binding.texture_list_count);
}

[[nodiscard]] bool valid_draw_state(
    const NativePortGraphicsDrawStateDescriptor& state) noexcept {
    if (!valid_viewport(state.viewport) || !valid_topology(state.topology) ||
        !valid_shading(state.rasterizer.shading) || state.batch.identity == 0u ||
        !valid_blend(state.blend) || !valid_depth(state.depth) ||
        !valid_depth_mapping(state.depth_mapping) ||
        !valid_texture_stage(state.texture_stage) ||
        !valid_sampler(state.sampler) || !valid_material(state.material) ||
        !valid_lighting(state.lighting) || !valid_fog(state.fog) ||
        !valid_color_clamp(state.color_clamp) ||
        !valid_alpha_test(state.alpha_test) ||
        !valid_draw_diagnostics(state.diagnostics) ||
        !finite_array(state.transform.values) ||
        !finite_array(state.normal_transform.values)) {
        return false;
    }
    if (state.texture_stage == NativePortTextureStage::Disabled
            ? static_cast<bool>(state.texture)
            : state.texture_stage != NativePortTextureStage::RequiredResolved ||
                  !static_cast<bool>(state.texture)) {
        return false;
    }
    switch (state.vertex_space) {
    case NativePortVertexSpace::ObjectHomogeneous:
    case NativePortVertexSpace::PvrScreenReciprocal:
    case NativePortVertexSpace::ClipHomogeneous:
        break;
    default:
        return false;
    }
    switch (state.draw_class) {
    case NativePortDrawClass::Opaque:
    case NativePortDrawClass::PunchThrough:
    case NativePortDrawClass::Translucent:
    case NativePortDrawClass::Overlay:
        break;
    default:
        return false;
    }
    switch (state.batch.semantic) {
    case NativePortDrawBatchClass::Scene3D:
    case NativePortDrawBatchClass::GameOverlay:
    case NativePortDrawBatchClass::UiOverlay:
    case NativePortDrawBatchClass::FontOverlay:
        break;
    default:
        return false;
    }
    switch (state.translucency) {
    case NativePortTranslucencyPolicy::NotApplicable:
    case NativePortTranslucencyPolicy::AuthoredUnsorted:
    case NativePortTranslucencyPolicy::StableDepthSorted:
        break;
    case NativePortTranslucencyPolicy::Type2AutoSorted:
        if (state.type2_autosort.contract_version !=
                native_port_type2_autosort_contract_version ||
            state.type2_autosort.presort != 0u)
            return false;
        break;
    default:
        return false;
    }
    if (state.interpolation != NativePortInterpolationMode::PerspectiveCorrect &&
        state.interpolation != NativePortInterpolationMode::PvrScreenGouraud)
        return false;
    switch (state.depth_mapping.mode) {
    case NativePortDepthCoordinateMode::ClipSpace:
    case NativePortDepthCoordinateMode::ReciprocalPositive:
    case NativePortDepthCoordinateMode::ReciprocalPositiveHomogeneousClip:
        break;
    default:
        return false;
    }
    if (state.rasterizer.cull != NativePortCullMode::None &&
        state.rasterizer.cull != NativePortCullMode::Front &&
        state.rasterizer.cull != NativePortCullMode::Back)
        return false;
    if (state.rasterizer.fill != NativePortFillMode::Solid &&
        state.rasterizer.fill != NativePortFillMode::Wireframe)
        return false;
    if (state.rasterizer.small_triangle_area_threshold < 0.0f ||
        !std::isfinite(state.rasterizer.small_triangle_area_threshold))
        return false;
    if (state.rasterizer.small_triangle_area_space !=
            NativePortTriangleAreaSpace::Submitted &&
        state.rasterizer.small_triangle_area_space !=
            NativePortTriangleAreaSpace::LogicalViewportAfterTransform)
        return false;
    if (state.rasterizer.small_triangle_area_space ==
            NativePortTriangleAreaSpace::LogicalViewportAfterTransform &&
        state.rasterizer.small_triangle_area_threshold != 0.0f &&
        !valid_extent(state.rasterizer.small_triangle_reference_extent))
        return false;
    if (!finite_array(state.material.diffuse) ||
        !finite_array(state.material.ambient) ||
        !finite_array(state.material.specular) ||
        !finite_array(state.material.emission) ||
        !finite_array(state.lighting.ambient) ||
        state.lighting.light_count > native_port_maximum_directional_lights ||
        !finite_array(state.fog.color) || !finite_array(state.color_clamp.minimum) ||
        !finite_array(state.color_clamp.maximum))
        return false;
    for (std::size_t index = 0u; index < state.lighting.light_count; ++index) {
        const auto& light = state.lighting.lights[index];
        if (!finite_array(light.direction) || !finite_array(light.color))
            return false;
    }
    return true;
}

struct ImageLayoutPlan final {
    std::array<NativePortGraphicsImageDescriptor,
                native_port_graphics_command_stream_max_mip_levels>
        descriptors{};
    std::uint32_t count = 0u;
    std::uint32_t descriptors_offset = 0u;
    std::uint32_t total_size = 0u;
};

[[nodiscard]] bool append_layout_segment(const std::size_t size,
                                         const std::size_t alignment,
                                         std::size_t& cursor,
                                         std::uint32_t& relative_offset) noexcept {
    std::size_t aligned = 0u;
    if (!checked_align_up(cursor, alignment, aligned) ||
        !checked_u32(aligned, relative_offset) ||
        !checked_add(aligned, size, cursor) || !checked_u32(cursor, relative_offset)) {
        // The final checked_u32 above is only an overflow check on cursor; it
        // is overwritten below with the segment start.
        return false;
    }
    relative_offset = static_cast<std::uint32_t>(aligned);
    return true;
}

[[nodiscard]] bool plan_images(
    const std::size_t root_size,
    const std::span<const NativePortImageView> images,
    const NativePortTextureConfig* texture_config,
    ImageLayoutPlan& plan) noexcept {
    if (images.size() > native_port_graphics_command_stream_max_mip_levels)
        return false;
    static_cast<void>(texture_config);
    std::size_t cursor = root_size;
    if (!images.empty()) {
        if (!append_layout_segment(
                images.size() * sizeof(NativePortGraphicsImageDescriptor),
                alignof(NativePortGraphicsImageDescriptor), cursor,
                plan.descriptors_offset))
            return false;
    }
    plan.count = static_cast<std::uint32_t>(images.size());
    for (std::size_t index = 0u; index < images.size(); ++index) {
        const auto& image = images[index];
        if (!valid_image(image)) return false;
        auto& descriptor = plan.descriptors[index];
        descriptor.extent_width = image.extent.width;
        descriptor.extent_height = image.extent.height;
        descriptor.format = static_cast<std::uint32_t>(image.format);
        descriptor.stride_bytes = image.stride_bytes;
        descriptor.bottom_up = image.bottom_up ? 1u : 0u;
        descriptor.display_aspect_numerator = image.display_aspect_numerator;
        descriptor.display_aspect_denominator = image.display_aspect_denominator;
        std::uint32_t ignored = 0u;
        if (!append_layout_segment(image.pixels.size(), 1u, cursor,
                                   descriptor.pixels_offset) ||
            !checked_u32(image.pixels.size(), ignored))
            return false;
        descriptor.pixels_size = ignored;
    }
    if (!checked_u32(cursor, plan.total_size)) return false;
    return true;
}

[[nodiscard]] NativePortGraphicsTextureConfigDescriptor encode_texture_config(
    const NativePortTextureConfig& config) noexcept {
    NativePortGraphicsTextureConfigDescriptor result{};
    result.extent = config.extent;
    result.format = config.format;
    result.mip_levels = config.mip_levels;
    result.shader_resource = config.shader_resource ? 1u : 0u;
    result.dynamic = config.dynamic ? 1u : 0u;
    result.provenance = config.provenance;
    return result;
}

[[nodiscard]] NativePortTextureConfig decode_texture_config(
    const NativePortGraphicsTextureConfigDescriptor& config) noexcept {
    NativePortTextureConfig result{};
    result.extent = config.extent;
    result.format = config.format;
    result.mip_levels = config.mip_levels;
    result.shader_resource = config.shader_resource != 0u;
    result.dynamic = config.dynamic != 0u;
    result.provenance = config.provenance;
    return result;
}

[[nodiscard]] bool valid_wire_image_descriptor(
    const NativePortGraphicsImageDescriptor& descriptor,
    const std::span<const std::byte> payload,
    const NativePortExtent* expected_extent = nullptr,
    const NativePortTextureFormat* expected_format = nullptr) noexcept {
    if (descriptor.bottom_up > 1u)
        return false;
    NativePortImageView image{};
    image.extent = {descriptor.extent_width, descriptor.extent_height};
    image.format = static_cast<NativePortTextureFormat>(descriptor.format);
    image.stride_bytes = descriptor.stride_bytes;
    image.bottom_up = descriptor.bottom_up != 0u;
    image.display_aspect_numerator = descriptor.display_aspect_numerator;
    image.display_aspect_denominator = descriptor.display_aspect_denominator;
    if (descriptor.pixels_offset > payload.size() ||
        descriptor.pixels_size > payload.size() - descriptor.pixels_offset ||
        descriptor.pixels_offset % alignof(std::byte) != 0u)
        return false;
    image.pixels = payload.subspan(descriptor.pixels_offset,
                                   descriptor.pixels_size);
    static_cast<void>(expected_extent);
    static_cast<void>(expected_format);
    return valid_image(image);
}

template <typename T>
[[nodiscard]] bool aligned_range(const std::span<const std::byte> bytes,
                                 const std::uint32_t offset,
                                 const std::size_t count,
                                 std::span<const T>& result) noexcept {
    if (offset % alignof(T) != 0u || offset > bytes.size()) return false;
    const auto available = bytes.size() - offset;
    if (count > available / sizeof(T)) return false;
    const auto* const address = bytes.data() + offset;
    if (reinterpret_cast<std::uintptr_t>(address) % alignof(T) != 0u)
        return false;
    result = {reinterpret_cast<const T*>(address), count};
    return true;
}

template <typename T>
[[nodiscard]] bool read_root(const std::span<const std::byte> bytes,
                             T& root) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes.size() < sizeof(T) ||
        reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(T) != 0u)
        return false;
    std::memcpy(&root, bytes.data(), sizeof(T));
    return root.header.contract_version ==
               native_port_graphics_command_stream_contract_version &&
           root.header.byte_size == bytes.size();
}

[[nodiscard]] NativePortGraphicsDrawStateDescriptor encode_draw_state(
    const NativePortDrawPacket& packet) noexcept {
    NativePortGraphicsDrawStateDescriptor result{};
    result.mesh = packet.mesh;
    result.vertex_space = packet.vertex_space;
    result.draw_class = packet.draw_class;
    result.batch = packet.batch;
    result.translucency = packet.translucency;
    result.type2_autosort = packet.type2_autosort;
    result.diagnostics = packet.diagnostics;
    result.interpolation = packet.interpolation;
    result.transform = packet.transform;
    result.normal_transform = packet.normal_transform;
    result.texture = packet.texture;
    result.texture_stage = packet.texture_stage;
    result.viewport = packet.viewport;
    result.topology = packet.topology;
    result.blend = packet.blend;
    result.depth = packet.depth;
    result.depth_mapping = packet.depth_mapping;
    result.rasterizer = packet.rasterizer;
    result.sampler = packet.sampler;
    result.material = packet.material;
    result.lighting = packet.lighting;
    result.fog = packet.fog;
    result.color_clamp = packet.color_clamp;
    result.alpha_test = packet.alpha_test;
    return result;
}

[[nodiscard]] NativePortDrawPacket decode_draw_packet(
    const NativePortGraphicsDrawPayload& payload,
    const std::span<const NativePortVertex> vertices,
    const std::span<const std::uint32_t> indices) noexcept {
    NativePortDrawPacket result{};
    result.vertices = vertices;
    result.indices = indices;
    result.mesh = payload.state.mesh;
    result.vertex_space = payload.state.vertex_space;
    result.draw_class = payload.state.draw_class;
    result.batch = payload.state.batch;
    result.translucency = payload.state.translucency;
    result.type2_autosort = payload.state.type2_autosort;
    result.diagnostics = payload.state.diagnostics;
    result.interpolation = payload.state.interpolation;
    result.transform = payload.state.transform;
    result.normal_transform = payload.state.normal_transform;
    result.texture = payload.state.texture;
    result.texture_stage = payload.state.texture_stage;
    result.viewport = payload.state.viewport;
    result.topology = payload.state.topology;
    result.blend = payload.state.blend;
    result.depth = payload.state.depth;
    result.depth_mapping = payload.state.depth_mapping;
    result.rasterizer = payload.state.rasterizer;
    result.sampler = payload.state.sampler;
    result.material = payload.state.material;
    result.lighting = payload.state.lighting;
    result.fog = payload.state.fog;
    result.color_clamp = payload.state.color_clamp;
    result.alpha_test = payload.state.alpha_test;
    return result;
}

[[nodiscard]] bool valid_frame_config(
    const NativePortFrameConfig& config) noexcept {
    return finite_array(config.clear_color) &&
           std::isfinite(config.clear_depth) && config.clear_depth >= 0.0f &&
           config.clear_depth <= 1.0f &&
           (config.depth_buffer == NativePortDepthBufferConvention::Forward ||
            config.depth_buffer ==
                NativePortDepthBufferConvention::ReciprocalPositive);
}

[[nodiscard]] bool valid_geometry(
    const std::span<const NativePortVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const NativePortPrimitiveTopology topology,
    const bool allow_empty) noexcept {
    if (!valid_topology(topology) || vertices.size() >
            native_port_graphics_command_stream_max_vertices ||
        indices.size() > native_port_graphics_command_stream_max_indices ||
        vertices.size_bytes() > max_u32 || indices.size_bytes() > max_u32 ||
        (!allow_empty && vertices.empty()))
        return false;
    for (const auto index : indices)
        if (index >= vertices.size()) return false;
    for (const auto& vertex : vertices) {
        if (!finite_array(vertex.position) ||
            !finite_array(vertex.texture_coordinate) ||
            !finite_array(vertex.color) || !finite_array(vertex.normal) ||
            !finite_array(vertex.secondary_color) ||
            !std::isfinite(vertex.fog_coordinate) ||
            !std::isfinite(vertex.depth_coordinate) ||
            !std::isfinite(vertex.position_w))
            return false;
    }
    const auto element_count =
        indices.empty() ? vertices.size() : indices.size();
    if (element_count == 0u && allow_empty) return true;
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
}

[[nodiscard]] bool validate_wire_command(
    const NativePortFrameCommand& command,
    const std::span<const std::byte> frame_payload) noexcept {
    if (command.flags != 0u) return false;
    if (command.payload_offset > frame_payload.size() ||
        command.payload_size > frame_payload.size() - command.payload_offset)
        return false;
    const auto bytes = frame_payload.subspan(command.payload_offset,
                                             command.payload_size);
    const auto kind = static_cast<NativePortGraphicsCommandKind>(command.kind);
    switch (kind) {
    case NativePortGraphicsCommandKind::Show:
    case NativePortGraphicsCommandKind::PollEvents:
    case NativePortGraphicsCommandKind::FlushType2:
    case NativePortGraphicsCommandKind::Present:
    case NativePortGraphicsCommandKind::RepeatPresent:
    case NativePortGraphicsCommandKind::Shutdown:
        return bytes.empty();
    case NativePortGraphicsCommandKind::CreateTexture: {
        NativePortGraphicsCreateTexturePayload root{};
        if (!read_root(bytes, root) || root.config.shader_resource > 1u ||
            root.config.dynamic > 1u)
            return false;
        if (root.mip_count > native_port_graphics_command_stream_max_mip_levels ||
            (!root.mip_count && root.mip_descriptors_offset != 0u))
            return false;
        std::span<const NativePortGraphicsImageDescriptor> descriptors;
        if (root.mip_count != 0u &&
            !aligned_range(bytes, root.mip_descriptors_offset, root.mip_count,
                           descriptors))
            return false;
        for (const auto& descriptor : descriptors)
            if (!valid_wire_image_descriptor(descriptor, bytes)) return false;
        return true;
    }
    case NativePortGraphicsCommandKind::UpdateTexture: {
        NativePortGraphicsUpdateTexturePayload root{};
        if (!read_root(bytes, root) ||
            root.mip_count > native_port_graphics_command_stream_max_mip_levels)
            return false;
        std::span<const NativePortGraphicsImageDescriptor> descriptors;
        if (root.mip_count == 0u ? root.mip_descriptors_offset != 0u
                                 : !aligned_range(bytes,
                                                  root.mip_descriptors_offset,
                                                  root.mip_count,
                                                  descriptors))
            return false;
        for (const auto& descriptor : descriptors)
            if (!valid_wire_image_descriptor(descriptor, bytes)) return false;
        return true;
    }
    case NativePortGraphicsCommandKind::DestroyTexture: {
        NativePortGraphicsHandlePayload root{};
        return read_root(bytes, root);
    }
    case NativePortGraphicsCommandKind::CreateMesh: {
        NativePortGraphicsCreateMeshPayload root{};
        if (!read_root(bytes, root) ||
            root.mesh.vertices_count >
                native_port_graphics_command_stream_max_vertices ||
            root.mesh.indices_count >
                native_port_graphics_command_stream_max_indices)
            return false;
        std::span<const NativePortVertex> vertices;
        std::span<const std::uint32_t> indices;
        if (!aligned_range(bytes, root.mesh.vertices_offset,
                           root.mesh.vertices_count, vertices) ||
            !aligned_range(bytes, root.mesh.indices_offset,
                           root.mesh.indices_count, indices))
            return false;
        return true;
    }
    case NativePortGraphicsCommandKind::DestroyMesh: {
        NativePortGraphicsHandlePayload root{};
        return read_root(bytes, root);
    }
    case NativePortGraphicsCommandKind::BeginFrame: {
        NativePortGraphicsBeginFramePayload root{};
        return read_root(bytes, root);
    }
    case NativePortGraphicsCommandKind::Draw: {
        NativePortGraphicsDrawPayload root{};
        if (!read_root(bytes, root) ||
            root.vertices_count > native_port_graphics_command_stream_max_vertices ||
            root.indices_count > native_port_graphics_command_stream_max_indices)
            return false;
        std::span<const NativePortVertex> vertices;
        std::span<const std::uint32_t> indices;
        if (root.vertices_count != 0u &&
            !aligned_range(bytes, root.vertices_offset, root.vertices_count,
                           vertices))
            return false;
        if (root.indices_count != 0u &&
            !aligned_range(bytes, root.indices_offset, root.indices_count,
                           indices))
            return false;
        return true;
    }
    case NativePortGraphicsCommandKind::PresentImage: {
        NativePortGraphicsPresentImagePayload root{};
        if (!read_root(bytes, root) ||
            !valid_wire_image_descriptor(root.image, bytes))
            return false;
        return true;
    }
    }
    return false;
}

[[nodiscard]] bool append_part(NativePortFrameWriteLease& lease,
                               const std::span<const std::byte> bytes,
                               const std::uint32_t alignment,
                               const std::uint32_t root_offset,
                               const std::uint32_t relative_offset) noexcept {
    const auto actual = lease.append_payload(bytes, alignment);
    if (!actual.has_value()) return false;
    std::uint32_t expected = 0u;
    if (!checked_u32(static_cast<std::size_t>(root_offset) + relative_offset,
                     expected))
        return false;
    return *actual == expected;
}

template <typename T>
[[nodiscard]] std::span<const std::byte> pod_bytes(const T& value) noexcept {
    return std::as_bytes(std::span<const T>(&value, 1u));
}

template <typename T>
[[nodiscard]] std::span<const std::byte> pod_array_bytes(
    const T* values, const std::size_t count) noexcept {
    return count == 0u ? std::span<const std::byte>{}
                       : std::as_bytes(std::span<const T>(values, count));
}

template <typename T>
[[nodiscard]] bool checked_count_bytes(const std::size_t count,
                                       const std::size_t maximum_count,
                                       std::uint32_t& bytes) noexcept {
    if (count > maximum_count || count > max_u32 / sizeof(T)) return false;
    bytes = static_cast<std::uint32_t>(count * sizeof(T));
    return true;
}

[[nodiscard]] bool valid_writer_source_draw(
    const NativePortDrawPacket& packet) noexcept {
    if (packet.vertices.size() > native_port_graphics_command_stream_max_vertices ||
        packet.indices.size() > native_port_graphics_command_stream_max_indices ||
        packet.vertices.size_bytes() > max_u32 || packet.indices.size_bytes() > max_u32)
        return false;
    return true;
}

} // namespace

std::optional<std::uint32_t>
NativePortGraphicsCommandPayloadRequirement::capacity_from(
    const std::uint32_t current_payload_size) const noexcept {
    std::size_t aligned = 0u;
    std::size_t total = 0u;
    std::uint32_t capacity = 0u;
    if (!checked_align_up(current_payload_size, alignment, aligned) ||
        !checked_add(aligned, static_cast<std::size_t>(bytes), total) ||
        !checked_u32(total, capacity))
        return std::nullopt;
    return capacity;
}

std::optional<NativePortGraphicsCommandPayloadRequirement>
native_port_graphics_encoded_create_texture_size(
    const NativePortTextureConfig& config,
    const std::span<const NativePortImageView> initial_mip_levels) noexcept {
    ImageLayoutPlan plan;
    if (!plan_images(sizeof(NativePortGraphicsCreateTexturePayload),
                     initial_mip_levels, &config, plan))
        return std::nullopt;
    return NativePortGraphicsCommandPayloadRequirement{
        plan.total_size,
        static_cast<std::uint32_t>(alignof(NativePortGraphicsCreateTexturePayload))};
}

std::optional<NativePortGraphicsCommandPayloadRequirement>
native_port_graphics_encoded_update_texture_size(
    const std::span<const NativePortImageView> mip_levels) noexcept {
    ImageLayoutPlan plan;
    if (!plan_images(sizeof(NativePortGraphicsUpdateTexturePayload), mip_levels,
                     nullptr, plan))
        return std::nullopt;
    return NativePortGraphicsCommandPayloadRequirement{
        plan.total_size,
        static_cast<std::uint32_t>(alignof(NativePortGraphicsUpdateTexturePayload))};
}

std::optional<NativePortGraphicsCommandPayloadRequirement>
native_port_graphics_encoded_create_mesh_size(
    const NativePortMeshConfig& config) noexcept {
    std::uint32_t vertex_bytes = 0u;
    std::uint32_t index_bytes = 0u;
    if (!checked_count_bytes<NativePortVertex>(
            config.vertices.size(), native_port_graphics_command_stream_max_vertices,
            vertex_bytes) ||
        !checked_count_bytes<std::uint32_t>(
            config.indices.size(), native_port_graphics_command_stream_max_indices,
            index_bytes))
        return std::nullopt;
    std::size_t cursor = sizeof(NativePortGraphicsCreateMeshPayload);
    std::uint32_t ignored = 0u;
    if (!append_layout_segment(vertex_bytes, alignof(NativePortVertex), cursor,
                               ignored) ||
        !append_layout_segment(index_bytes, alignof(std::uint32_t), cursor,
                               ignored))
        return std::nullopt;
    std::uint32_t total = 0u;
    if (!checked_u32(cursor, total)) return std::nullopt;
    return NativePortGraphicsCommandPayloadRequirement{
        total, static_cast<std::uint32_t>(alignof(NativePortGraphicsCreateMeshPayload))};
}

std::optional<NativePortGraphicsCommandPayloadRequirement>
native_port_graphics_encoded_draw_size(
    const NativePortDrawPacket& packet) noexcept {
    if (!valid_writer_source_draw(packet)) return std::nullopt;
    std::uint32_t vertex_bytes = 0u;
    std::uint32_t index_bytes = 0u;
    if (!checked_count_bytes<NativePortVertex>(
            packet.vertices.size(), native_port_graphics_command_stream_max_vertices,
            vertex_bytes) ||
        !checked_count_bytes<std::uint32_t>(
            packet.indices.size(), native_port_graphics_command_stream_max_indices,
            index_bytes))
        return std::nullopt;
    std::size_t cursor = sizeof(NativePortGraphicsDrawPayload);
    std::uint32_t ignored = 0u;
    if (!append_layout_segment(vertex_bytes, alignof(NativePortVertex), cursor,
                               ignored) ||
        !append_layout_segment(index_bytes, alignof(std::uint32_t), cursor,
                               ignored))
        return std::nullopt;
    std::uint32_t total = 0u;
    if (!checked_u32(cursor, total)) return std::nullopt;
    return NativePortGraphicsCommandPayloadRequirement{
        total, static_cast<std::uint32_t>(alignof(NativePortGraphicsDrawPayload))};
}

std::optional<NativePortGraphicsCommandPayloadRequirement>
native_port_graphics_encoded_present_image_size(
    const NativePortImageView& image) noexcept {
    if (!valid_image(image)) return std::nullopt;
    std::size_t cursor = sizeof(NativePortGraphicsPresentImagePayload);
    std::uint32_t ignored = 0u;
    if (!append_layout_segment(image.pixels.size(), 1u, cursor, ignored))
        return std::nullopt;
    std::uint32_t total = 0u;
    if (!checked_u32(cursor, total)) return std::nullopt;
    return NativePortGraphicsCommandPayloadRequirement{
        total, static_cast<std::uint32_t>(alignof(NativePortGraphicsPresentImagePayload))};
}

NativePortGraphicsCommandWriter::NativePortGraphicsCommandWriter(
    NativePortFrameWriteLease& lease) noexcept
    : lease_(&lease) {
    if (!lease.valid()) {
        failed_ = true;
        error_ = NativePortGraphicsCommandEncodeError::InvalidWriter;
    }
}

bool NativePortGraphicsCommandWriter::valid() const noexcept {
    return !failed_ && lease_ != nullptr && lease_->valid();
}

bool NativePortGraphicsCommandWriter::failed() const noexcept {
    return failed_;
}

NativePortGraphicsCommandEncodeError
NativePortGraphicsCommandWriter::error() const noexcept {
    return error_;
}

std::uint32_t NativePortGraphicsCommandWriter::encoded_payload_bytes() const noexcept {
    return encoded_payload_bytes_;
}

bool NativePortGraphicsCommandWriter::reject(
    const NativePortGraphicsCommandEncodeError error) noexcept {
    if (!failed_) {
        failed_ = true;
        error_ = error;
        if (lease_ != nullptr && lease_->valid()) lease_->abort();
    }
    return false;
}

bool NativePortGraphicsCommandWriter::append_empty(
    const NativePortGraphicsCommandKind kind) noexcept {
    if (!valid()) return false;
    const auto offset = lease_->append_payload({}, 1u);
    if (!offset.has_value() ||
        !lease_->append_command_reference(static_cast<std::uint32_t>(kind),
                                          *offset, 0u))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    return true;
}

bool NativePortGraphicsCommandWriter::show() noexcept {
    return append_empty(NativePortGraphicsCommandKind::Show);
}

bool NativePortGraphicsCommandWriter::poll_events() noexcept {
    return append_empty(NativePortGraphicsCommandKind::PollEvents);
}

bool NativePortGraphicsCommandWriter::create_texture(
    const NativePortTextureHandle texture,
    const NativePortTextureConfig& config,
    const std::span<const NativePortImageView> initial_mip_levels) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    ImageLayoutPlan plan;
    if (!plan_images(sizeof(NativePortGraphicsCreateTexturePayload),
                     initial_mip_levels, &config, plan))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsCreateTexturePayload root{};
    root.header.byte_size = plan.total_size;
    root.texture = texture;
    root.config = encode_texture_config(config);
    root.mip_descriptors_offset = plan.descriptors_offset;
    root.mip_count = plan.count;
    const auto root_alignment = static_cast<std::uint32_t>(std::max(
        alignof(NativePortGraphicsCreateTexturePayload),
        alignof(NativePortGraphicsImageDescriptor)));
    const auto root_offset = lease_->append_payload(pod_bytes(root), root_alignment);
    if (!root_offset.has_value())
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (plan.count != 0u &&
        !append_part(*lease_, pod_array_bytes(plan.descriptors.data(), plan.count),
                     static_cast<std::uint32_t>(alignof(
                         NativePortGraphicsImageDescriptor)),
                     *root_offset, plan.descriptors_offset))
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    for (std::size_t index = 0u; index < plan.count; ++index) {
        if (!append_part(*lease_,
                         std::as_bytes(std::span<const std::byte>(
                             reinterpret_cast<const std::byte*>(
                                 initial_mip_levels[index].pixels.data()),
                             initial_mip_levels[index].pixels.size())),
                         1u, *root_offset,
                         plan.descriptors[index].pixels_offset))
            return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    }
    if (!lease_->append_command_reference(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::CreateTexture),
            *root_offset, plan.total_size))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - plan.total_size
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + plan.total_size;
    return true;
}

bool NativePortGraphicsCommandWriter::create_texture(
    const NativePortTextureHandle texture,
    const NativePortTextureConfig& config,
    const NativePortImageView* const initial_pixels) noexcept {
    if (initial_pixels == nullptr)
        return create_texture(texture, config, std::span<const NativePortImageView>{});
    return create_texture(texture, config,
                          std::span<const NativePortImageView>(initial_pixels, 1u));
}

bool NativePortGraphicsCommandWriter::update_texture(
    const NativePortTextureHandle texture,
    const std::span<const NativePortImageView> mip_levels) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    ImageLayoutPlan plan;
    if (!plan_images(sizeof(NativePortGraphicsUpdateTexturePayload), mip_levels,
                     nullptr, plan))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsUpdateTexturePayload root{};
    root.header.byte_size = plan.total_size;
    root.texture = texture;
    root.mip_descriptors_offset = plan.descriptors_offset;
    root.mip_count = plan.count;
    const auto root_alignment = static_cast<std::uint32_t>(std::max(
        alignof(NativePortGraphicsUpdateTexturePayload),
        alignof(NativePortGraphicsImageDescriptor)));
    const auto root_offset = lease_->append_payload(pod_bytes(root), root_alignment);
    if (!root_offset.has_value())
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (plan.count != 0u &&
        !append_part(*lease_, pod_array_bytes(plan.descriptors.data(), plan.count),
                     static_cast<std::uint32_t>(alignof(
                         NativePortGraphicsImageDescriptor)),
                     *root_offset, plan.descriptors_offset))
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    for (std::size_t index = 0u; index < plan.count; ++index) {
        if (!append_part(*lease_,
                         std::span<const std::byte>(
                             reinterpret_cast<const std::byte*>(
                                 mip_levels[index].pixels.data()),
                             mip_levels[index].pixels.size()),
                         1u, *root_offset,
                         plan.descriptors[index].pixels_offset))
            return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    }
    if (!lease_->append_command_reference(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::UpdateTexture),
            *root_offset, plan.total_size))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - plan.total_size
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + plan.total_size;
    return true;
}

bool NativePortGraphicsCommandWriter::update_texture(
    const NativePortTextureHandle texture,
    const NativePortImageView& pixels) noexcept {
    return update_texture(texture, std::span<const NativePortImageView>(&pixels, 1u));
}

bool NativePortGraphicsCommandWriter::destroy_texture(
    const NativePortTextureHandle texture) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsHandlePayload root{};
    root.header.byte_size = sizeof(root);
    root.handle = texture.value;
    if (!lease_->append_command(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::DestroyTexture),
            pod_bytes(root), static_cast<std::uint32_t>(alignof(decltype(root)))))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - sizeof(root)
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + static_cast<std::uint32_t>(sizeof(root));
    return true;
}

bool NativePortGraphicsCommandWriter::create_mesh(
    const NativePortMeshHandle mesh,
    const NativePortMeshConfig& config) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::uint32_t vertex_bytes = 0u;
    std::uint32_t index_bytes = 0u;
    if (!checked_count_bytes<NativePortVertex>(
            config.vertices.size(), native_port_graphics_command_stream_max_vertices,
            vertex_bytes) ||
        !checked_count_bytes<std::uint32_t>(
            config.indices.size(), native_port_graphics_command_stream_max_indices,
            index_bytes))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::size_t cursor = sizeof(NativePortGraphicsCreateMeshPayload);
    std::uint32_t vertices_offset = 0u;
    std::uint32_t indices_offset = 0u;
    if (!append_layout_segment(vertex_bytes, alignof(NativePortVertex), cursor,
                               vertices_offset) ||
        !append_layout_segment(index_bytes, alignof(std::uint32_t), cursor,
                               indices_offset))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::uint32_t total_size = 0u;
    if (!checked_u32(cursor, total_size))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsCreateMeshPayload root{};
    root.header.byte_size = total_size;
    root.mesh.mesh = mesh;
    root.mesh.topology = static_cast<std::uint32_t>(config.topology);
    root.mesh.shading = static_cast<std::uint32_t>(config.shading);
    root.mesh.small_triangle_area_threshold = config.small_triangle_area_threshold;
    root.mesh.vertices_offset = vertices_offset;
    root.mesh.vertices_count = static_cast<std::uint32_t>(config.vertices.size());
    root.mesh.indices_offset = indices_offset;
    root.mesh.indices_count = static_cast<std::uint32_t>(config.indices.size());
    const auto root_alignment = static_cast<std::uint32_t>(std::max(
        alignof(NativePortGraphicsCreateMeshPayload), alignof(NativePortVertex)));
    const auto root_offset = lease_->append_payload(pod_bytes(root), root_alignment);
    if (!root_offset.has_value())
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (!append_part(*lease_, std::as_bytes(config.vertices),
                     static_cast<std::uint32_t>(alignof(NativePortVertex)),
                     *root_offset, vertices_offset) ||
        !append_part(*lease_, std::as_bytes(config.indices),
                     static_cast<std::uint32_t>(alignof(std::uint32_t)),
                     *root_offset, indices_offset))
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (!lease_->append_command_reference(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::CreateMesh),
            *root_offset, total_size))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - total_size
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + total_size;
    return true;
}

bool NativePortGraphicsCommandWriter::destroy_mesh(
    const NativePortMeshHandle mesh) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsHandlePayload root{};
    root.header.byte_size = sizeof(root);
    root.handle = mesh.value;
    if (!lease_->append_command(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::DestroyMesh),
            pod_bytes(root), static_cast<std::uint32_t>(alignof(decltype(root)))))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - sizeof(root)
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + static_cast<std::uint32_t>(sizeof(root));
    return true;
}

bool NativePortGraphicsCommandWriter::begin_frame(
    const NativePortFrameConfig& config) noexcept {
    if (!valid())
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsBeginFramePayload root{};
    root.header.byte_size = sizeof(root);
    root.config = config;
    if (!lease_->append_command(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::BeginFrame),
            pod_bytes(root), static_cast<std::uint32_t>(alignof(decltype(root)))))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - sizeof(root)
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + static_cast<std::uint32_t>(sizeof(root));
    return true;
}

bool NativePortGraphicsCommandWriter::draw(
    const NativePortDrawPacket& packet) noexcept {
    if (!valid() || !valid_writer_source_draw(packet))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::uint32_t vertex_bytes = 0u;
    std::uint32_t index_bytes = 0u;
    if (!checked_count_bytes<NativePortVertex>(
            packet.vertices.size(), native_port_graphics_command_stream_max_vertices,
            vertex_bytes) ||
        !checked_count_bytes<std::uint32_t>(
            packet.indices.size(), native_port_graphics_command_stream_max_indices,
            index_bytes))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::size_t cursor = sizeof(NativePortGraphicsDrawPayload);
    std::uint32_t vertices_offset = 0u;
    std::uint32_t indices_offset = 0u;
    if (!append_layout_segment(vertex_bytes, alignof(NativePortVertex), cursor,
                               vertices_offset) ||
        !append_layout_segment(index_bytes, alignof(std::uint32_t), cursor,
                               indices_offset))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::uint32_t total_size = 0u;
    if (!checked_u32(cursor, total_size))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsDrawPayload root{};
    root.header.byte_size = total_size;
    root.vertices_offset = vertices_offset;
    root.vertices_count = static_cast<std::uint32_t>(packet.vertices.size());
    root.indices_offset = indices_offset;
    root.indices_count = static_cast<std::uint32_t>(packet.indices.size());
    root.state = encode_draw_state(packet);
    const auto root_alignment = static_cast<std::uint32_t>(std::max(
        alignof(NativePortGraphicsDrawPayload), alignof(NativePortVertex)));
    const auto root_offset = lease_->append_payload(pod_bytes(root), root_alignment);
    if (!root_offset.has_value())
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if ((vertex_bytes != 0u &&
         !append_part(*lease_, std::as_bytes(packet.vertices),
                      static_cast<std::uint32_t>(alignof(NativePortVertex)),
                      *root_offset, vertices_offset)) ||
        (index_bytes != 0u &&
         !append_part(*lease_, std::as_bytes(packet.indices),
                      static_cast<std::uint32_t>(alignof(std::uint32_t)),
                      *root_offset, indices_offset)))
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (!lease_->append_command_reference(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::Draw),
            *root_offset, total_size))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - total_size
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + total_size;
    return true;
}

bool NativePortGraphicsCommandWriter::flush_type2_translucency() noexcept {
    return append_empty(NativePortGraphicsCommandKind::FlushType2);
}

bool NativePortGraphicsCommandWriter::present() noexcept {
    return append_empty(NativePortGraphicsCommandKind::Present);
}

bool NativePortGraphicsCommandWriter::repeat_present() noexcept {
    return append_empty(NativePortGraphicsCommandKind::RepeatPresent);
}

bool NativePortGraphicsCommandWriter::present_image(
    const NativePortImageView& image,
    const NativePortViewportTarget viewport,
    const NativePortImageFit fit) noexcept {
    if (!valid() || !valid_image(image))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::size_t cursor = sizeof(NativePortGraphicsPresentImagePayload);
    std::uint32_t pixels_offset = 0u;
    if (!append_layout_segment(image.pixels.size(), 1u, cursor, pixels_offset))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    std::uint32_t total_size = 0u;
    if (!checked_u32(cursor, total_size))
        return reject(NativePortGraphicsCommandEncodeError::InvalidInput);
    NativePortGraphicsPresentImagePayload root{};
    root.header.byte_size = total_size;
    root.image.extent_width = image.extent.width;
    root.image.extent_height = image.extent.height;
    root.image.format = static_cast<std::uint32_t>(image.format);
    root.image.stride_bytes = image.stride_bytes;
    root.image.bottom_up = image.bottom_up ? 1u : 0u;
    root.image.display_aspect_numerator = image.display_aspect_numerator;
    root.image.display_aspect_denominator = image.display_aspect_denominator;
    root.image.pixels_offset = pixels_offset;
    root.image.pixels_size = static_cast<std::uint32_t>(image.pixels.size());
    root.viewport = static_cast<std::uint32_t>(viewport);
    root.fit = static_cast<std::uint32_t>(fit);
    const auto root_offset = lease_->append_payload(
        pod_bytes(root), static_cast<std::uint32_t>(alignof(decltype(root))));
    if (!root_offset.has_value())
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (!append_part(*lease_,
                     std::span<const std::byte>(
                         reinterpret_cast<const std::byte*>(image.pixels.data()),
                         image.pixels.size()),
                     1u, *root_offset, pixels_offset))
        return reject(NativePortGraphicsCommandEncodeError::CapacityExceeded);
    if (!lease_->append_command_reference(
            static_cast<std::uint32_t>(NativePortGraphicsCommandKind::PresentImage),
            *root_offset, total_size))
        return reject(NativePortGraphicsCommandEncodeError::QueueRejected);
    encoded_payload_bytes_ = encoded_payload_bytes_ >
            std::numeric_limits<std::uint32_t>::max() - total_size
        ? std::numeric_limits<std::uint32_t>::max()
        : encoded_payload_bytes_ + total_size;
    return true;
}

bool NativePortGraphicsCommandWriter::shutdown() noexcept {
    return append_empty(NativePortGraphicsCommandKind::Shutdown);
}

bool NativePortGraphicsCommandWriter::publish() noexcept {
    if (!valid()) return false;
    if (lease_->publish()) return true;
    failed_ = true;
    error_ = NativePortGraphicsCommandEncodeError::QueueRejected;
    return false;
}

void NativePortGraphicsCommandWriter::abort() noexcept {
    if (lease_ != nullptr && lease_->valid()) lease_->abort();
    failed_ = true;
    if (error_ == NativePortGraphicsCommandEncodeError::None)
        error_ = NativePortGraphicsCommandEncodeError::QueueRejected;
}

std::optional<NativePortImageView> NativePortGraphicsImageViewRange::at(
    const std::size_t index) const noexcept {
    if (index >= descriptors.size()) return std::nullopt;
    const auto& descriptor = descriptors[index];
    if (!valid_wire_image_descriptor(descriptor, payload)) return std::nullopt;
    NativePortImageView result{};
    result.extent = {descriptor.extent_width, descriptor.extent_height};
    result.format = static_cast<NativePortTextureFormat>(descriptor.format);
    result.stride_bytes = descriptor.stride_bytes;
    result.bottom_up = descriptor.bottom_up != 0u;
    result.display_aspect_numerator = descriptor.display_aspect_numerator;
    result.display_aspect_denominator = descriptor.display_aspect_denominator;
    result.pixels = payload.subspan(descriptor.pixels_offset,
                                    descriptor.pixels_size);
    return result;
}

NativePortGraphicsCommandReader::NativePortGraphicsCommandReader(
    const NativePortFrameReadLease& lease) noexcept
    : lease_(&lease), commands_(lease.commands()), payload_(lease.payload()) {
    valid_ = lease.valid() && validate_all();
    if (!valid_) {
        commands_ = {};
        payload_ = {};
    }
}

bool NativePortGraphicsCommandReader::valid() const noexcept {
    return valid_;
}

std::size_t NativePortGraphicsCommandReader::size() const noexcept {
    return valid_ ? commands_.size() : 0u;
}

std::size_t NativePortGraphicsCommandReader::position() const noexcept {
    return valid_ ? position_ : 0u;
}

void NativePortGraphicsCommandReader::reset() noexcept {
    position_ = 0u;
}

std::optional<NativePortGraphicsCommandView>
NativePortGraphicsCommandReader::next() noexcept {
    if (!valid_ || position_ >= commands_.size()) return std::nullopt;
    const auto result = decode(position_);
    if (!result.has_value()) {
        valid_ = false;
        commands_ = {};
        payload_ = {};
        position_ = 0u;
        return std::nullopt;
    }
    ++position_;
    return result;
}

std::optional<NativePortGraphicsCommandView>
NativePortGraphicsCommandReader::at(const std::size_t ordinal) const noexcept {
    if (!valid_ || ordinal >= commands_.size()) return std::nullopt;
    return decode(ordinal);
}

bool NativePortGraphicsCommandReader::validate_all() noexcept {
    for (const auto& command : commands_)
        if (!validate_wire_command(command, payload_)) return false;
    return true;
}

std::optional<NativePortGraphicsCommandView>
NativePortGraphicsCommandReader::decode(const std::size_t ordinal) const noexcept {
    if (!valid_ && lease_ == nullptr) return std::nullopt;
    if (ordinal >= commands_.size()) return std::nullopt;
    const auto& command = commands_[ordinal];
    if (!validate_wire_command(command, payload_)) return std::nullopt;
    const auto bytes = payload_.subspan(command.payload_offset,
                                        command.payload_size);
    const auto kind = static_cast<NativePortGraphicsCommandKind>(command.kind);
    NativePortGraphicsCommandView result{};
    result.kind = kind;
    result.ordinal = static_cast<std::uint32_t>(ordinal);
    switch (kind) {
    case NativePortGraphicsCommandKind::Show:
        result.payload = NativePortGraphicsShowView{};
        break;
    case NativePortGraphicsCommandKind::PollEvents:
        result.payload = NativePortGraphicsPollEventsView{};
        break;
    case NativePortGraphicsCommandKind::CreateTexture: {
        NativePortGraphicsCreateTexturePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        std::span<const NativePortGraphicsImageDescriptor> descriptors;
        if (root.mip_count != 0u &&
            !aligned_range(bytes, root.mip_descriptors_offset, root.mip_count,
                           descriptors))
            return std::nullopt;
        NativePortGraphicsCreateTextureView view{};
        view.texture = root.texture;
        view.config = decode_texture_config(root.config);
        view.initial_mip_levels = {descriptors, bytes};
        result.payload = view;
        break;
    }
    case NativePortGraphicsCommandKind::UpdateTexture: {
        NativePortGraphicsUpdateTexturePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        std::span<const NativePortGraphicsImageDescriptor> descriptors;
        if (!aligned_range(bytes, root.mip_descriptors_offset, root.mip_count,
                           descriptors))
            return std::nullopt;
        NativePortGraphicsUpdateTextureView view{};
        view.texture = root.texture;
        view.mip_levels = {descriptors, bytes};
        result.payload = view;
        break;
    }
    case NativePortGraphicsCommandKind::DestroyTexture: {
        NativePortGraphicsHandlePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        result.payload = NativePortGraphicsDestroyTextureView{
            NativePortTextureHandle{root.handle}};
        break;
    }
    case NativePortGraphicsCommandKind::CreateMesh: {
        NativePortGraphicsCreateMeshPayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        std::span<const NativePortVertex> vertices;
        std::span<const std::uint32_t> indices;
        if (!aligned_range(bytes, root.mesh.vertices_offset,
                           root.mesh.vertices_count, vertices) ||
            !aligned_range(bytes, root.mesh.indices_offset,
                           root.mesh.indices_count, indices))
            return std::nullopt;
        NativePortGraphicsCreateMeshView view{};
        view.mesh = root.mesh.mesh;
        view.vertices = vertices;
        view.indices = indices;
        view.topology = static_cast<NativePortPrimitiveTopology>(
            root.mesh.topology);
        view.shading = static_cast<NativePortShadingMode>(root.mesh.shading);
        view.small_triangle_area_threshold = root.mesh.small_triangle_area_threshold;
        result.payload = view;
        break;
    }
    case NativePortGraphicsCommandKind::DestroyMesh: {
        NativePortGraphicsHandlePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        result.payload = NativePortGraphicsDestroyMeshView{
            NativePortMeshHandle{root.handle}};
        break;
    }
    case NativePortGraphicsCommandKind::BeginFrame: {
        NativePortGraphicsBeginFramePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        result.payload = NativePortGraphicsBeginFrameView{root.config};
        break;
    }
    case NativePortGraphicsCommandKind::Draw: {
        NativePortGraphicsDrawPayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        std::span<const NativePortVertex> vertices;
        std::span<const std::uint32_t> indices;
        if (root.vertices_count != 0u &&
            !aligned_range(bytes, root.vertices_offset, root.vertices_count,
                           vertices))
            return std::nullopt;
        if (root.indices_count != 0u &&
            !aligned_range(bytes, root.indices_offset, root.indices_count,
                           indices))
            return std::nullopt;
        result.payload = NativePortGraphicsDrawView{
            decode_draw_packet(root, vertices, indices)};
        break;
    }
    case NativePortGraphicsCommandKind::FlushType2:
        result.payload = NativePortGraphicsFlushType2View{};
        break;
    case NativePortGraphicsCommandKind::Present:
        result.payload = NativePortGraphicsPresentView{};
        break;
    case NativePortGraphicsCommandKind::RepeatPresent:
        result.payload = NativePortGraphicsRepeatPresentView{};
        break;
    case NativePortGraphicsCommandKind::PresentImage: {
        NativePortGraphicsPresentImagePayload root{};
        if (!read_root(bytes, root)) return std::nullopt;
        NativePortGraphicsPresentImageView view{};
        view.image.extent = {root.image.extent_width,
                             root.image.extent_height};
        view.image.format = static_cast<NativePortTextureFormat>(
            root.image.format);
        view.image.stride_bytes = root.image.stride_bytes;
        view.image.bottom_up = root.image.bottom_up != 0u;
        view.image.display_aspect_numerator =
            root.image.display_aspect_numerator;
        view.image.display_aspect_denominator =
            root.image.display_aspect_denominator;
        view.image.pixels = bytes.subspan(root.image.pixels_offset,
                                          root.image.pixels_size);
        view.viewport = static_cast<NativePortViewportTarget>(root.viewport);
        view.fit = static_cast<NativePortImageFit>(root.fit);
        result.payload = view;
        break;
    }
    case NativePortGraphicsCommandKind::Shutdown:
        result.payload = NativePortGraphicsShutdownView{};
        break;
    }
    return result;
}

} // namespace katana::runtime
