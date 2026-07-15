#include "katana/runtime/maple.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {

ReplayInputBackend::ReplayInputBackend(std::vector<ControllerState> frames)
    : frames_(std::move(frames)) {
    if (frames_.empty()) {
        throw std::invalid_argument("Ein Input-Replay braucht mindestens einen Frame.");
    }
}

ControllerState ReplayInputBackend::sample(const std::uint64_t frame) {
    if (frame >= frames_.size()) {
        throw std::out_of_range("Input-Replay ist fuer den angeforderten Frame zu kurz.");
    }
    return frames_[static_cast<std::size_t>(frame)];
}

MapleControllerDevice::MapleControllerDevice(std::shared_ptr<HostInputBackend> input)
    : input_(std::move(input)) {
    if (!input_) {
        throw std::invalid_argument("Controller braucht ein Host-Input-Backend.");
    }
}

MapleResponse MapleControllerDevice::transact(const MapleRequest& request) {
    constexpr std::uint32_t controller_function = 0x01000000u;
    if (request.command == MapleCommand::DeviceRequest) {
        return {MapleResponseCode::DeviceInfo, {controller_function, 0u, 0u}};
    }
    if (request.command != MapleCommand::GetCondition) {
        return {MapleResponseCode::UnknownCommand, {}};
    }
    const auto state = input_->sample(next_frame_++);
    const auto buttons = static_cast<std::uint16_t>(~state.pressed_buttons);
    const std::uint32_t condition0 =
        static_cast<std::uint32_t>(buttons) |
        (static_cast<std::uint32_t>(state.right_trigger) << 16u) |
        (static_cast<std::uint32_t>(state.left_trigger) << 24u);
    const std::uint32_t condition1 =
        static_cast<std::uint32_t>(state.joystick_x) |
        (static_cast<std::uint32_t>(state.joystick_y) << 8u) |
        (static_cast<std::uint32_t>(state.joystick2_x) << 16u) |
        (static_cast<std::uint32_t>(state.joystick2_y) << 24u);
    return {MapleResponseCode::DataTransfer, {controller_function, condition0, condition1}};
}

std::uint64_t MapleControllerDevice::sampled_frames() const noexcept {
    return next_frame_;
}

std::size_t MapleBus::slot(const std::uint8_t port, const std::uint8_t unit) {
    if (port >= maple_port_count || unit >= maple_units_per_port) {
        throw std::out_of_range("Maple-Port oder -Unit liegt ausserhalb des Busses.");
    }
    return static_cast<std::size_t>(port) * maple_units_per_port + unit;
}

void MapleBus::attach(
    const std::uint8_t port,
    const std::uint8_t unit,
    std::shared_ptr<MapleDevice> device
) {
    if (!device) {
        throw std::invalid_argument("Ein Maple-Geraet darf nicht null sein.");
    }
    auto& target = devices_[slot(port, unit)];
    if (target) {
        throw std::invalid_argument("Maple-Port und -Unit sind bereits belegt.");
    }
    target = std::move(device);
}

bool MapleBus::attached(const std::uint8_t port, const std::uint8_t unit) const {
    return static_cast<bool>(devices_[slot(port, unit)]);
}

MapleResponse MapleBus::exchange(
    const std::uint8_t port,
    const std::uint8_t unit,
    const MapleRequest& request
) {
    auto& device = devices_[slot(port, unit)];
    if (!device) {
        throw std::runtime_error("Kein Maple-Geraet an der angeforderten Adresse.");
    }
    auto response = device->transact(request);
    history_.push_back(MapleTransactionRecord{
        next_sequence_++, port, unit, request.command, response.code
    });
    return response;
}

std::span<const MapleTransactionRecord> MapleBus::history() const noexcept {
    return history_;
}

} // namespace katana::runtime
