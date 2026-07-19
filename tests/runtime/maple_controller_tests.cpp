#include "katana/runtime/maple.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
} // namespace

int main() {
    using namespace katana::runtime;
    const auto bit = [](const ControllerButton button) {
        return static_cast<std::uint16_t>(button);
    };
    std::vector<ControllerState> frames(2u);
    frames[0].pressed_buttons =
        static_cast<std::uint16_t>(bit(ControllerButton::A) | bit(ControllerButton::Start));
    frames[0].right_trigger = 0x40u;
    frames[0].left_trigger = 0x80u;
    frames[0].joystick_x = 0x20u;
    frames[0].joystick_y = 0xE0u;
    frames[1].pressed_buttons = bit(ControllerButton::DpadLeft);

    auto replay = std::make_shared<ReplayInputBackend>(frames);
    auto controller = std::make_shared<MapleControllerDevice>(replay);
    MapleBus bus;
    bus.attach(0u, 0u, controller);

    const auto info = bus.exchange(0u, 0u, {MapleCommand::DeviceRequest, {}});
    require(info.code == MapleResponseCode::DeviceInfo && info.payload.size() == 28u &&
                info.payload[0] == 0x01000000u && info.payload[1] == 0xFE060F00u,
            "Controller meldet keinen vollstaendigen 28-Wort-Device-Info-Vertrag.");
    const auto first = bus.exchange(0u, 0u, {MapleCommand::GetCondition, {0x01000000u}});
    require(first.code == MapleResponseCode::DataTransfer && first.payload.size() == 3u,
            "Controller liefert keinen vollstaendigen Condition-Frame.");
    require((first.payload[1] & bit(ControllerButton::A)) == 0u &&
                (first.payload[1] & bit(ControllerButton::Start)) == 0u &&
                ((first.payload[1] >> 16u) & 0xFFu) == 0x40u &&
                ((first.payload[1] >> 24u) & 0xFFu) == 0x80u,
            "Tasten- oder Triggerzustand ist nicht Dreamcast-aktiv-low kodiert.");
    require(first.payload[2] == 0x8080E020u, "Analogachsen gehen im Maple-Frame verloren.");
    const auto second = bus.exchange(0u, 0u, {MapleCommand::GetCondition, {}});
    require((second.payload[1] & bit(ControllerButton::DpadLeft)) == 0u &&
                controller->sampled_frames() == 2u,
            "Deterministisches Replay schreitet nicht frameweise fort.");
    require(throws<std::out_of_range>(
                [&] { static_cast<void>(bus.exchange(0u, 0u, {MapleCommand::GetCondition, {}})); }),
            "Ein zu kurzes Replay faellt still auf Hostzustand zurueck.");
    require(throws<std::invalid_argument>([] { static_cast<void>(ReplayInputBackend({})); }),
            "Leeres Input-Replay wird akzeptiert.");

    std::cout << "KR-2702 Controller und Input-Replay erfolgreich.\n";
}
