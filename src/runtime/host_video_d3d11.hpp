#pragma once

#ifdef _WIN32

#include "katana/runtime/host_video.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace katana::runtime::detail {

enum class Win32D3d11SwapChainMode : std::uint8_t {
    FlipSequential,
    LegacyDiscard,
};

struct Win32D3d11SwapChainConfiguration {
    Win32D3d11SwapChainMode mode = Win32D3d11SwapChainMode::LegacyDiscard;
    std::uint32_t buffer_count = 1u;

    [[nodiscard]] bool operator==(
        const Win32D3d11SwapChainConfiguration&) const = default;
};

inline constexpr std::array win32_d3d11_swap_chain_configurations{
    Win32D3d11SwapChainConfiguration{
        Win32D3d11SwapChainMode::FlipSequential, 2u},
    Win32D3d11SwapChainConfiguration{
        Win32D3d11SwapChainMode::LegacyDiscard, 1u},
};

inline constexpr std::uint64_t win32_d3d11_occlusion_probe_interval_ms = 100u;
inline constexpr std::uint64_t win32_d3d11_recovery_initial_delay_ms = 250u;
inline constexpr std::uint64_t win32_d3d11_recovery_maximum_delay_ms = 4'000u;
inline constexpr std::uint32_t win32_d3d11_recovery_maximum_attempts = 6u;

[[nodiscard]] constexpr std::uint64_t
win32_d3d11_deadline_after(const std::uint64_t now_ms,
                           const std::uint64_t delay_ms) noexcept {
    return delay_ms > std::numeric_limits<std::uint64_t>::max() - now_ms
        ? std::numeric_limits<std::uint64_t>::max()
        : now_ms + delay_ms;
}

[[nodiscard]] constexpr bool
win32_d3d11_deadline_reached(const std::uint64_t now_ms,
                             const std::uint64_t deadline_ms) noexcept {
    return now_ms >= deadline_ms;
}

[[nodiscard]] constexpr std::uint64_t
win32_d3d11_recovery_delay_ms(const std::uint32_t failed_attempts) noexcept {
    auto delay = win32_d3d11_recovery_initial_delay_ms;
    for (std::uint32_t attempt = 0u;
         attempt < failed_attempts &&
         delay < win32_d3d11_recovery_maximum_delay_ms;
         ++attempt) {
        delay = delay >
                        win32_d3d11_recovery_maximum_delay_ms / 2u
            ? win32_d3d11_recovery_maximum_delay_ms
            : delay * 2u;
    }
    return delay;
}

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
