#include "katana/runtime/host_input.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Callback> bool throws(Callback&& callback) {
    try {
        callback();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

class FakeGamepadSource final : public katana::runtime::HostGamepadSource {
  public:
    [[nodiscard]] std::vector<katana::runtime::HostControllerSample> poll() override {
        return devices;
    }

    std::vector<katana::runtime::HostControllerSample> devices;
};

std::uint16_t button(const katana::runtime::ControllerButton value) {
    return static_cast<std::uint16_t>(value);
}

katana::runtime::HostControllerSample
sample(const std::uint64_t id, const katana::runtime::HostControllerKind kind) {
    return {id, kind, 0u, 0, 0, 0, 0, 0u, 0u, true};
}

} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(native_controller_contract_version == 1u);

    const auto semantic_buttons =
        host_controller_button(HostControllerButton::South) |
        host_controller_button(HostControllerButton::East) |
        host_controller_button(HostControllerButton::West) |
        host_controller_button(HostControllerButton::North) |
        host_controller_button(HostControllerButton::Start) |
        host_controller_button(HostControllerButton::DpadLeft);
    std::optional<ControllerState> normalized_profile;
    auto profile_id = std::uint64_t{1u};
    for (const auto kind : {HostControllerKind::XInput,
                            HostControllerKind::DualSense,
                            HostControllerKind::DualShock,
                            HostControllerKind::StandardHid}) {
        auto current = sample(profile_id++, kind);
        current.buttons = semantic_buttons;
        current.left_x = 32'767;
        current.left_y = std::numeric_limits<std::int16_t>::min();
        current.right_x = 1'000;
        current.right_y = -1'000;
        current.left_trigger = 65'535u;
        current.right_trigger = 7'710u;
        const auto state = normalize_host_controller(current);
        require((state.pressed_buttons & button(ControllerButton::A)) != 0u &&
                    (state.pressed_buttons & button(ControllerButton::B)) != 0u &&
                    (state.pressed_buttons & button(ControllerButton::X)) != 0u &&
                    (state.pressed_buttons & button(ControllerButton::Y)) != 0u &&
                    (state.pressed_buttons & button(ControllerButton::Start)) != 0u &&
                    (state.pressed_buttons & button(ControllerButton::DpadLeft)) != 0u &&
                    state.left_trigger == 0xFFu && state.right_trigger == 0u &&
                    state.joystick_x == 0xFFu && state.joystick_y == 0u &&
                    state.joystick2_x == 0x80u && state.joystick2_y == 0x80u,
                "Controllerprofil verletzt Button-, Deadzone-, Trigger- oder Achsenvertrag.");
        if (normalized_profile)
            require(*normalized_profile == state,
                    "Xbox-, Sony- und Standardprofile normalisieren nicht identisch.");
        else
            normalized_profile = state;
    }

    auto shoulder = sample(9u, HostControllerKind::StandardHid);
    shoulder.buttons = host_controller_button(HostControllerButton::LeftShoulder) |
                       host_controller_button(HostControllerButton::RightShoulder) |
                       host_controller_button(HostControllerButton::LeftStick) |
                       host_controller_button(HostControllerButton::RightStick) |
                       host_controller_button(HostControllerButton::Back);
    const auto shoulder_state = normalize_host_controller(shoulder);
    require(shoulder_state.left_trigger == 0xFFu && shoulder_state.right_trigger == 0xFFu &&
                (shoulder_state.pressed_buttons & button(ControllerButton::C)) != 0u &&
                (shoulder_state.pressed_buttons & button(ControllerButton::Z)) != 0u &&
                (shoulder_state.pressed_buttons & button(ControllerButton::D)) != 0u,
            "Moderne Schulter-, Stick- oder Backtasten besitzen keinen Dreamcast-Fallback.");
    require(throws<std::invalid_argument>([] {
                auto invalid = sample(1u, HostControllerKind::XInput);
                invalid.buttons = 1u << 31u;
                static_cast<void>(normalize_host_controller(invalid));
            }) &&
                throws<std::invalid_argument>([] {
                    static_cast<void>(ControllerInputTimeline(
                        ControllerNormalizationConfig{32'767u, 0u, 0u}));
                }),
            "Ungueltige Controllerbits oder geschlossene Deadzones werden akzeptiert.");

    FakeGamepadSource source;
    auto xbox = sample(10u, HostControllerKind::XInput);
    xbox.buttons = host_controller_button(HostControllerButton::South);
    xbox.left_x = 32'767;
    xbox.left_trigger = 32'768u;
    source.devices = {xbox};
    auto timeline =
        std::make_shared<ControllerInputTimeline>(ControllerNormalizationConfig{0u, 0u, 0u});
    const auto connected = timeline->poll(source, 10u);
    require(connected && connected->sequence == 1u && connected->guest_cycle == 10u &&
                connected->change == ControllerInputChangeKind::Connected &&
                connected->controller_kind == HostControllerKind::XInput &&
                connected->device_id == 10u && connected->connected,
            "Erster Hotplug wird nicht gastzyklusgestempelt und typisiert.");
    require(!timeline->poll(source, 11u) && timeline->trace().size() == 1u,
            "Unveraenderter Hostzustand wird erneut eingespeist.");
    const auto keyboard =
        timeline->keyboard_event(KeyboardControllerKey::X, true, 12u);
    require(keyboard && keyboard->sequence == 2u &&
                (keyboard->state.pressed_buttons & button(ControllerButton::X)) != 0u,
            "Keyboardfallback wird nicht ueber dieselbe Ereignisschnittstelle gemischt.");

    xbox.buttons = host_controller_button(HostControllerButton::East);
    source.devices = {xbox};
    const auto changed = timeline->poll(source, 20u);
    require(changed && changed->sequence == 3u &&
                changed->change == ControllerInputChangeKind::State,
            "Geaenderter Livezustand fehlt in der Ereignisspur.");
    require(timeline->sample_at(0u, 9u) == ControllerState{} &&
                (timeline->sample_at(1u, 10u).pressed_buttons &
                 button(ControllerButton::A)) != 0u &&
                (timeline->sample_at(2u, 15u).pressed_buttons &
                 (button(ControllerButton::A) | button(ControllerButton::X))) ==
                    (button(ControllerButton::A) | button(ControllerButton::X)) &&
                (timeline->sample_at(3u, 20u).pressed_buttons &
                 (button(ControllerButton::B) | button(ControllerButton::X))) ==
                    (button(ControllerButton::B) | button(ControllerButton::X)),
            "Timeline liefert nicht den letzten zum Gastzyklus sichtbaren Zustand.");

    auto live_controller = std::make_shared<MapleControllerDevice>(timeline);
    MapleBus live_bus;
    live_bus.attach(0u, 0u, live_controller);
    const auto live_at_15 =
        live_bus.exchange_at(0u, 0u, {MapleCommand::GetCondition, {}}, 15u);
    const auto live_at_20 =
        live_bus.exchange_at(0u, 0u, {MapleCommand::GetCondition, {}}, 20u);
    require(live_at_15.payload.size() == 3u && live_at_20.payload.size() == 3u &&
                (live_at_15.payload[1] & button(ControllerButton::A)) == 0u &&
                (live_at_15.payload[1] & button(ControllerButton::B)) != 0u &&
                (live_at_20.payload[1] & button(ControllerButton::A)) != 0u &&
                (live_at_20.payload[1] & button(ControllerButton::B)) == 0u,
            "Maple GetCondition bindet den Zustand nicht bitgenau an den Transaktionszyklus.");

    source.devices.clear();
    const auto fallback = timeline->poll(source, 30u);
    require(fallback && fallback->change == ControllerInputChangeKind::Selection &&
                fallback->controller_kind == HostControllerKind::Keyboard &&
                fallback->connected &&
                fallback->state.pressed_buttons == button(ControllerButton::X),
            "Controllerverlust wechselt nicht ohne haengende Gamepadtasten auf Keyboard.");
    const auto focus_reset = timeline->set_focus(false, 31u);
    require(focus_reset && focus_reset->change == ControllerInputChangeKind::FocusReset &&
                focus_reset->state == ControllerState{} && !timeline->focused() &&
                !timeline->keyboard_event(KeyboardControllerKey::A, true, 32u),
            "Fokusverlust leert nicht alle Eingabequellen oder nimmt Hintergrundinput an.");
    require(!timeline->set_focus(true, 33u) && timeline->focused(),
            "Fokusgewinn stellt den Pollvertrag nicht wieder her.");
    auto dualsense = sample(20u, HostControllerKind::DualSense);
    source.devices = {dualsense};
    const auto reconnected = timeline->poll(source, 34u);
    require(reconnected && reconnected->change == ControllerInputChangeKind::Connected &&
                reconnected->controller_kind == HostControllerKind::DualSense,
            "Hotplug nach Fokusgewinn wird nicht frisch erfasst.");

    FakeGamepadSource selection_source;
    auto selection_xbox = sample(100u, HostControllerKind::XInput);
    selection_xbox.buttons = host_controller_button(HostControllerButton::South);
    auto selection_sony = sample(200u, HostControllerKind::DualSense);
    selection_sony.buttons = host_controller_button(HostControllerButton::East);
    selection_source.devices = {selection_sony, selection_xbox};
    ControllerInputTimeline selection({0u, 0u, 0u});
    require(selection.poll(selection_source, 1u) &&
                selection.active_device_id() == std::optional<std::uint64_t>{100u},
            "Automatische Controller-1-Auswahl ist nicht stabil und priorisiert.");
    const auto selected = selection.select_controller(200u, 2u);
    require(selected && selected->change == ControllerInputChangeKind::Selection &&
                selection.active_device_id() == std::optional<std::uint64_t>{200u} &&
                (selected->state.pressed_buttons & button(ControllerButton::B)) != 0u,
            "Explizite Controller-1-Auswahl wird nicht angewendet.");
    selection_source.devices = {selection_xbox};
    const auto selected_removed = selection.poll(selection_source, 3u);
    require(selected_removed &&
                selected_removed->change == ControllerInputChangeKind::Disconnected &&
                !selection.active_device_id() && selected_removed->state == ControllerState{},
            "Entfernter ausgewaehlter Controller faellt still auf ein anderes Geraet zurueck.");
    const auto automatic = selection.select_controller(std::nullopt, 4u);
    require(automatic && automatic->change == ControllerInputChangeKind::Connected &&
                selection.active_device_id() == std::optional<std::uint64_t>{100u},
            "Rueckkehr zur automatischen Controller-1-Auswahl scheitert.");

    const auto trace = timeline->trace();
    auto replay = std::make_shared<ControllerInputReplay>(trace);
    auto replay_controller = std::make_shared<MapleControllerDevice>(replay);
    MapleBus replay_bus;
    replay_bus.attach(0u, 0u, replay_controller);
    const auto replay_at_15 =
        replay_bus.exchange_at(0u, 0u, {MapleCommand::GetCondition, {}}, 15u);
    const auto replay_at_20 =
        replay_bus.exchange_at(0u, 0u, {MapleCommand::GetCondition, {}}, 20u);
    require(replay_at_15.payload == live_at_15.payload &&
                replay_at_20.payload == live_at_20.payload &&
                replay->trace() == trace && replay->sampled_frames() == 2u,
            "Replay ueber dieselbe Ereignisspur ist nicht bytegleich zum Live-Maple-Pfad.");
    auto malformed_trace = trace;
    malformed_trace.front().sequence = 2u;
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(ControllerInputReplay(malformed_trace)); }) &&
                throws<std::invalid_argument>(
                    [&] { static_cast<void>(timeline->poll(source, 1u)); }),
            "Replaysequenz- oder Gastzyklusregression wird akzeptiert.");

#ifdef _WIN32
    require(native_gamepad_input_available() && create_native_gamepad_source() != nullptr,
            "Win32 stellt kein hotplugfaehiges XInput-/HID-Backend bereit.");
#else
    require(!native_gamepad_input_available() &&
                throws<std::runtime_error>(
                    [] { static_cast<void>(create_native_gamepad_source()); }),
            "Nicht implementiertes natives Gamepadbackend wird behauptet.");
#endif

    std::cout << "KR-4814 moderner Controllerkern und gastzeitgebundenes Replay erfolgreich.\n";
}
