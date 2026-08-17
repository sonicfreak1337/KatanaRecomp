#include "katana/runtime/host_input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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

    ControllerInputRecording recording(8u);
    const auto recording_state_a = normalized_profile.value();
    const std::array slots_a{recording_state_a, ControllerState{}, ControllerState{}, ControllerState{}};
    const auto recording_state_b = ControllerState{};
    const std::array slots_b{recording_state_b, normalized_profile.value(), ControllerState{}, ControllerState{}};
    recording.append(40u, slots_a);
    recording.append(41u, slots_b);
    require(recording.frame_count() == 2u &&
                recording.sample_slot(40u, 0u) == recording_state_a &&
                recording.sample_slot(41u, 1u) == normalized_profile.value() &&
                recording.sample(39u) == ControllerState{} &&
                recording.sample_slot(42u, 0u) == ControllerState{},
            "Gebundene Mehrslot-Aufzeichnung ist nicht frame-indexiert.");
    require(throws<std::invalid_argument>([&] { recording.append(43u, slots_a); }),
            "Mehrslot-Aufzeichnung akzeptiert keinen Frame-Sprung.");
    const auto recording_path =
        std::filesystem::temp_directory_path() / "katana-controller-input-test.kin1";
    recording.save(recording_path, "test-input-identity");
    auto loaded_recording =
        ControllerInputRecording::load(recording_path, "test-input-identity", 8u);
    require(loaded_recording.frames() == recording.frames() &&
                loaded_recording.sample_slot(41u, 1u) == normalized_profile.value() &&
                throws<std::runtime_error>([&] {
                    static_cast<void>(ControllerInputRecording::load(
                        recording_path, "wrong-input-identity", 8u));
                }),
            "Aufzeichnung validiert Identitaet oder Serialisierung nicht fail-closed.");
    std::error_code recording_cleanup_error;
    std::filesystem::remove(recording_path, recording_cleanup_error);

    FakeGamepadSource concurrent_source;
    auto concurrent_a = sample(300u, HostControllerKind::XInput);
    concurrent_a.buttons = host_controller_button(HostControllerButton::South);
    concurrent_a.left_x = std::numeric_limits<std::int16_t>::max();
    concurrent_a.left_trigger = std::numeric_limits<std::uint16_t>::max();
    auto concurrent_b = sample(300u, HostControllerKind::XInput);
    concurrent_b.buttons = host_controller_button(HostControllerButton::East);
    concurrent_b.left_x = std::numeric_limits<std::int16_t>::min();
    concurrent_b.right_trigger = std::numeric_limits<std::uint16_t>::max();
    const auto concurrent_state_a =
        normalize_host_controller(concurrent_a, {0u, 0u, 0u});
    const auto concurrent_state_b =
        normalize_host_controller(concurrent_b, {0u, 0u, 0u});
    const auto condition_words = [](const ControllerState& state) {
        return std::array{
            static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                ~state.pressed_buttons)) |
                (static_cast<std::uint32_t>(state.right_trigger) << 16u) |
                (static_cast<std::uint32_t>(state.left_trigger) << 24u),
            static_cast<std::uint32_t>(state.joystick_x) |
                (static_cast<std::uint32_t>(state.joystick_y) << 8u) |
                (static_cast<std::uint32_t>(state.joystick2_x) << 16u) |
                (static_cast<std::uint32_t>(state.joystick2_y) << 24u)};
    };
    const auto condition_a = condition_words(concurrent_state_a);
    const auto condition_b = condition_words(concurrent_state_b);
    concurrent_source.devices = {concurrent_a};
    auto concurrent_timeline =
        std::make_shared<ControllerInputTimeline>(
            ControllerNormalizationConfig{0u, 0u, 0u});
    require(concurrent_timeline->poll(concurrent_source, 0u).has_value(),
            "Race-Regression kann den ersten Controllerzustand nicht setzen.");
    MapleBus concurrent_bus;
    concurrent_bus.attach(
        0u, 0u, std::make_shared<MapleControllerDevice>(concurrent_timeline));
    std::atomic_bool concurrent_start = false;
    std::atomic_bool torn_state = false;
    std::thread maple_reader([&] {
        while (!concurrent_start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (std::size_t sample_index = 0u; sample_index < 4'096u; ++sample_index) {
            const auto response = concurrent_bus.exchange_at(
                0u,
                0u,
                {MapleCommand::GetCondition, {}},
                std::numeric_limits<std::uint64_t>::max());
            const auto complete_a = response.payload.size() == 3u &&
                                    response.payload[1] == condition_a[0] &&
                                    response.payload[2] == condition_a[1];
            const auto complete_b = response.payload.size() == 3u &&
                                    response.payload[1] == condition_b[0] &&
                                    response.payload[2] == condition_b[1];
            if (!complete_a && !complete_b) {
                torn_state.store(true, std::memory_order_release);
                return;
            }
            if ((sample_index & 7u) == 0u) std::this_thread::yield();
        }
    });
    concurrent_start.store(true, std::memory_order_release);
    for (std::uint64_t cycle = 1u; cycle <= 2'048u; ++cycle) {
        concurrent_source.devices = {(cycle & 1u) == 0u ? concurrent_a : concurrent_b};
        require(concurrent_timeline->poll(concurrent_source, cycle).has_value(),
                "Alternierender Race-Sample wird als unveraendert verworfen.");
        if ((cycle & 7u) == 0u) std::this_thread::yield();
    }
    maple_reader.join();
    require(!torn_state.load(std::memory_order_acquire) &&
                concurrent_timeline->trace().size() == 2'049u,
            "Gleichzeitiger Hostpoll und Maple-Read sieht einen zerrissenen Zustand.");

#ifdef _WIN32
    auto native_source = create_native_gamepad_source();
    require(native_gamepad_input_available() && native_source != nullptr,
            "Win32 stellt kein hotplugfaehiges XInput-/HID-Backend bereit.");
    const auto native_samples = native_source->poll();
    const auto native_samples_valid =
        std::all_of(native_samples.begin(), native_samples.end(), [](const auto& native_sample) {
            if (!native_sample.connected || native_sample.device_id == 0u) return false;
            try {
                static_cast<void>(normalize_host_controller(native_sample));
                return true;
            } catch (...) {
                return false;
            }
        });
    require(native_samples_valid,
            "Win32-Gamepadbackend liefert keinen normalisierbaren Samplevertrag.");
#else
    require(!native_gamepad_input_available() &&
                throws<std::runtime_error>(
                    [] { static_cast<void>(create_native_gamepad_source()); }),
            "Nicht implementiertes natives Gamepadbackend wird behauptet.");
#endif

    std::cout << "KR-4814 moderner Controllerkern und gastzeitgebundenes Replay erfolgreich.\n";
}
