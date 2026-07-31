#include "host_video_d3d11.hpp"
#include "katana/runtime/host_video.hpp"

#ifdef _WIN32

#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace katana::runtime::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char shader_source[] = R"(
struct VertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
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
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.texcoord = texcoords[vertex_id];
    return output;
}

Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float4 pixel_main(VertexOutput input) : SV_Target {
    return source_texture.Sample(source_sampler, input.texcoord);
}
)";

struct ShaderCompilerApi {
    HMODULE module = nullptr;
    decltype(&D3DCompile) compile = nullptr;
};

[[nodiscard]] const ShaderCompilerApi& shader_compiler_api() noexcept {
    // Keep the selected compiler DLL pinned for the process lifetime because
    // compiled shaders are created only during presenter construction.
    static const auto api = []() noexcept {
        constexpr std::array compiler_names{
            L"d3dcompiler_47.dll",
            L"d3dcompiler_46.dll",
            L"d3dcompiler_43.dll",
        };
        for (const auto* compiler_name : compiler_names) {
            const auto module = LoadLibraryW(compiler_name);
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

ComPtr<ID3DBlob> compile_shader(const char* entry, const char* target) {
    const auto compile = shader_compiler_api().compile;
    if (compile == nullptr)
        throw std::runtime_error("D3D11-Hostvideocompiler ist nicht verfuegbar.");
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const auto result = compile(shader_source,
                                sizeof(shader_source) - 1u,
                                "katana-host-video-d3d11",
                                nullptr,
                                nullptr,
                                entry,
                                target,
                                D3DCOMPILE_ENABLE_STRICTNESS |
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0u,
                                bytecode.GetAddressOf(),
                                errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("D3D11-Hostvideoshader konnte nicht kompiliert werden.");
    return bytecode;
}

class D3d11Presenter final : public Win32D3d11Presenter {
  public:
    D3d11Presenter(const HWND window,
                   const std::uint32_t client_width,
                   const std::uint32_t client_height) {
        if (window == nullptr || client_width == 0u || client_height == 0u)
            throw std::invalid_argument("D3D11-Hostvideo besitzt kein gueltiges Fenster.");

        constexpr std::array requested_levels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        const auto create_device_and_swap_chain =
            [&](DXGI_SWAP_CHAIN_DESC& swap_chain_desc,
                const bool request_feature_level_11_1) {
            const auto first_level =
                request_feature_level_11_1 ? 0u : 1u;
            D3D_FEATURE_LEVEL selected_level = D3D_FEATURE_LEVEL_10_0;
            swap_chain_.Reset();
            device_.Reset();
            context_.Reset();
            return D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_SINGLETHREADED,
                requested_levels.data() + first_level,
                static_cast<UINT>(requested_levels.size() - first_level),
                D3D11_SDK_VERSION,
                &swap_chain_desc,
                swap_chain_.GetAddressOf(),
                device_.GetAddressOf(),
                &selected_level,
                context_.GetAddressOf());
        };

        HRESULT result = E_FAIL;
        for (const auto& configuration :
             win32_d3d11_swap_chain_configurations) {
            DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
            swap_chain_desc.BufferDesc.Width = client_width;
            swap_chain_desc.BufferDesc.Height = client_height;
            swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swap_chain_desc.SampleDesc.Count = 1u;
            swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swap_chain_desc.BufferCount = configuration.buffer_count;
            swap_chain_desc.OutputWindow = window;
            swap_chain_desc.Windowed = TRUE;
            swap_chain_desc.SwapEffect =
                configuration.mode ==
                        Win32D3d11SwapChainMode::FlipSequential
                ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                : DXGI_SWAP_EFFECT_DISCARD;
            result = create_device_and_swap_chain(swap_chain_desc, true);
            if (result == E_INVALIDARG)
                result =
                    create_device_and_swap_chain(swap_chain_desc, false);
            if (SUCCEEDED(result)) break;
        }
        if (FAILED(result))
            throw std::runtime_error("Hardware-D3D11-Geraet ist nicht verfuegbar.");

        ComPtr<IDXGIDevice1> dxgi_device;
        if (SUCCEEDED(device_.As(&dxgi_device)))
            static_cast<void>(dxgi_device->SetMaximumFrameLatency(1u));

        const auto vertex_bytecode = compile_shader("vertex_main", "vs_4_0");
        if (FAILED(device_->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                                vertex_bytecode->GetBufferSize(),
                                                nullptr,
                                                vertex_shader_.GetAddressOf())))
            throw std::runtime_error("D3D11-Vertexshader konnte nicht erstellt werden.");
        const auto pixel_bytecode = compile_shader("pixel_main", "ps_4_0");
        if (FAILED(device_->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                               pixel_bytecode->GetBufferSize(),
                                               nullptr,
                                               pixel_shader_.GetAddressOf())))
            throw std::runtime_error("D3D11-Pixelshader konnte nicht erstellt werden.");

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler_desc, sampler_.GetAddressOf())))
            throw std::runtime_error("D3D11-Sampler konnte nicht erstellt werden.");

        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(
                &rasterizer_desc, rasterizer_.GetAddressOf())))
            throw std::runtime_error("D3D11-Rasterzustand konnte nicht erstellt werden.");

        client_width_ = client_width;
        client_height_ = client_height;
        if (!create_render_target())
            throw std::runtime_error("D3D11-Backbuffer konnte nicht gebunden werden.");
    }

    ~D3d11Presenter() override {
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
    }

    [[nodiscard]] bool resize(const std::uint32_t client_width,
                              const std::uint32_t client_height) noexcept override {
        if (client_width == 0u || client_height == 0u) return true;
        if (client_width == client_width_ && client_height == client_height_)
            return true;
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);
        context_->OMSetRenderTargets(0u, nullptr, nullptr);
        render_target_.Reset();
        if (FAILED(swap_chain_->ResizeBuffers(
                0u, client_width, client_height, DXGI_FORMAT_UNKNOWN, 0u)))
            return false;
        client_width_ = client_width;
        client_height_ = client_height;
        return create_render_target();
    }

    [[nodiscard]] Win32D3d11PresentResult
    present(const std::span<const std::uint8_t> rgba,
            const std::uint32_t frame_width,
            const std::uint32_t frame_height,
            const std::uint32_t client_width,
            const std::uint32_t client_height) noexcept override {
        if (client_width == 0u || client_height == 0u)
            return Win32D3d11PresentResult::NotPresentable;
        if (occluded_) {
            const auto now_ms = GetTickCount64();
            if (!win32_d3d11_deadline_reached(
                    now_ms, next_occlusion_probe_ms_))
                return Win32D3d11PresentResult::Occluded;
            next_occlusion_probe_ms_ = win32_d3d11_deadline_after(
                now_ms, win32_d3d11_occlusion_probe_interval_ms);
            const auto visibility =
                swap_chain_->Present(0u, DXGI_PRESENT_TEST);
            if (visibility == DXGI_STATUS_OCCLUDED)
                return Win32D3d11PresentResult::Occluded;
            if (visibility != S_OK)
                return Win32D3d11PresentResult::Failed;
            occluded_ = false;
            next_occlusion_probe_ms_ = 0u;
        }
        if (!resize(client_width, client_height) ||
            !upload_frame(rgba, frame_width, frame_height))
            return Win32D3d11PresentResult::Failed;

        constexpr std::array clear_color{0.0f, 0.0f, 0.0f, 1.0f};
        context_->OMSetRenderTargets(1u, render_target_.GetAddressOf(), nullptr);
        context_->ClearRenderTargetView(render_target_.Get(), clear_color.data());

        const auto destination = calculate_native_video_viewport(
            frame_width, frame_height, client_width, client_height);
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(destination.x);
        viewport.TopLeftY = static_cast<float>(destination.y);
        viewport.Width = static_cast<float>(destination.width);
        viewport.Height = static_cast<float>(destination.height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1u, &viewport);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0u);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0u);
        context_->PSSetSamplers(0u, 1u, sampler_.GetAddressOf());
        context_->PSSetShaderResources(0u, 1u, frame_view_.GetAddressOf());
        context_->Draw(3u, 0u);
        ID3D11ShaderResourceView* no_view = nullptr;
        context_->PSSetShaderResources(0u, 1u, &no_view);

        const auto result = swap_chain_->Present(1u, 0u);
        if (result == S_OK) {
            occluded_ = false;
            next_occlusion_probe_ms_ = 0u;
            return Win32D3d11PresentResult::Presented;
        }
        if (result == DXGI_STATUS_OCCLUDED) {
            occluded_ = true;
            next_occlusion_probe_ms_ = win32_d3d11_deadline_after(
                GetTickCount64(),
                win32_d3d11_occlusion_probe_interval_ms);
            return Win32D3d11PresentResult::Occluded;
        }
        return Win32D3d11PresentResult::Failed;
    }

  private:
    [[nodiscard]] bool create_render_target() noexcept {
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain_->GetBuffer(
                0u,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(back_buffer.GetAddressOf()))))
            return false;
        return SUCCEEDED(device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, render_target_.GetAddressOf()));
    }

    [[nodiscard]] bool upload_frame(const std::span<const std::uint8_t> rgba,
                                    const std::uint32_t width,
                                    const std::uint32_t height) noexcept {
        if (width == 0u || height == 0u ||
            width > std::numeric_limits<std::size_t>::max() / 4u)
            return false;
        const auto row_bytes = static_cast<std::size_t>(width) * 4u;
        if (height > std::numeric_limits<std::size_t>::max() / row_bytes ||
            rgba.size() != row_bytes * height)
            return false;
        if (!frame_texture_ || width != frame_width_ || height != frame_height_) {
            ID3D11ShaderResourceView* no_view = nullptr;
            context_->PSSetShaderResources(0u, 1u, &no_view);
            frame_view_.Reset();
            frame_texture_.Reset();
            D3D11_TEXTURE2D_DESC texture_desc{};
            texture_desc.Width = width;
            texture_desc.Height = height;
            texture_desc.MipLevels = 1u;
            texture_desc.ArraySize = 1u;
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_desc.SampleDesc.Count = 1u;
            texture_desc.Usage = D3D11_USAGE_DYNAMIC;
            texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateTexture2D(
                    &texture_desc, nullptr, frame_texture_.GetAddressOf())) ||
                FAILED(device_->CreateShaderResourceView(
                    frame_texture_.Get(), nullptr, frame_view_.GetAddressOf())))
                return false;
            frame_width_ = width;
            frame_height_ = height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(
                frame_texture_.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped)))
            return false;
        if (mapped.RowPitch < row_bytes) {
            context_->Unmap(frame_texture_.Get(), 0u);
            return false;
        }
        for (std::uint32_t row = 0u; row < height; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(row) * mapped.RowPitch,
                        rgba.data() + static_cast<std::size_t>(row) * row_bytes,
                        row_bytes);
        }
        context_->Unmap(frame_texture_.Get(), 0u);
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Texture2D> frame_texture_;
    ComPtr<ID3D11ShaderResourceView> frame_view_;
    std::uint32_t client_width_ = 0u;
    std::uint32_t client_height_ = 0u;
    std::uint32_t frame_width_ = 0u;
    std::uint32_t frame_height_ = 0u;
    std::uint64_t next_occlusion_probe_ms_ = 0u;
    bool occluded_ = false;
};

} // namespace

std::unique_ptr<Win32D3d11Presenter>
try_create_win32_d3d11_presenter(void* const native_window,
                                 const std::uint32_t client_width,
                                 const std::uint32_t client_height) noexcept {
    try {
        return std::make_unique<D3d11Presenter>(
            static_cast<HWND>(native_window), client_width, client_height);
    } catch (...) {
        return nullptr;
    }
}

} // namespace katana::runtime::detail

#endif
