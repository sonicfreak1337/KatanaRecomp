#include "katana/runtime/maple.hpp"

#include <algorithm>
#include <array>
#include <limits>
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

MapleResponse MapleDevice::transact_at(const MapleRequest& request, const std::uint64_t) {
    return transact(request);
}

ControllerState HostInputBackend::sample_at(const std::uint64_t frame, const std::uint64_t) {
    return sample(frame);
}

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
    return transact_at(request, 0u);
}

MapleResponse MapleControllerDevice::transact_at(const MapleRequest& request,
                                                 const std::uint64_t guest_cycle) {
    constexpr std::uint32_t controller_function = 0x01000000u;
    if (request.command == MapleCommand::DeviceRequest) {
        return {MapleResponseCode::DeviceInfo,
                device_info_payload(
                    controller_function, 0xFE060F00u, "Dreamcast Controller", 0x01AEu, 0x01F4u)};
    }
    if (request.command != MapleCommand::GetCondition) {
        return {MapleResponseCode::UnknownCommand, {}};
    }
    const auto state = input_->sample_at(next_frame_++, guest_cycle);
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

void MapleControllerDevice::restore_sampled_frames(
    const std::uint64_t next_frame) noexcept {
    next_frame_ = next_frame;
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

MapleVmuStateSnapshot MapleVmuDevice::state_snapshot() const {
    MapleVmuStateSnapshot result;
    result.source_image.resize(vmu_storage_size);
    for (std::size_t offset = 0u; offset < result.source_image.size(); ++offset)
        result.source_image[offset] = source_byte(offset);
    if (persistent_image_) {
        const auto working = persistent_image_->bytes();
        result.working_image.assign(working.begin(), working.end());
    } else {
        result.working_image = working_;
    }
    result.write_protected = write_protected_;
    result.working_copy_dirty = working_copy_dirty();
    result.persistent_working_copy = persistent_working_copy();
    return result;
}

void MapleVmuDevice::validate_state_restore(
    const MapleVmuStateSnapshot& state) const {
    if (state.source_image.size() != vmu_storage_size ||
        state.working_image.size() != vmu_storage_size)
        throw std::invalid_argument(
            "VMU-Handoff braucht exakt 128 KiB Quell- und Arbeitsabbild.");
    if (state.persistent_working_copy != persistent_working_copy())
        throw std::invalid_argument(
            "VMU-Handoff und Runtime besitzen unterschiedliche Persistenzvertraege.");
    if (!state.persistent_working_copy && state.working_copy_dirty)
        throw std::invalid_argument(
            "Nichtpersistente VMU darf keinen persistenten Dirty-Zustand tragen.");

    for (std::size_t offset = 0u; offset < vmu_storage_size; ++offset) {
        if (source_byte(offset) != state.source_image[offset])
            throw std::invalid_argument(
                "VMU-Handoff passt nicht zum gebundenen Quellabbild.");
    }

    if (!persistent_image_) return;
    const auto current = persistent_image_->bytes();
    const auto bytes_match =
        std::equal(current.begin(), current.end(), state.working_image.begin());
    if (!state.working_copy_dirty &&
        (persistent_image_->dirty() || !bytes_match))
        throw std::invalid_argument(
            "Ein sauberer VMU-Handoff darf keine andere oder bereits dirty "
            "Arbeitskopie ohne Hostdatei-Commit ersetzen.");
}

void MapleVmuDevice::restore_state(const MapleVmuStateSnapshot& state) {
    validate_state_restore(state);

    if (!persistent_image_) {
        auto restored = state.working_image;
        working_ = std::move(restored);
        write_protected_ = state.write_protected;
        return;
    }

    persistent_image_->write(0u, state.working_image);
    if (state.working_copy_dirty && !persistent_image_->dirty()) {
        // Dirty is host-persistence bookkeeping, but it is part of a lossless
        // state transfer. Force it without changing the final guest bytes.
        const auto original = state.working_image.front();
        persistent_image_->write_byte(
            0u, static_cast<std::uint8_t>(original ^ 1u));
        persistent_image_->write_byte(0u, original);
    }
    write_protected_ = state.write_protected;
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
    return exchange_impl(port, unit, request, true, 0u);
}

MapleResponse MapleBus::exchange_at(const std::uint8_t port,
                                    const std::uint8_t unit,
                                    const MapleRequest& request,
                                    const std::uint64_t guest_cycle) {
    return exchange_impl(port, unit, request, true, guest_cycle);
}

MapleResponse MapleBus::exchange_without_completion(const std::uint8_t port,
                                                    const std::uint8_t unit,
                                                    const MapleRequest& request) {
    return exchange_impl(port, unit, request, false, 0u);
}

MapleResponse
MapleBus::exchange_without_completion_at(const std::uint8_t port,
                                         const std::uint8_t unit,
                                         const MapleRequest& request,
                                         const std::uint64_t guest_cycle) {
    return exchange_impl(port, unit, request, false, guest_cycle);
}

MapleResponse MapleBus::exchange_impl(const std::uint8_t port,
                                      const std::uint8_t unit,
                                      const MapleRequest& request,
                                      const bool notify_completion,
                                      const std::uint64_t guest_cycle) {
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Maple-Transaktionssequenz ist uebergelaufen.");
    auto& device = devices_[slot(port, unit)];
    if (!device) {
        throw std::runtime_error("Kein Maple-Geraet an der angeforderten Adresse.");
    }
    auto response = device->transact_at(request, guest_cycle);
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

MapleBusStateSnapshot MapleBus::state_snapshot() const {
    MapleBusStateSnapshot result;
    for (std::size_t index = 0u; index < devices_.size(); ++index) {
        const auto& device = devices_[index];
        result.attached[index] = device != nullptr;
        if (!device) continue;

        MapleAttachedPeripheralStateSnapshot peripheral;
        peripheral.port =
            static_cast<std::uint8_t>(index / maple_units_per_port);
        peripheral.unit =
            static_cast<std::uint8_t>(index % maple_units_per_port);
        if (const auto controller =
                std::dynamic_pointer_cast<MapleControllerDevice>(device)) {
            peripheral.state =
                MapleControllerDeviceStateSnapshot{
                    controller->sampled_frames()};
        } else if (const auto vmu =
                       std::dynamic_pointer_cast<MapleVmuDevice>(device)) {
            peripheral.state = vmu->state_snapshot();
        } else {
            throw std::runtime_error(
                "Maple-Handoff kann ein unbekanntes Peripheriemodell nicht "
                "verlustfrei abbilden.");
        }
        result.peripherals.push_back(std::move(peripheral));
    }
    result.history = history_;
    result.next_sequence = next_sequence_;
    return result;
}

void MapleBus::validate_state_restore(
    const MapleBusStateSnapshot& state) const {
    std::array<bool, maple_port_count * maple_units_per_port>
        described{};
    std::size_t previous_slot = 0u;
    bool have_previous = false;

    for (std::size_t index = 0u; index < devices_.size(); ++index) {
        if (state.attached[index] != static_cast<bool>(devices_[index]))
            throw std::invalid_argument(
                "Maple-Handoff passt nicht zur Runtime-Peripherietopologie.");
    }

    for (const auto& peripheral : state.peripherals) {
        const auto index = slot(peripheral.port, peripheral.unit);
        if (have_previous && previous_slot >= index)
            throw std::invalid_argument(
                "Maple-Handoff-Peripherie muss eindeutig und geordnet sein.");
        previous_slot = index;
        have_previous = true;
        if (!state.attached[index] || !devices_[index] ||
            described[index])
            throw std::invalid_argument(
                "Maple-Handoff beschreibt eine ungueltige Peripherie.");
        described[index] = true;

        if (const auto* controller =
                std::get_if<MapleControllerDeviceStateSnapshot>(
                    &peripheral.state)) {
            static_cast<void>(controller);
            if (!std::dynamic_pointer_cast<MapleControllerDevice>(
                    devices_[index]))
                throw std::invalid_argument(
                    "Maple-Handoff-Controller passt nicht zum Runtimegeraet.");
        } else if (const auto* vmu =
                       std::get_if<MapleVmuStateSnapshot>(
                           &peripheral.state)) {
            const auto target =
                std::dynamic_pointer_cast<MapleVmuDevice>(
                    devices_[index]);
            if (!target)
                throw std::invalid_argument(
                    "Maple-Handoff-VMU passt nicht zum Runtimegeraet.");
            target->validate_state_restore(*vmu);
        } else {
            throw std::invalid_argument(
                "Maple-Handoff besitzt einen unbekannten Peripheriezustand.");
        }
    }

    for (std::size_t index = 0u; index < devices_.size(); ++index) {
        if (state.attached[index] != described[index])
            throw std::invalid_argument(
                "Maple-Handoff fehlt Zustand fuer eine angeschlossene "
                "Peripherie.");
    }

    if (state.next_sequence == 0u)
        throw std::invalid_argument(
            "Maple-Handoff besitzt eine ungueltige Folgesequenz.");
    std::uint64_t previous_sequence = 0u;
    for (const auto& record : state.history) {
        static_cast<void>(slot(record.port, record.unit));
        if (record.sequence == 0u ||
            record.sequence <= previous_sequence ||
            record.sequence >= state.next_sequence)
            throw std::invalid_argument(
                "Maple-Handoff besitzt eine ungueltige Transaktionshistorie.");
        previous_sequence = record.sequence;
    }
}

void MapleBus::restore_state(const MapleBusStateSnapshot& state) {
    validate_state_restore(state);

    for (const auto& peripheral : state.peripherals) {
        auto& target = devices_[slot(peripheral.port, peripheral.unit)];
        if (const auto* controller =
                std::get_if<MapleControllerDeviceStateSnapshot>(
                    &peripheral.state)) {
            std::dynamic_pointer_cast<MapleControllerDevice>(target)
                ->restore_sampled_frames(controller->next_frame);
        } else {
            std::dynamic_pointer_cast<MapleVmuDevice>(target)->restore_state(
                std::get<MapleVmuStateSnapshot>(peripheral.state));
        }
    }
    history_ = state.history;
    next_sequence_ = state.next_sequence;
}

} // namespace katana::runtime
