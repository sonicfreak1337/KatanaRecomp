#pragma once

#ifdef _WIN32

#include "katana/runtime/host_video.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace katana::runtime::detail {

enum class Win32D3d11PresentResult : std::uint8_t {
    Presented,
    Occluded,
    NotPresentable,
    Failed,
};

struct Win32D3d11PresentDecision {
    NativeVideoPresentationOutcome outcome =
        NativeVideoPresentationOutcome::BackendFailure;
    bool retain_backend = false;

    [[nodiscard]] bool operator==(
        const Win32D3d11PresentDecision&) const = default;
};

[[nodiscard]] constexpr Win32D3d11PresentDecision
classify_win32_d3d11_present_result(
    const Win32D3d11PresentResult result) noexcept {
    switch (result) {
    case Win32D3d11PresentResult::Presented:
        return {NativeVideoPresentationOutcome::Presented, true};
    case Win32D3d11PresentResult::Occluded:
        return {NativeVideoPresentationOutcome::Occluded, true};
    case Win32D3d11PresentResult::NotPresentable:
        return {NativeVideoPresentationOutcome::NotPresentable, true};
    case Win32D3d11PresentResult::Failed:
        return {NativeVideoPresentationOutcome::BackendFailure, false};
    }
    return {};
}

class Win32D3d11Presenter {
  public:
    virtual ~Win32D3d11Presenter() = default;
    [[nodiscard]] virtual bool resize(std::uint32_t client_width,
                                      std::uint32_t client_height) noexcept = 0;
    [[nodiscard]] virtual Win32D3d11PresentResult
    present(std::span<const std::uint8_t> rgba,
            std::uint32_t frame_width,
            std::uint32_t frame_height,
            std::uint32_t client_width,
            std::uint32_t client_height) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<Win32D3d11Presenter>
try_create_win32_d3d11_presenter(void* native_window,
                                 std::uint32_t client_width,
                                 std::uint32_t client_height) noexcept;

} // namespace katana::runtime::detail

#endif
