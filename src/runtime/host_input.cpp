#include "katana/runtime/host_input.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#include <array>
#include <cwchar>
#endif

namespace katana::runtime {
namespace {

constexpr HostControllerButtonMask known_host_buttons =
    host_controller_button(HostControllerButton::South) |
    host_controller_button(HostControllerButton::East) |
    host_controller_button(HostControllerButton::West) |
    host_controller_button(HostControllerButton::North) |
    host_controller_button(HostControllerButton::Start) |
    host_controller_button(HostControllerButton::Back) |
    host_controller_button(HostControllerButton::DpadUp) |
    host_controller_button(HostControllerButton::DpadDown) |
    host_controller_button(HostControllerButton::DpadLeft) |
    host_controller_button(HostControllerButton::DpadRight) |
    host_controller_button(HostControllerButton::LeftShoulder) |
    host_controller_button(HostControllerButton::RightShoulder) |
    host_controller_button(HostControllerButton::LeftStick) |
    host_controller_button(HostControllerButton::RightStick);

bool known_kind(const HostControllerKind kind) noexcept {
    switch (kind) {
    case HostControllerKind::None:
    case HostControllerKind::Keyboard:
    case HostControllerKind::XInput:
    case HostControllerKind::DualSense:
    case HostControllerKind::DualShock:
    case HostControllerKind::StandardHid:
        return true;
    }
    return false;
}

bool known_change(const ControllerInputChangeKind change) noexcept {
    switch (change) {
    case ControllerInputChangeKind::State:
    case ControllerInputChangeKind::Connected:
    case ControllerInputChangeKind::Disconnected:
    case ControllerInputChangeKind::FocusReset:
    case ControllerInputChangeKind::Selection:
        return true;
    }
    return false;
}

void validate_config(const ControllerNormalizationConfig& config) {
    if (config.left_stick_deadzone >= 32'767u ||
        config.right_stick_deadzone >= 32'767u ||
        config.trigger_deadzone >= 65'535u) {
        throw std::invalid_argument("Controller-Deadzone laesst keinen nutzbaren Wertebereich.");
    }
}

std::int32_t apply_axis_deadzone(const std::int16_t value, const std::uint16_t deadzone) noexcept {
    if (value == 0) return 0;
    const auto negative = value < 0;
    const auto magnitude =
        negative ? -static_cast<std::int32_t>(value) : static_cast<std::int32_t>(value);
    if (magnitude <= static_cast<std::int32_t>(deadzone)) return 0;
    const auto maximum = negative ? 32'768 : 32'767;
    const auto scaled =
        (static_cast<std::int64_t>(magnitude - deadzone) * maximum) / (maximum - deadzone);
    return negative ? -static_cast<std::int32_t>(scaled) : static_cast<std::int32_t>(scaled);
}

std::uint8_t controller_axis(const std::int16_t value, const std::uint16_t deadzone) noexcept {
    const auto normalized = apply_axis_deadzone(value, deadzone);
    if (normalized >= 0) {
        return static_cast<std::uint8_t>(
            0x80u + (static_cast<std::uint32_t>(normalized) * 0x7Fu) / 32'767u);
    }
    return static_cast<std::uint8_t>(
        0x80u - (static_cast<std::uint32_t>(-normalized) * 0x80u) / 32'768u);
}

std::uint8_t controller_trigger(const std::uint16_t value,
                                const std::uint16_t deadzone) noexcept {
    if (value <= deadzone) return 0u;
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(value - deadzone) * 0xFFu) / (65'535u - deadzone));
}

bool pressed(const HostControllerButtonMask buttons, const HostControllerButton button) noexcept {
    return (buttons & host_controller_button(button)) != 0u;
}

void press(ControllerState& state, const ControllerButton button) noexcept {
    state.pressed_buttons |= static_cast<std::uint16_t>(button);
}

bool neutral(const ControllerState& state) noexcept {
    return state == ControllerState{};
}

ControllerState merge_states(ControllerState controller, const ControllerState& keyboard) noexcept {
    controller.pressed_buttons |= keyboard.pressed_buttons;
    controller.left_trigger = std::max(controller.left_trigger, keyboard.left_trigger);
    controller.right_trigger = std::max(controller.right_trigger, keyboard.right_trigger);
    return controller;
}

std::uint8_t kind_priority(const HostControllerKind kind) noexcept {
    switch (kind) {
    case HostControllerKind::XInput:
        return 0u;
    case HostControllerKind::DualSense:
        return 1u;
    case HostControllerKind::DualShock:
        return 2u;
    case HostControllerKind::StandardHid:
        return 3u;
    case HostControllerKind::Keyboard:
        return 4u;
    case HostControllerKind::None:
        return 5u;
    }
    return 5u;
}

} // namespace

ControllerState normalize_host_controller(const HostControllerSample& sample,
                                          const ControllerNormalizationConfig& config) {
    validate_config(config);
    if (!sample.connected) return {};
    if (sample.device_id == 0u || !known_kind(sample.kind) ||
        sample.kind == HostControllerKind::None || sample.kind == HostControllerKind::Keyboard ||
        (sample.buttons & ~known_host_buttons) != 0u) {
        throw std::invalid_argument("Hostcontroller-Sample verletzt den normalisierten Vertrag.");
    }

    ControllerState result;
    if (pressed(sample.buttons, HostControllerButton::South)) press(result, ControllerButton::A);
    if (pressed(sample.buttons, HostControllerButton::East)) press(result, ControllerButton::B);
    if (pressed(sample.buttons, HostControllerButton::West)) press(result, ControllerButton::X);
    if (pressed(sample.buttons, HostControllerButton::North)) press(result, ControllerButton::Y);
    if (pressed(sample.buttons, HostControllerButton::Start)) press(result, ControllerButton::Start);
    if (pressed(sample.buttons, HostControllerButton::Back)) press(result, ControllerButton::D);
    if (pressed(sample.buttons, HostControllerButton::DpadUp))
        press(result, ControllerButton::DpadUp);
    if (pressed(sample.buttons, HostControllerButton::DpadDown))
        press(result, ControllerButton::DpadDown);
    if (pressed(sample.buttons, HostControllerButton::DpadLeft))
        press(result, ControllerButton::DpadLeft);
    if (pressed(sample.buttons, HostControllerButton::DpadRight))
        press(result, ControllerButton::DpadRight);
    if (pressed(sample.buttons, HostControllerButton::LeftStick))
        press(result, ControllerButton::C);
    if (pressed(sample.buttons, HostControllerButton::RightStick))
        press(result, ControllerButton::Z);

    result.left_trigger = controller_trigger(sample.left_trigger, config.trigger_deadzone);
    result.right_trigger = controller_trigger(sample.right_trigger, config.trigger_deadzone);
    if (pressed(sample.buttons, HostControllerButton::LeftShoulder))
        result.left_trigger = 0xFFu;
    if (pressed(sample.buttons, HostControllerButton::RightShoulder))
        result.right_trigger = 0xFFu;
    result.joystick_x = controller_axis(sample.left_x, config.left_stick_deadzone);
    result.joystick_y = controller_axis(sample.left_y, config.left_stick_deadzone);
    result.joystick2_x = controller_axis(sample.right_x, config.right_stick_deadzone);
    result.joystick2_y = controller_axis(sample.right_y, config.right_stick_deadzone);
    return result;
}

ControllerInputTimeline::ControllerInputTimeline(const ControllerNormalizationConfig config)
    : config_(config) {
    validate_config(config_);
}

std::optional<ControllerInputChange>
ControllerInputTimeline::poll(HostGamepadSource& source, const std::uint64_t guest_cycle) {
    auto devices = source.poll();
    devices.erase(std::remove_if(devices.begin(),
                                 devices.end(),
                                 [](const auto& sample) { return !sample.connected; }),
                  devices.end());
    for (const auto& sample : devices) {
        if (sample.device_id == 0u || !known_kind(sample.kind) ||
            sample.kind == HostControllerKind::None ||
            sample.kind == HostControllerKind::Keyboard ||
            (sample.buttons & ~known_host_buttons) != 0u) {
            throw std::invalid_argument("Gamepadquelle lieferte einen ungueltigen Sample.");
        }
    }
    std::sort(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
        return left.device_id < right.device_id;
    });
    if (std::adjacent_find(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
            return left.device_id == right.device_id;
        }) != devices.end()) {
        throw std::invalid_argument("Gamepadquelle lieferte eine doppelte Geraeteidentitaet.");
    }

    std::scoped_lock lock(mutex_);
    validate_cycle_locked(guest_cycle);
    if (!focused_) return std::nullopt;
    const auto old_active = active_device_id_;
    devices_ = std::move(devices);
    choose_active_locked();
    auto change = ControllerInputChangeKind::State;
    if (!old_active && active_device_id_)
        change = ControllerInputChangeKind::Connected;
    else if (old_active && !active_device_id_)
        change = ControllerInputChangeKind::Disconnected;
    else if (old_active != active_device_id_)
        change = ControllerInputChangeKind::Selection;
    return update_locked(guest_cycle, change);
}

std::optional<ControllerInputChange>
ControllerInputTimeline::keyboard_event(const KeyboardControllerKey key,
                                        const bool pressed_value,
                                        const std::uint64_t guest_cycle) {
    if (key >= KeyboardControllerKey::Count)
        throw std::invalid_argument("Unbekannte Keyboard-Controller-Taste.");
    std::scoped_lock lock(mutex_);
    validate_cycle_locked(guest_cycle);
    const auto index = static_cast<std::size_t>(key);
    const auto effective_pressed = focused_ && pressed_value;
    if (keyboard_[index] == effective_pressed) return std::nullopt;
    keyboard_[index] = effective_pressed;
    return update_locked(guest_cycle, ControllerInputChangeKind::State);
}

std::optional<ControllerInputChange>
ControllerInputTimeline::set_focus(const bool focused_value, const std::uint64_t guest_cycle) {
    std::scoped_lock lock(mutex_);
    validate_cycle_locked(guest_cycle);
    if (focused_ == focused_value) return std::nullopt;
    focused_ = focused_value;
    if (focused_) return std::nullopt;
    keyboard_.fill(false);
    devices_.clear();
    active_device_id_.reset();
    return update_locked(guest_cycle, ControllerInputChangeKind::FocusReset);
}

std::optional<ControllerInputChange> ControllerInputTimeline::select_controller(
    const std::optional<std::uint64_t> device_id, const std::uint64_t guest_cycle) {
    if (device_id == 0u)
        throw std::invalid_argument("Geraeteidentitaet null ist fuer Keyboard reserviert.");
    std::scoped_lock lock(mutex_);
    validate_cycle_locked(guest_cycle);
    if (selected_device_id_ == device_id) return std::nullopt;
    selected_device_id_ = device_id;
    if (!focused_) return std::nullopt;
    choose_active_locked();
    return update_locked(guest_cycle, ControllerInputChangeKind::Selection);
}

ControllerState ControllerInputTimeline::sample(const std::uint64_t) {
    std::scoped_lock lock(mutex_);
    ++sampled_frames_;
    return visible_state_;
}

ControllerState ControllerInputTimeline::sample_at(const std::uint64_t,
                                                   const std::uint64_t guest_cycle) {
    std::scoped_lock lock(mutex_);
    ++sampled_frames_;
    const auto iterator =
        std::upper_bound(trace_.begin(),
                         trace_.end(),
                         guest_cycle,
                         [](const auto cycle, const auto& event) {
                             return cycle < event.guest_cycle;
                         });
    return iterator == trace_.begin() ? ControllerState{} : std::prev(iterator)->state;
}

std::vector<ControllerInputChange> ControllerInputTimeline::trace() const {
    std::scoped_lock lock(mutex_);
    return trace_;
}

std::optional<std::uint64_t> ControllerInputTimeline::active_device_id() const {
    std::scoped_lock lock(mutex_);
    return active_device_id_;
}

std::optional<std::uint64_t> ControllerInputTimeline::selected_device_id() const {
    std::scoped_lock lock(mutex_);
    return selected_device_id_;
}

bool ControllerInputTimeline::focused() const {
    std::scoped_lock lock(mutex_);
    return focused_;
}

std::uint64_t ControllerInputTimeline::sampled_frames() const {
    std::scoped_lock lock(mutex_);
    return sampled_frames_;
}

std::optional<ControllerInputChange>
ControllerInputTimeline::update_locked(const std::uint64_t guest_cycle,
                                       const ControllerInputChangeKind requested_change) {
    const auto old_kind = visible_kind_;
    const auto old_device_id = visible_device_id_;
    const auto old_connected = visible_connected_;

    const auto state = combined_state_locked();
    auto kind = HostControllerKind::None;
    auto device_id = std::uint64_t{0u};
    auto connected = false;
    if (active_device_id_) {
        const auto iterator =
            std::find_if(devices_.begin(), devices_.end(), [&](const auto& sample) {
                return sample.device_id == *active_device_id_;
            });
        if (iterator != devices_.end()) {
            kind = iterator->kind;
            device_id = iterator->device_id;
            connected = true;
        }
    } else if (!neutral(keyboard_state_locked())) {
        kind = HostControllerKind::Keyboard;
        connected = true;
    }

    if (visible_state_ == state && visible_kind_ == kind && visible_device_id_ == device_id &&
        visible_connected_ == connected) {
        return std::nullopt;
    }

    auto event_kind = requested_change;
    if (event_kind != ControllerInputChangeKind::FocusReset) {
        if (!old_connected && connected)
            event_kind = ControllerInputChangeKind::Connected;
        else if (old_connected && !connected)
            event_kind = ControllerInputChangeKind::Disconnected;
        else if (old_device_id != device_id || old_kind != kind)
            event_kind = ControllerInputChangeKind::Selection;
        else
            event_kind = ControllerInputChangeKind::State;
    }

    ControllerInputChange event{
        next_sequence_++, guest_cycle, event_kind, kind, device_id, connected, state};
    if ((event_kind == ControllerInputChangeKind::Disconnected ||
         event_kind == ControllerInputChangeKind::FocusReset) &&
        !connected) {
        event.controller_kind = old_kind;
        event.device_id = old_device_id;
    }
    trace_.push_back(event);
    visible_state_ = state;
    visible_kind_ = kind;
    visible_device_id_ = device_id;
    visible_connected_ = connected;
    return event;
}

void ControllerInputTimeline::validate_cycle_locked(const std::uint64_t guest_cycle) {
    if (have_guest_cycle_ && guest_cycle < last_guest_cycle_)
        throw std::invalid_argument("Controllerereignis besitzt eine Gastzyklusregression.");
    last_guest_cycle_ = guest_cycle;
    have_guest_cycle_ = true;
}

void ControllerInputTimeline::choose_active_locked() {
    const auto contains = [&](const std::uint64_t id) {
        return std::any_of(devices_.begin(), devices_.end(), [&](const auto& sample) {
            return sample.device_id == id;
        });
    };
    if (selected_device_id_) {
        active_device_id_ =
            contains(*selected_device_id_) ? selected_device_id_ : std::nullopt;
        return;
    }
    if (active_device_id_ && contains(*active_device_id_)) return;
    if (devices_.empty()) {
        active_device_id_.reset();
        return;
    }
    const auto selected = std::min_element(devices_.begin(), devices_.end(), [](const auto& left,
                                                                                const auto& right) {
        return std::tuple{kind_priority(left.kind), left.device_id} <
               std::tuple{kind_priority(right.kind), right.device_id};
    });
    active_device_id_ = selected->device_id;
}

ControllerState ControllerInputTimeline::keyboard_state_locked() const noexcept {
    const auto held = [&](const KeyboardControllerKey key) {
        return keyboard_[static_cast<std::size_t>(key)];
    };
    ControllerState result;
    if (held(KeyboardControllerKey::Start)) press(result, ControllerButton::Start);
    if (held(KeyboardControllerKey::A)) press(result, ControllerButton::A);
    if (held(KeyboardControllerKey::B)) press(result, ControllerButton::B);
    if (held(KeyboardControllerKey::X)) press(result, ControllerButton::X);
    if (held(KeyboardControllerKey::Y)) press(result, ControllerButton::Y);
    if (held(KeyboardControllerKey::DpadUp)) press(result, ControllerButton::DpadUp);
    if (held(KeyboardControllerKey::DpadDown)) press(result, ControllerButton::DpadDown);
    if (held(KeyboardControllerKey::DpadLeft)) press(result, ControllerButton::DpadLeft);
    if (held(KeyboardControllerKey::DpadRight)) press(result, ControllerButton::DpadRight);
    if (held(KeyboardControllerKey::LeftTrigger)) result.left_trigger = 0xFFu;
    if (held(KeyboardControllerKey::RightTrigger)) result.right_trigger = 0xFFu;
    return result;
}

ControllerState ControllerInputTimeline::combined_state_locked() const {
    auto result = ControllerState{};
    if (active_device_id_) {
        const auto iterator =
            std::find_if(devices_.begin(), devices_.end(), [&](const auto& sample) {
                return sample.device_id == *active_device_id_;
            });
        if (iterator != devices_.end()) result = normalize_host_controller(*iterator, config_);
    }
    return merge_states(result, keyboard_state_locked());
}

ControllerInputReplay::ControllerInputReplay(std::vector<ControllerInputChange> trace)
    : trace_(std::move(trace)) {
    auto previous_cycle = std::uint64_t{0u};
    for (std::size_t index = 0u; index < trace_.size(); ++index) {
        const auto& event = trace_[index];
        if (event.sequence != index + 1u || (index != 0u && event.guest_cycle < previous_cycle) ||
            !known_change(event.change) || !known_kind(event.controller_kind) ||
            (event.connected && event.controller_kind == HostControllerKind::None) ||
            (event.device_id == 0u && event.controller_kind != HostControllerKind::None &&
             event.controller_kind != HostControllerKind::Keyboard)) {
            throw std::invalid_argument("Controller-Replayspur verletzt den Ereignisvertrag.");
        }
        previous_cycle = event.guest_cycle;
    }
}

ControllerState ControllerInputReplay::sample(const std::uint64_t frame) {
    return sample_at(frame, frame);
}

ControllerState ControllerInputReplay::sample_at(const std::uint64_t,
                                                 const std::uint64_t guest_cycle) {
    ++sampled_frames_;
    const auto iterator =
        std::upper_bound(trace_.begin(),
                         trace_.end(),
                         guest_cycle,
                         [](const auto cycle, const auto& event) {
                             return cycle < event.guest_cycle;
                         });
    return iterator == trace_.begin() ? ControllerState{} : std::prev(iterator)->state;
}

std::uint64_t ControllerInputReplay::sampled_frames() const noexcept {
    return sampled_frames_;
}

const std::vector<ControllerInputChange>& ControllerInputReplay::trace() const noexcept {
    return trace_;
}

const char* host_controller_kind_name(const HostControllerKind kind) noexcept {
    switch (kind) {
    case HostControllerKind::None:
        return "none";
    case HostControllerKind::Keyboard:
        return "keyboard";
    case HostControllerKind::XInput:
        return "xinput";
    case HostControllerKind::DualSense:
        return "dualsense";
    case HostControllerKind::DualShock:
        return "dualshock";
    case HostControllerKind::StandardHid:
        return "standard-hid";
    }
    return "none";
}

#ifdef _WIN32
namespace {

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr std::uint64_t xinput_device_domain = 0x0100000000000000ull;
constexpr std::uint64_t joystick_device_domain = 0x0200000000000000ull;

std::int16_t invert_axis(const SHORT value) noexcept {
    const auto inverted = -static_cast<std::int32_t>(value);
    return static_cast<std::int16_t>(
        std::clamp(inverted,
                   static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
}

std::int16_t joystick_axis(const DWORD value, const UINT minimum, const UINT maximum) noexcept {
    if (maximum <= minimum) return 0;
    const auto clamped = std::clamp<std::uint64_t>(value, minimum, maximum) - minimum;
    const auto range = static_cast<std::uint64_t>(maximum) - minimum;
    const auto scaled = (clamped * 65'535u) / range;
    return static_cast<std::int16_t>(
        static_cast<std::int32_t>(scaled) - static_cast<std::int32_t>(32'768u));
}

HostControllerKind joystick_kind(const JOYCAPSW& capabilities) noexcept {
    constexpr WORD sony_vendor = 0x054Cu;
    if (capabilities.wMid != sony_vendor) return HostControllerKind::StandardHid;
    switch (capabilities.wPid) {
    case 0x0CE6u:
    case 0x0DF2u:
        return HostControllerKind::DualSense;
    case 0x05C4u:
    case 0x09CCu:
    case 0x0BA0u:
        return HostControllerKind::DualShock;
    default:
        return HostControllerKind::StandardHid;
    }
}

void map_pov(HostControllerButtonMask& buttons, const DWORD pov) noexcept {
    if (pov == JOY_POVCENTERED || pov > 35'999u) return;
    if (pov >= 31'500u || pov < 4'500u)
        buttons |= host_controller_button(HostControllerButton::DpadUp);
    if (pov >= 4'500u && pov < 13'500u)
        buttons |= host_controller_button(HostControllerButton::DpadRight);
    if (pov >= 13'500u && pov < 22'500u)
        buttons |= host_controller_button(HostControllerButton::DpadDown);
    if (pov >= 22'500u && pov < 31'500u)
        buttons |= host_controller_button(HostControllerButton::DpadLeft);
}

void map_joystick_buttons(HostControllerSample& sample, const DWORD buttons) noexcept {
    const auto button = [&](const unsigned index) {
        return (buttons & (DWORD{1u} << index)) != 0u;
    };
    const auto sony = sample.kind == HostControllerKind::DualSense ||
                      sample.kind == HostControllerKind::DualShock;
    const auto add = [&](const bool set, const HostControllerButton target) {
        if (set) sample.buttons |= host_controller_button(target);
    };
    if (sony) {
        add(button(0u), HostControllerButton::West);
        add(button(1u), HostControllerButton::South);
        add(button(2u), HostControllerButton::East);
        add(button(3u), HostControllerButton::North);
        add(button(8u), HostControllerButton::Back);
        add(button(9u), HostControllerButton::Start);
    } else {
        add(button(0u), HostControllerButton::South);
        add(button(1u), HostControllerButton::East);
        add(button(2u), HostControllerButton::West);
        add(button(3u), HostControllerButton::North);
        add(button(6u), HostControllerButton::Back);
        add(button(7u), HostControllerButton::Start);
    }
    add(button(4u), HostControllerButton::LeftShoulder);
    add(button(5u), HostControllerButton::RightShoulder);
    add(button(10u), HostControllerButton::LeftStick);
    add(button(11u), HostControllerButton::RightStick);
}

class Win32GamepadSource final : public HostGamepadSource {
  public:
    Win32GamepadSource() {
        constexpr std::array libraries = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const auto* library : libraries) {
            xinput_library_ = LoadLibraryW(library);
            if (xinput_library_ == nullptr) continue;
            xinput_get_state_ = reinterpret_cast<XInputGetStateFunction>(
                GetProcAddress(xinput_library_, "XInputGetState"));
            if (xinput_get_state_ != nullptr) break;
            FreeLibrary(xinput_library_);
            xinput_library_ = nullptr;
        }
    }

    ~Win32GamepadSource() override {
        if (xinput_library_ != nullptr) FreeLibrary(xinput_library_);
    }

    [[nodiscard]] std::vector<HostControllerSample> poll() override {
        std::vector<HostControllerSample> result;
        poll_xinput(result);
        poll_joysticks(result);
        return result;
    }

  private:
    void poll_xinput(std::vector<HostControllerSample>& result) const {
        if (xinput_get_state_ == nullptr) return;
        for (DWORD slot = 0u; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state{};
            if (xinput_get_state_(slot, &state) != ERROR_SUCCESS) continue;
            HostControllerSample sample;
            sample.device_id = xinput_device_domain | (static_cast<std::uint64_t>(slot) + 1u);
            sample.kind = HostControllerKind::XInput;
            sample.connected = true;
            const auto add = [&](const WORD source, const HostControllerButton target) {
                if ((state.Gamepad.wButtons & source) != 0u)
                    sample.buttons |= host_controller_button(target);
            };
            add(XINPUT_GAMEPAD_A, HostControllerButton::South);
            add(XINPUT_GAMEPAD_B, HostControllerButton::East);
            add(XINPUT_GAMEPAD_X, HostControllerButton::West);
            add(XINPUT_GAMEPAD_Y, HostControllerButton::North);
            add(XINPUT_GAMEPAD_START, HostControllerButton::Start);
            add(XINPUT_GAMEPAD_BACK, HostControllerButton::Back);
            add(XINPUT_GAMEPAD_DPAD_UP, HostControllerButton::DpadUp);
            add(XINPUT_GAMEPAD_DPAD_DOWN, HostControllerButton::DpadDown);
            add(XINPUT_GAMEPAD_DPAD_LEFT, HostControllerButton::DpadLeft);
            add(XINPUT_GAMEPAD_DPAD_RIGHT, HostControllerButton::DpadRight);
            add(XINPUT_GAMEPAD_LEFT_SHOULDER, HostControllerButton::LeftShoulder);
            add(XINPUT_GAMEPAD_RIGHT_SHOULDER, HostControllerButton::RightShoulder);
            add(XINPUT_GAMEPAD_LEFT_THUMB, HostControllerButton::LeftStick);
            add(XINPUT_GAMEPAD_RIGHT_THUMB, HostControllerButton::RightStick);
            sample.left_x = state.Gamepad.sThumbLX;
            sample.left_y = invert_axis(state.Gamepad.sThumbLY);
            sample.right_x = state.Gamepad.sThumbRX;
            sample.right_y = invert_axis(state.Gamepad.sThumbRY);
            sample.left_trigger =
                static_cast<std::uint16_t>(state.Gamepad.bLeftTrigger) * 257u;
            sample.right_trigger =
                static_cast<std::uint16_t>(state.Gamepad.bRightTrigger) * 257u;
            result.push_back(sample);
        }
    }

    static void poll_joysticks(std::vector<HostControllerSample>& result) {
        const auto count = joyGetNumDevs();
        for (UINT slot = 0u; slot < count; ++slot) {
            JOYCAPSW capabilities{};
            if (joyGetDevCapsW(slot, &capabilities, sizeof(capabilities)) != JOYERR_NOERROR)
                continue;
            JOYINFOEX info{};
            info.dwSize = sizeof(info);
            info.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(slot, &info) != JOYERR_NOERROR) continue;

            HostControllerSample sample;
            sample.device_id = joystick_device_domain | (static_cast<std::uint64_t>(slot) + 1u);
            sample.kind = joystick_kind(capabilities);
            sample.connected = true;
            map_joystick_buttons(sample, info.dwButtons);
            map_pov(sample.buttons, info.dwPOV);
            sample.left_x =
                joystick_axis(info.dwXpos, capabilities.wXmin, capabilities.wXmax);
            sample.left_y =
                joystick_axis(info.dwYpos, capabilities.wYmin, capabilities.wYmax);
            if ((capabilities.wCaps & JOYCAPS_HASR) != 0u)
                sample.right_x =
                    joystick_axis(info.dwRpos, capabilities.wRmin, capabilities.wRmax);
            if ((capabilities.wCaps & JOYCAPS_HASU) != 0u)
                sample.right_y =
                    joystick_axis(info.dwUpos, capabilities.wUmin, capabilities.wUmax);
            else if ((capabilities.wCaps & JOYCAPS_HASV) != 0u)
                sample.right_y =
                    joystick_axis(info.dwVpos, capabilities.wVmin, capabilities.wVmax);
            if ((capabilities.wCaps & JOYCAPS_HASZ) != 0u &&
                capabilities.wZmax > capabilities.wZmin) {
                const auto axis =
                    joystick_axis(info.dwZpos, capabilities.wZmin, capabilities.wZmax);
                if (axis < 0) {
                    const auto magnitude =
                        static_cast<std::uint32_t>(-static_cast<std::int32_t>(axis));
                    sample.left_trigger =
                        static_cast<std::uint16_t>(std::min(magnitude * 2u, 65'535u));
                } else {
                    sample.right_trigger = static_cast<std::uint16_t>(
                        std::min(static_cast<std::uint32_t>(axis) * 2u, 65'535u));
                }
            }
            result.push_back(sample);
        }
    }

    HMODULE xinput_library_ = nullptr;
    XInputGetStateFunction xinput_get_state_ = nullptr;
};

} // namespace

bool native_gamepad_input_available() noexcept {
    return true;
}

std::unique_ptr<HostGamepadSource> create_native_gamepad_source() {
    return std::make_unique<Win32GamepadSource>();
}

#else

bool native_gamepad_input_available() noexcept {
    return false;
}

std::unique_ptr<HostGamepadSource> create_native_gamepad_source() {
    throw std::runtime_error("Natives Gamepad-Input ist auf diesem Host nicht verfuegbar.");
}

#endif

} // namespace katana::runtime
