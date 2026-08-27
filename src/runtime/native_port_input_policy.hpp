#pragma once

#include "katana/runtime/native_port_platform.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace katana::runtime::detail {

inline constexpr std::uint64_t xinput_device_domain =
    0x0100000000000000ull;
inline constexpr std::uint64_t joystick_device_domain =
    0x0200000000000000ull;
inline constexpr std::uint64_t keyboard_device_domain =
    0x0300000000000000ull;
inline constexpr std::uint64_t input_device_domain_mask =
    0xFF00000000000000ull;

struct NativeGamepadButtonStability final {
    std::uint32_t buttons = 0u;
    std::array<std::uint32_t, 2u> release_history{};
};

// A physical pad exposed through more than one Windows input path can leave a
// short run of neutral samples between the two representations of one button
// press. Publishing that run creates a false release/press pair at the title
// boundary. Confirm releases only after three consecutive neutral polls while
// publishing presses immediately. This covers the observed zero-, one- and
// two-poll XInput/HID hand-off holes; a stable release still clears the state.
[[nodiscard]] constexpr std::uint32_t stabilize_gamepad_buttons(
    NativeGamepadButtonStability& stability,
    const std::uint32_t raw_buttons) noexcept {
    const auto released = stability.buttons & ~raw_buttons;
    const auto confirmed =
        released & stability.release_history[0] &
        stability.release_history[1];
    stability.buttons = (stability.buttons | raw_buttons) & ~confirmed;
    stability.release_history[1] = stability.release_history[0];
    stability.release_history[0] = released & ~confirmed;
    return stability.buttons;
}

// Keep a proven cross-backend alias for exactly the poll in which its old,
// currently assigned endpoint disappeared and the correlated replacement is
// still visible. Once the slot has moved, the absent endpoint is no longer
// assigned and the stale alias is retired on the next poll.
[[nodiscard]] constexpr bool retain_cross_backend_alias(
    const bool first_present,
    const bool second_present,
    const bool first_assigned,
    const bool second_assigned) noexcept {
    if (first_present && second_present) return true;
    if (first_present == second_present) return false;
    return first_present ? second_assigned : first_assigned;
}

[[nodiscard]] constexpr bool is_correlated_backend_handoff(
    const std::uint64_t old_device_id,
    const std::uint64_t new_device_id,
    const std::span<const std::pair<std::uint64_t, std::uint64_t>> aliases)
    noexcept {
    if (old_device_id == 0u || new_device_id == 0u ||
        old_device_id == new_device_id)
        return false;

    const auto old_domain = old_device_id & input_device_domain_mask;
    const auto new_domain = new_device_id & input_device_domain_mask;
    if (!((old_domain == xinput_device_domain &&
           new_domain == joystick_device_domain) ||
          (old_domain == joystick_device_domain &&
           new_domain == xinput_device_domain)))
        return false;

    for (const auto& alias : aliases) {
        if ((alias.first == old_device_id && alias.second == new_device_id) ||
            (alias.first == new_device_id && alias.second == old_device_id))
            return true;
    }
    return false;
}

} // namespace katana::runtime::detail
