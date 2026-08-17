#pragma once

#include "katana/runtime/maple.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_controller_contract_version = 1u;
inline constexpr std::uint32_t controller_input_recording_version = 1u;
inline constexpr std::size_t controller_input_recording_default_max_frames = 360'000u;

enum class HostControllerKind : std::uint8_t {
    None,
    Keyboard,
    XInput,
    DualSense,
    DualShock,
    StandardHid
};

enum class HostControllerButton : std::uint32_t {
    South = 1u << 0u,
    East = 1u << 1u,
    West = 1u << 2u,
    North = 1u << 3u,
    Start = 1u << 4u,
    Back = 1u << 5u,
    DpadUp = 1u << 6u,
    DpadDown = 1u << 7u,
    DpadLeft = 1u << 8u,
    DpadRight = 1u << 9u,
    LeftShoulder = 1u << 10u,
    RightShoulder = 1u << 11u,
    LeftStick = 1u << 12u,
    RightStick = 1u << 13u
};

using HostControllerButtonMask = std::uint32_t;

[[nodiscard]] constexpr HostControllerButtonMask
host_controller_button(const HostControllerButton value) noexcept {
    return static_cast<HostControllerButtonMask>(value);
}

struct HostControllerSample {
    std::uint64_t device_id = 0u;
    HostControllerKind kind = HostControllerKind::None;
    HostControllerButtonMask buttons = 0u;
    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;
    std::uint16_t left_trigger = 0u;
    std::uint16_t right_trigger = 0u;
    bool connected = false;

    [[nodiscard]] bool operator==(const HostControllerSample&) const = default;
};

struct ControllerNormalizationConfig {
    std::uint16_t left_stick_deadzone = 7'849u;
    std::uint16_t right_stick_deadzone = 8'689u;
    std::uint16_t trigger_deadzone = 7'710u;
};

[[nodiscard]] ControllerState
normalize_host_controller(const HostControllerSample& sample,
                          const ControllerNormalizationConfig& config = {});

enum class KeyboardControllerKey : std::uint8_t {
    Start,
    A,
    B,
    X,
    Y,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    Count
};

enum class ControllerInputChangeKind : std::uint8_t {
    State,
    Connected,
    Disconnected,
    FocusReset,
    Selection
};

struct ControllerInputChange {
    std::uint64_t sequence = 0u;
    std::uint64_t guest_cycle = 0u;
    ControllerInputChangeKind change = ControllerInputChangeKind::State;
    HostControllerKind controller_kind = HostControllerKind::None;
    std::uint64_t device_id = 0u;
    bool connected = false;
    ControllerState state{};

    [[nodiscard]] bool operator==(const ControllerInputChange&) const = default;
};

class HostGamepadSource {
  public:
    virtual ~HostGamepadSource() = default;
    [[nodiscard]] virtual std::vector<HostControllerSample> poll() = 0;
};

class ControllerInputTimeline final : public HostInputBackend {
  public:
    explicit ControllerInputTimeline(ControllerNormalizationConfig config = {});

    [[nodiscard]] std::optional<ControllerInputChange>
    poll(HostGamepadSource& source, std::uint64_t guest_cycle);
    [[nodiscard]] std::optional<ControllerInputChange>
    keyboard_event(KeyboardControllerKey key, bool pressed, std::uint64_t guest_cycle);
    [[nodiscard]] std::optional<ControllerInputChange>
    set_focus(bool focused, std::uint64_t guest_cycle);
    [[nodiscard]] std::optional<ControllerInputChange>
    select_controller(std::optional<std::uint64_t> device_id, std::uint64_t guest_cycle);

    [[nodiscard]] ControllerState sample(std::uint64_t frame) override;
    [[nodiscard]] ControllerState sample_at(std::uint64_t frame,
                                            std::uint64_t guest_cycle) override;
    [[nodiscard]] std::vector<ControllerInputChange> trace() const;
    [[nodiscard]] std::optional<std::uint64_t> active_device_id() const;
    [[nodiscard]] std::optional<std::uint64_t> selected_device_id() const;
    [[nodiscard]] bool focused() const;
    [[nodiscard]] std::uint64_t sampled_frames() const;

  private:
    [[nodiscard]] std::optional<ControllerInputChange>
    update_locked(std::uint64_t guest_cycle, ControllerInputChangeKind requested_change);
    void validate_cycle_locked(std::uint64_t guest_cycle);
    void choose_active_locked();
    [[nodiscard]] ControllerState keyboard_state_locked() const noexcept;
    [[nodiscard]] ControllerState combined_state_locked() const;

    ControllerNormalizationConfig config_;
    mutable std::mutex mutex_;
    std::vector<HostControllerSample> devices_;
    std::vector<ControllerInputChange> trace_;
    std::array<bool, static_cast<std::size_t>(KeyboardControllerKey::Count)> keyboard_{};
    std::optional<std::uint64_t> selected_device_id_;
    std::optional<std::uint64_t> active_device_id_;
    ControllerState visible_state_{};
    HostControllerKind visible_kind_ = HostControllerKind::None;
    std::uint64_t visible_device_id_ = 0u;
    std::uint64_t next_sequence_ = 1u;
    std::uint64_t last_guest_cycle_ = 0u;
    std::uint64_t sampled_frames_ = 0u;
    bool have_guest_cycle_ = false;
    bool visible_connected_ = false;
    bool focused_ = true;
};

class ControllerInputReplay final : public HostInputBackend {
  public:
    explicit ControllerInputReplay(std::vector<ControllerInputChange> trace);
    [[nodiscard]] ControllerState sample(std::uint64_t frame) override;
    [[nodiscard]] ControllerState sample_at(std::uint64_t frame,
                                            std::uint64_t guest_cycle) override;
    [[nodiscard]] std::uint64_t sampled_frames() const noexcept;
    [[nodiscard]] const std::vector<ControllerInputChange>& trace() const noexcept;

  private:
    std::vector<ControllerInputChange> trace_;
    std::uint64_t sampled_frames_ = 0u;
};

// A bounded, contiguous frame capture for deterministic diagnostics. The
// recording owns no host handles and its sample path is a constant-time array
// lookup with no locking, allocation, or filesystem access. The binary format
// is intentionally independent of a title or platform backend; callers bind
// it to a build/content identity when saving and loading.
class ControllerInputRecording final : public HostInputBackend {
  public:
    using Frame = std::array<ControllerState, maple_port_count>;

    explicit ControllerInputRecording(
        std::size_t max_frames = controller_input_recording_default_max_frames);

    ControllerInputRecording(const ControllerInputRecording&) = delete;
    ControllerInputRecording& operator=(const ControllerInputRecording&) = delete;
    ControllerInputRecording(ControllerInputRecording&&) noexcept = default;
    ControllerInputRecording& operator=(ControllerInputRecording&&) noexcept = default;

    // Frames must be appended consecutively. This keeps replay frame lookup
    // O(1) and makes dropped/duplicated capture frames fail closed.
    void append(std::uint64_t frame, std::span<const ControllerState> slots);
    [[nodiscard]] ControllerState sample(std::uint64_t frame) override;
    [[nodiscard]] ControllerState sample_at(std::uint64_t frame,
                                             std::uint64_t guest_cycle) override;
    [[nodiscard]] ControllerState sample_slot(std::uint64_t frame,
                                               std::size_t slot) const;

    [[nodiscard]] std::uint64_t first_frame() const noexcept;
    [[nodiscard]] std::size_t frame_count() const noexcept;
    [[nodiscard]] std::size_t max_frames() const noexcept;
    [[nodiscard]] const std::vector<Frame>& frames() const noexcept;

    // Identity is stored in the file header and must be supplied by the
    // caller. Loading with an expected identity rejects mismatched captures.
    void save(const std::filesystem::path& path,
              std::string_view identity) const;
    [[nodiscard]] static ControllerInputRecording
    load(const std::filesystem::path& path,
         std::string_view expected_identity = {},
         std::size_t max_frames = controller_input_recording_default_max_frames);

  private:
    std::vector<Frame> frames_;
    std::size_t max_frames_ = 0u;
    std::uint64_t first_frame_ = 0u;
    bool have_first_frame_ = false;
};

[[nodiscard]] bool native_gamepad_input_available() noexcept;
[[nodiscard]] std::unique_ptr<HostGamepadSource> create_native_gamepad_source();
[[nodiscard]] const char* host_controller_kind_name(HostControllerKind kind) noexcept;

} // namespace katana::runtime
