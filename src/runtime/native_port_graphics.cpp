#include "katana/runtime/native_port_graphics.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

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
    return topology == NativePortPrimitiveTopology::TriangleList ||
           topology == NativePortPrimitiveTopology::TriangleStrip ||
           topology == NativePortPrimitiveTopology::LineList;
}

[[nodiscard]] bool valid_blend(const NativePortBlendMode blend) noexcept {
    return blend == NativePortBlendMode::Opaque ||
           blend == NativePortBlendMode::Alpha ||
           blend == NativePortBlendMode::Additive ||
           blend == NativePortBlendMode::Multiply;
}

[[nodiscard]] bool valid_depth(const NativePortDepthMode depth) noexcept {
    return depth == NativePortDepthMode::Disabled ||
           depth == NativePortDepthMode::ReadWrite ||
           depth == NativePortDepthMode::ReadOnly;
}

[[nodiscard]] bool valid_cull(const NativePortCullMode cull) noexcept {
    return cull == NativePortCullMode::None ||
           cull == NativePortCullMode::Front ||
           cull == NativePortCullMode::Back;
}

[[nodiscard]] bool valid_filter(const NativePortTextureFilter filter) noexcept {
    return filter == NativePortTextureFilter::Nearest ||
           filter == NativePortTextureFilter::Linear;
}

[[nodiscard]] bool valid_address(
    const NativePortTextureAddress address) noexcept {
    return address == NativePortTextureAddress::Clamp ||
           address == NativePortTextureAddress::Wrap ||
           address == NativePortTextureAddress::Mirror;
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
        config.maximum_transient_vertices < 3u ||
        config.maximum_transient_indices < 3u ||
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

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const char* entry,
                                               const char* target);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view value);
[[nodiscard]] DXGI_FORMAT texture_format(
    NativePortTextureFormat format) noexcept;
[[nodiscard]] D3D11_PRIMITIVE_TOPOLOGY primitive_topology(
    NativePortPrimitiveTopology topology) noexcept;
[[nodiscard]] UINT next_buffer_capacity(UINT required) noexcept;

} // namespace

class NativePortGraphicsDevice::Impl final {
  public:
    explicit Impl(const NativePortGraphicsConfig& config)
        : title_storage_(copy_validated_graphics_title(config.title)),
          config_(config), owner_thread_(std::this_thread::get_id()) {
        config_.title = title_storage_;
        validate_graphics_config(config_);
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
        context_->OMSetRenderTargets(
            1u, render_target_.GetAddressOf(), depth_view_.Get());
        context_->ClearRenderTargetView(render_target_.Get(),
                                        frame.clear_color.data());
        context_->ClearDepthStencilView(
            depth_view_.Get(), D3D11_CLEAR_DEPTH, frame.clear_depth, 0u);
        frame_open_ = true;
        saturating_increment(snapshot_.begun_frames);
    }

    void draw(const NativePortDrawPacket& packet) {
        require_owner_thread();
        if (!frame_open_)
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-outside-frame");
        validate_draw(packet);
        ensure_vertex_buffer(packet.vertices.size_bytes());
        upload_dynamic(vertex_buffer_.Get(), packet.vertices.data(),
                       packet.vertices.size_bytes());
        if (!packet.indices.empty()) {
            ensure_index_buffer(packet.indices.size_bytes());
            upload_dynamic(index_buffer_.Get(), packet.indices.data(),
                           packet.indices.size_bytes());
        }

        DrawConstants constants{};
        constants.transform = packet.transform.values;
        context_->UpdateSubresource(
            draw_constants_.Get(), 0u, nullptr, &constants, 0u, 0u);

        const auto current_layout = layout();
        const auto viewport_rect =
            packet.viewport == NativePortViewportTarget::Ui
                ? current_layout.ui_viewport
                : current_layout.game_viewport;
        set_viewport(viewport_rect);

        const auto blend_index = static_cast<std::size_t>(packet.blend);
        const auto depth_index = static_cast<std::size_t>(packet.depth);
        const auto cull_index = static_cast<std::size_t>(packet.cull);
        const auto filter_index = static_cast<std::size_t>(packet.filter);
        const auto address_index = static_cast<std::size_t>(packet.address);
        constexpr std::array blend_factor{0.0f, 0.0f, 0.0f, 0.0f};
        context_->OMSetBlendState(
            blend_states_[blend_index].Get(), blend_factor.data(), 0xFFFFFFFFu);
        context_->OMSetDepthStencilState(depth_states_[depth_index].Get(), 0u);
        context_->RSSetState(rasterizer_states_[cull_index].Get());
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(primitive_topology(packet.topology));
        const UINT stride = sizeof(NativePortVertex);
        const UINT offset = 0u;
        context_->IASetVertexBuffers(
            0u, 1u, vertex_buffer_.GetAddressOf(), &stride, &offset);
        if (!packet.indices.empty())
            context_->IASetIndexBuffer(
                index_buffer_.Get(), DXGI_FORMAT_R32_UINT, 0u);
        context_->VSSetShader(draw_vertex_shader_.Get(), nullptr, 0u);
        context_->VSSetConstantBuffers(
            0u, 1u, draw_constants_.GetAddressOf());
        context_->PSSetShader(draw_pixel_shader_.Get(), nullptr, 0u);
        context_->PSSetSamplers(
            0u,
            1u,
            sampler_states_[filter_index][address_index].GetAddressOf());
        auto* view = packet.texture ? resolve_texture(packet.texture).view.Get()
                                    : white_view_.Get();
        context_->PSSetShaderResources(0u, 1u, &view);
        if (packet.indices.empty())
            context_->Draw(static_cast<UINT>(packet.vertices.size()), 0u);
        else
            context_->DrawIndexed(static_cast<UINT>(packet.indices.size()),
                                  0u,
                                  0);
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);
        saturating_increment(snapshot_.draw_calls);
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
        context_->OMSetBlendState(
            blend_states_[0].Get(), blend_factor.data(), 0xFFFFFFFFu);
        context_->OMSetDepthStencilState(depth_states_[0].Get(), 0u);
        context_->RSSetState(rasterizer_states_[0].Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(composite_vertex_shader_.Get(), nullptr, 0u);
        context_->PSSetShader(composite_pixel_shader_.Get(), nullptr, 0u);
        context_->PSSetSamplers(
            0u, 1u, sampler_states_[1][0].GetAddressOf());
        context_->PSSetShaderResources(
            0u, 1u, render_view_.GetAddressOf());
        context_->Draw(3u, 0u);
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);
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
                static_cast<double>(image.extent.width) / image.extent.height;
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
        packet.depth = NativePortDepthMode::Disabled;
        packet.cull = NativePortCullMode::None;
        packet.blend = NativePortBlendMode::Opaque;
        draw(packet);
        present();
    }

    [[nodiscard]] NativePortGraphicsSnapshot snapshot() const {
        require_owner_thread();
        auto result = snapshot_;
        result.live_textures = live_textures_;
        result.hardware_accelerated = device_ != nullptr;
        result.frame_open = frame_open_;
        return result;
    }

  private:
    struct DrawConstants final {
        std::array<float, 16u> transform{};
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

        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 3u> elements{
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
        create_blend_states();
        create_depth_states();
        create_rasterizer_states();
        create_sampler_states();
    }

    void create_blend_states() {
        for (std::size_t index = 0u; index < blend_states_.size(); ++index) {
            D3D11_BLEND_DESC description{};
            auto& target = description.RenderTarget[0];
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (index != static_cast<std::size_t>(NativePortBlendMode::Opaque)) {
                target.BlendEnable = TRUE;
                target.BlendOp = D3D11_BLEND_OP_ADD;
                target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                target.SrcBlendAlpha = D3D11_BLEND_ONE;
                target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                switch (static_cast<NativePortBlendMode>(index)) {
                case NativePortBlendMode::Alpha:
                    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                    break;
                case NativePortBlendMode::Additive:
                    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                    target.DestBlend = D3D11_BLEND_ONE;
                    break;
                case NativePortBlendMode::Multiply:
                    target.SrcBlend = D3D11_BLEND_DEST_COLOR;
                    target.DestBlend = D3D11_BLEND_ZERO;
                    break;
                case NativePortBlendMode::Opaque:
                    break;
                }
            }
            const auto result = device_->CreateBlendState(
                &description, blend_states_[index].GetAddressOf());
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     "blend-state");
        }
    }

    void create_depth_states() {
        for (std::size_t index = 0u; index < depth_states_.size(); ++index) {
            D3D11_DEPTH_STENCIL_DESC description{};
            description.DepthEnable =
                index == static_cast<std::size_t>(NativePortDepthMode::Disabled)
                    ? FALSE
                    : TRUE;
            description.DepthWriteMask =
                index == static_cast<std::size_t>(NativePortDepthMode::ReadWrite)
                    ? D3D11_DEPTH_WRITE_MASK_ALL
                    : D3D11_DEPTH_WRITE_MASK_ZERO;
            description.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            const auto result = device_->CreateDepthStencilState(
                &description, depth_states_[index].GetAddressOf());
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     "depth-state");
        }
    }

    void create_rasterizer_states() {
        for (std::size_t index = 0u; index < rasterizer_states_.size(); ++index) {
            D3D11_RASTERIZER_DESC description{};
            description.FillMode = D3D11_FILL_SOLID;
            description.CullMode =
                index == static_cast<std::size_t>(NativePortCullMode::Front)
                    ? D3D11_CULL_FRONT
                    : index == static_cast<std::size_t>(NativePortCullMode::Back)
                          ? D3D11_CULL_BACK
                          : D3D11_CULL_NONE;
            description.FrontCounterClockwise = FALSE;
            description.DepthClipEnable = TRUE;
            description.ScissorEnable = TRUE;
            const auto result = device_->CreateRasterizerState(
                &description, rasterizer_states_[index].GetAddressOf());
            if (FAILED(result))
                fail(NativePortGraphicsFailure::ResourceCreation,
                     static_cast<std::uint32_t>(result),
                     "rasterizer-state");
        }
    }

    void create_sampler_states() {
        for (std::size_t filter = 0u; filter < sampler_states_.size(); ++filter) {
            for (std::size_t address = 0u;
                 address < sampler_states_[filter].size();
                 ++address) {
                D3D11_SAMPLER_DESC description{};
                description.Filter =
                    filter == static_cast<std::size_t>(
                                  NativePortTextureFilter::Linear)
                        ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                        : D3D11_FILTER_MIN_MAG_MIP_POINT;
                const auto address_mode =
                    address == static_cast<std::size_t>(
                                   NativePortTextureAddress::Wrap)
                        ? D3D11_TEXTURE_ADDRESS_WRAP
                        : address == static_cast<std::size_t>(
                                         NativePortTextureAddress::Mirror)
                              ? D3D11_TEXTURE_ADDRESS_MIRROR
                              : D3D11_TEXTURE_ADDRESS_CLAMP;
                description.AddressU = address_mode;
                description.AddressV = address_mode;
                description.AddressW = address_mode;
                description.MaxLOD = D3D11_FLOAT32_MAX;
                const auto result = device_->CreateSamplerState(
                    &description,
                    sampler_states_[filter][address].GetAddressOf());
                if (FAILED(result))
                    fail(NativePortGraphicsFailure::ResourceCreation,
                         static_cast<std::uint32_t>(result),
                         "sampler-state");
            }
        }
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

    void validate_draw(const NativePortDrawPacket& packet) {
        if (!valid_viewport_target(packet.viewport) ||
            !valid_topology(packet.topology) ||
            !valid_blend(packet.blend) ||
            !valid_depth(packet.depth) ||
            !valid_cull(packet.cull) ||
            !valid_filter(packet.filter) ||
            !valid_address(packet.address) ||
            packet.vertices.empty() ||
            packet.vertices.size() > config_.maximum_transient_vertices ||
            packet.indices.size() > config_.maximum_transient_indices ||
            packet.vertices.size_bytes() > std::numeric_limits<UINT>::max() ||
            packet.indices.size_bytes() > std::numeric_limits<UINT>::max() ||
            !std::all_of(packet.transform.values.begin(),
                         packet.transform.values.end(),
                         [](const float value) { return std::isfinite(value); }))
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-layout");
        for (const auto index : packet.indices) {
            if (index >= packet.vertices.size())
                fail(NativePortGraphicsFailure::InvalidDraw,
                     0u,
                     "draw-index");
        }
        for (const auto& vertex : packet.vertices) {
            const auto finite = [](const float value) {
                return std::isfinite(value);
            };
            if (!std::all_of(vertex.position.begin(),
                             vertex.position.end(), finite) ||
                !std::all_of(vertex.texture_coordinate.begin(),
                             vertex.texture_coordinate.end(), finite) ||
                !std::all_of(vertex.color.begin(), vertex.color.end(), finite))
                fail(NativePortGraphicsFailure::InvalidDraw,
                     0u,
                     "draw-vertex");
        }
        const auto element_count = packet.indices.empty()
                                       ? packet.vertices.size()
                                       : packet.indices.size();
        const bool valid_count =
            packet.topology == NativePortPrimitiveTopology::TriangleList
                ? element_count >= 3u && element_count % 3u == 0u
                : packet.topology == NativePortPrimitiveTopology::TriangleStrip
                      ? element_count >= 3u
                      : element_count >= 2u && element_count % 2u == 0u;
        if (!valid_count)
            fail(NativePortGraphicsFailure::InvalidDraw, 0u, "draw-topology");
        if (packet.texture) static_cast<void>(resolve_texture(packet.texture));
    }

    void ensure_vertex_buffer(const std::size_t byte_size) {
        if (byte_size <= vertex_buffer_capacity_) return;
        const auto required = static_cast<UINT>(byte_size);
        const auto capacity = next_buffer_capacity(required);
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
    }

    void ensure_index_buffer(const std::size_t byte_size) {
        if (byte_size <= index_buffer_capacity_) return;
        const auto required = static_cast<UINT>(byte_size);
        const auto capacity = next_buffer_capacity(required);
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
    }

    void upload_dynamic(ID3D11Buffer* const buffer,
                        const void* const source,
                        const std::size_t byte_size) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result = context_->Map(
            buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);
        if (FAILED(result))
            fail(NativePortGraphicsFailure::DeviceLost,
                 static_cast<std::uint32_t>(result),
                 "buffer-map");
        std::memcpy(mapped.pData, source, byte_size);
        context_->Unmap(buffer, 0u);
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

    void set_viewport(const NativePortPixelRect rect) {
        if (rect.width == 0u || rect.height == 0u)
            fail(NativePortGraphicsFailure::InvalidFrame, 0u, "viewport-empty");
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

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> swap_chain_target_;
    ComPtr<ID3D11Texture2D> render_texture_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11ShaderResourceView> render_view_;
    ComPtr<ID3D11Texture2D> depth_texture_;
    ComPtr<ID3D11DepthStencilView> depth_view_;
    ComPtr<ID3D11VertexShader> draw_vertex_shader_;
    ComPtr<ID3D11PixelShader> draw_pixel_shader_;
    ComPtr<ID3D11VertexShader> composite_vertex_shader_;
    ComPtr<ID3D11PixelShader> composite_pixel_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11Buffer> draw_constants_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> index_buffer_;
    UINT vertex_buffer_capacity_ = 0u;
    UINT index_buffer_capacity_ = 0u;
    std::array<ComPtr<ID3D11BlendState>, 4u> blend_states_;
    std::array<ComPtr<ID3D11DepthStencilState>, 3u> depth_states_;
    std::array<ComPtr<ID3D11RasterizerState>, 3u> rasterizer_states_;
    std::array<std::array<ComPtr<ID3D11SamplerState>, 3u>, 2u>
        sampler_states_;
    ComPtr<ID3D11Texture2D> white_texture_;
    ComPtr<ID3D11ShaderResourceView> white_view_;

    std::vector<TextureSlot> texture_slots_;
    std::vector<std::uint32_t> free_texture_slots_;
    std::uint64_t texture_bytes_ = 0u;
    std::uint32_t live_textures_ = 0u;
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
    frame_pacing_snapshot_.simulation_rate_hz =
        frame_pacing_config_.simulation_rate_hz;
    frame_pacing_snapshot_.presentation_rate_hz =
        frame_pacing_config_.presentation_rate_hz;
    frame_pacing_snapshot_.enabled = frame_pacing_config_.enabled;
}

NativePortDesktopHost::~NativePortDesktopHost() = default;

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
        // Never issue rapid catch-up frames. A late native frame is presented
        // once, then both clocks restart from the actual completion time.
        present_and_record(false);
        saturating_increment(frame_pacing_snapshot_.simulation_frames);
        now = monotonic_time_nanoseconds();
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
        if (simulation_late)
            saturating_increment(
                frame_pacing_snapshot_.late_simulation_frames);
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
        next_presentation_deadline_nanoseconds_ = now;
        presentation_deadline_remainder_ = 0u;
        advance_frame_deadline(
            next_presentation_deadline_nanoseconds_,
            presentation_deadline_remainder_,
            frame_pacing_config_.presentation_rate_hz);
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
};

struct DrawVertexInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR0;
};

struct DrawVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : COLOR0;
};

DrawVertexOutput draw_vertex_main(DrawVertexInput input) {
    DrawVertexOutput output;
    output.position = mul(float4(input.position, 1.0), draw_transform);
    output.texcoord = input.texcoord;
    output.color = input.color;
    return output;
}

Texture2D draw_texture : register(t0);
SamplerState draw_sampler : register(s0);

float4 draw_pixel_main(DrawVertexOutput input) : SV_Target {
    return draw_texture.Sample(draw_sampler, input.texcoord) * input.color;
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
    case NativePortPrimitiveTopology::TriangleList:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case NativePortPrimitiveTopology::TriangleStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case NativePortPrimitiveTopology::LineList:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
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
