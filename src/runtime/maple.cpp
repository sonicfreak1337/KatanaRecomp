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
    const std::uint32_t condition0 = static_cast<std::uint32_t>(buttons) |
                                     (static_cast<std::uint32_t>(state.right_trigger) << 16u) |
                                     (static_cast<std::uint32_t>(state.left_trigger) << 24u);
    const std::uint32_t condition1 = static_cast<std::uint32_t>(state.joystick_x) |
                                     (static_cast<std::uint32_t>(state.joystick_y) << 8u) |
                                     (static_cast<std::uint32_t>(state.joystick2_x) << 16u) |
                                     (static_cast<std::uint32_t>(state.joystick2_y) << 24u);
    return {MapleResponseCode::DataTransfer, {controller_function, condition0, condition1}};
}

std::uint64_t MapleControllerDevice::sampled_frames() const noexcept {
    return next_frame_;
}

MapleVmuDevice::MapleVmuDevice(const std::span<const std::uint8_t> image) {
    if (!image.empty() && image.size() != vmu_storage_size) {
        throw std::invalid_argument("Ein VMU-Abbild muss leer oder exakt 128 KiB gross sein.");
    }
    source_.assign(vmu_storage_size, 0xFFu);
    if (!image.empty()) {
        source_.assign(image.begin(), image.end());
    }
    working_ = source_;
}

MapleResponse MapleVmuDevice::transact(const MapleRequest& request) {
    constexpr std::uint32_t memory_function = 0x02000000u;
    switch (request.command) {
    case MapleCommand::DeviceRequest:
        return {MapleResponseCode::DeviceInfo, {memory_function, 0u, 0u}};
    case MapleCommand::BlockRead:
        return read_block(request);
    case MapleCommand::BlockWrite:
        return write_block(request);
    default:
        return {MapleResponseCode::UnknownCommand, {}};
    }
}

MapleResponse MapleVmuDevice::read_block(const MapleRequest& request) const {
    constexpr std::uint32_t memory_function = 0x02000000u;
    if (request.payload.size() != 1u || request.payload[0] >= vmu_block_count) {
        throw std::out_of_range("Ungueltige VMU-Blockleseanfrage.");
    }
    const auto block = static_cast<std::size_t>(request.payload[0]);
    const auto start = block * vmu_block_size;
    std::vector<std::uint32_t> payload;
    payload.reserve(2u + vmu_block_size / 4u);
    payload.push_back(memory_function);
    payload.push_back(static_cast<std::uint32_t>(block));
    for (std::size_t offset = 0u; offset < vmu_block_size; offset += 4u) {
        payload.push_back(static_cast<std::uint32_t>(working_[start + offset]) |
                          (static_cast<std::uint32_t>(working_[start + offset + 1u]) << 8u) |
                          (static_cast<std::uint32_t>(working_[start + offset + 2u]) << 16u) |
                          (static_cast<std::uint32_t>(working_[start + offset + 3u]) << 24u));
    }
    return {MapleResponseCode::DataTransfer, std::move(payload)};
}

MapleResponse MapleVmuDevice::write_block(const MapleRequest& request) {
    constexpr std::size_t words_per_block = vmu_block_size / 4u;
    if (request.payload.size() != 1u + words_per_block || request.payload[0] >= vmu_block_count) {
        throw std::invalid_argument("Ungueltige VMU-Blockschreibanfrage.");
    }
    if (write_protected_) {
        throw std::runtime_error("VMU ist schreibgeschuetzt.");
    }
    const auto start = static_cast<std::size_t>(request.payload[0]) * vmu_block_size;
    for (std::size_t word_index = 0u; word_index < words_per_block; ++word_index) {
        const auto word = request.payload[word_index + 1u];
        for (std::size_t byte = 0u; byte < 4u; ++byte) {
            working_[start + word_index * 4u + byte] =
                static_cast<std::uint8_t>(word >> (byte * 8u));
        }
    }
    return {MapleResponseCode::Ack, {}};
}

void MapleVmuDevice::set_write_protected(const bool value) noexcept {
    write_protected_ = value;
}
bool MapleVmuDevice::write_protected() const noexcept {
    return write_protected_;
}
std::uint8_t MapleVmuDevice::read_byte(const std::size_t offset) const {
    return working_.at(offset);
}
std::uint8_t MapleVmuDevice::source_byte(const std::size_t offset) const {
    return source_.at(offset);
}

std::size_t MapleBus::slot(const std::uint8_t port, const std::uint8_t unit) {
    if (port >= maple_port_count || unit >= maple_units_per_port) {
        throw std::out_of_range("Maple-Port oder -Unit liegt ausserhalb des Busses.");
    }
    return static_cast<std::size_t>(port) * maple_units_per_port + unit;
}

void MapleBus::attach(const std::uint8_t port,
                      const std::uint8_t unit,
                      std::shared_ptr<MapleDevice> device) {
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

MapleResponse
MapleBus::exchange(const std::uint8_t port, const std::uint8_t unit, const MapleRequest& request) {
    auto& device = devices_[slot(port, unit)];
    if (!device) {
        throw std::runtime_error("Kein Maple-Geraet an der angeforderten Adresse.");
    }
    auto response = device->transact(request);
    history_.push_back(
        MapleTransactionRecord{next_sequence_++, port, unit, request.command, response.code});
    return response;
}

std::span<const MapleTransactionRecord> MapleBus::history() const noexcept {
    return history_;
}

} // namespace katana::runtime
