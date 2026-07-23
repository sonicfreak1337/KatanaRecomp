#include "katana/runtime/maple.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {
std::vector<std::uint32_t> device_info_payload(const std::uint32_t functions,
                                               const std::uint32_t definition,
                                               const std::string_view name,
                                               const std::uint16_t standby_current,
                                               const std::uint16_t maximum_current) {
    constexpr std::string_view producer = "Produced By or Under License From SEGA ENTERPRISES,LTD.";
    std::array<std::uint8_t, 112u> bytes{};
    bytes.fill(static_cast<std::uint8_t>(' '));
    const auto put_word = [&bytes](const std::size_t offset, const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
    };
    put_word(0u, functions);
    put_word(4u, definition);
    put_word(8u, 0u);
    put_word(12u, 0u);
    bytes[16u] = 0xFFu;
    bytes[17u] = 0u;
    std::copy_n(name.begin(), std::min<std::size_t>(name.size(), 30u), bytes.begin() + 18u);
    std::copy_n(producer.begin(), std::min<std::size_t>(producer.size(), 60u), bytes.begin() + 48u);
    bytes[108u] = static_cast<std::uint8_t>(standby_current);
    bytes[109u] = static_cast<std::uint8_t>(standby_current >> 8u);
    bytes[110u] = static_cast<std::uint8_t>(maximum_current);
    bytes[111u] = static_cast<std::uint8_t>(maximum_current >> 8u);

    std::vector<std::uint32_t> words(bytes.size() / 4u);
    for (std::size_t word = 0u; word < words.size(); ++word) {
        words[word] = static_cast<std::uint32_t>(bytes[word * 4u]) |
                      (static_cast<std::uint32_t>(bytes[word * 4u + 1u]) << 8u) |
                      (static_cast<std::uint32_t>(bytes[word * 4u + 2u]) << 16u) |
                      (static_cast<std::uint32_t>(bytes[word * 4u + 3u]) << 24u);
    }
    return words;
}
} // namespace

MapleBus::MapleBus(std::function<void()> completion_observer)
    : completion_observer_(std::move(completion_observer)) {}

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
        return {MapleResponseCode::DeviceInfo,
                device_info_payload(
                    controller_function, 0xFE060F00u, "Dreamcast Controller", 0x01AEu, 0x01F4u)};
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

MapleVmuDevice::MapleVmuDevice(std::shared_ptr<PersistentImage> image)
    : persistent_image_(std::move(image)) {
    if (!persistent_image_ || persistent_image_->size() != vmu_storage_size)
        throw std::invalid_argument("Persistente VMU-Arbeitskopie besitzt nicht exakt 128 KiB.");
}

MapleResponse MapleVmuDevice::transact(const MapleRequest& request) {
    constexpr std::uint32_t memory_function = 0x02000000u;
    switch (request.command) {
    case MapleCommand::DeviceRequest:
        return {
            MapleResponseCode::DeviceInfo,
            device_info_payload(memory_function, 0x00410F00u, "Visual Memory", 0x007Cu, 0x0082u)};
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
    const auto bytes =
        persistent_image_ ? persistent_image_->bytes() : std::span<const std::uint8_t>(working_);
    for (std::size_t offset = 0u; offset < vmu_block_size; offset += 4u) {
        payload.push_back(static_cast<std::uint32_t>(bytes[start + offset]) |
                          (static_cast<std::uint32_t>(bytes[start + offset + 1u]) << 8u) |
                          (static_cast<std::uint32_t>(bytes[start + offset + 2u]) << 16u) |
                          (static_cast<std::uint32_t>(bytes[start + offset + 3u]) << 24u));
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
            const auto offset = start + word_index * 4u + byte;
            const auto value = static_cast<std::uint8_t>(word >> (byte * 8u));
            if (persistent_image_)
                persistent_image_->write_byte(offset, value);
            else
                working_[offset] = value;
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
    return persistent_image_ ? persistent_image_->read_byte(offset) : working_.at(offset);
}
std::uint8_t MapleVmuDevice::source_byte(const std::size_t offset) const {
    return persistent_image_ ? persistent_image_->source_byte(offset) : source_.at(offset);
}
void MapleVmuDevice::save_working_copy() {
    if (!persistent_image_) throw std::logic_error("VMU besitzt keine persistente Arbeitskopie.");
    persistent_image_->save();
}
bool MapleVmuDevice::working_copy_dirty() const noexcept {
    return persistent_image_ && persistent_image_->dirty();
}
bool MapleVmuDevice::persistent_working_copy() const noexcept {
    return persistent_image_ != nullptr;
}

MapleVmuSnapshot MapleVmuDevice::snapshot() const noexcept {
    return {
        persistent_image_ ? persistent_image_->size() : working_.size(),
        write_protected_,
        working_copy_dirty(),
        persistent_working_copy(),
    };
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
    return exchange_impl(port, unit, request, true);
}

MapleResponse MapleBus::exchange_without_completion(const std::uint8_t port,
                                                    const std::uint8_t unit,
                                                    const MapleRequest& request) {
    return exchange_impl(port, unit, request, false);
}

MapleResponse MapleBus::exchange_impl(const std::uint8_t port,
                                      const std::uint8_t unit,
                                      const MapleRequest& request,
                                      const bool notify_completion) {
    auto& device = devices_[slot(port, unit)];
    if (!device) {
        throw std::runtime_error("Kein Maple-Geraet an der angeforderten Adresse.");
    }
    auto response = device->transact(request);
    history_.push_back(
        MapleTransactionRecord{next_sequence_++, port, unit, request.command, response.code});
    if (notify_completion && completion_observer_) completion_observer_();
    return response;
}

std::span<const MapleTransactionRecord> MapleBus::history() const noexcept {
    return history_;
}

MapleBusSnapshot MapleBus::snapshot() const {
    MapleBusSnapshot result;
    for (std::size_t index = 0u; index < devices_.size(); ++index)
        result.attached[index] = devices_[index] != nullptr;
    result.history = history_;
    result.next_sequence = next_sequence_;
    return result;
}

} // namespace katana::runtime
