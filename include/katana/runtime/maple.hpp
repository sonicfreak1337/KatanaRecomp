#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace katana::runtime {

inline constexpr std::size_t maple_port_count = 4u;
inline constexpr std::size_t maple_units_per_port = 6u;

enum class MapleCommand : std::uint8_t {
    DeviceRequest = 0x01u,
    GetCondition = 0x09u,
    BlockRead = 0x0Bu,
    BlockWrite = 0x0Cu
};

enum class MapleResponseCode : std::uint8_t {
    DeviceInfo = 0x05u,
    DataTransfer = 0x08u,
    Ack = 0x07u,
    UnknownCommand = 0xFEu
};

struct MapleRequest {
    MapleCommand command = MapleCommand::DeviceRequest;
    std::vector<std::uint32_t> payload;
};

struct MapleResponse {
    MapleResponseCode code = MapleResponseCode::UnknownCommand;
    std::vector<std::uint32_t> payload;
};

class MapleDevice {
public:
    virtual ~MapleDevice() = default;
    [[nodiscard]] virtual MapleResponse transact(const MapleRequest& request) = 0;
};

enum class ControllerButton : std::uint16_t {
    C = 1u << 0u, B = 1u << 1u, A = 1u << 2u, Start = 1u << 3u,
    DpadUp = 1u << 4u, DpadDown = 1u << 5u, DpadLeft = 1u << 6u,
    DpadRight = 1u << 7u, Z = 1u << 8u, Y = 1u << 9u, X = 1u << 10u,
    D = 1u << 11u, Dpad2Up = 1u << 12u, Dpad2Down = 1u << 13u,
    Dpad2Left = 1u << 14u, Dpad2Right = 1u << 15u
};

struct ControllerState {
    std::uint16_t pressed_buttons = 0u;
    std::uint8_t right_trigger = 0u;
    std::uint8_t left_trigger = 0u;
    std::uint8_t joystick_x = 0x80u;
    std::uint8_t joystick_y = 0x80u;
    std::uint8_t joystick2_x = 0x80u;
    std::uint8_t joystick2_y = 0x80u;
};

class HostInputBackend {
public:
    virtual ~HostInputBackend() = default;
    [[nodiscard]] virtual ControllerState sample(std::uint64_t frame) = 0;
};

class ReplayInputBackend final : public HostInputBackend {
public:
    explicit ReplayInputBackend(std::vector<ControllerState> frames);
    [[nodiscard]] ControllerState sample(std::uint64_t frame) override;
private:
    std::vector<ControllerState> frames_;
};

class MapleControllerDevice final : public MapleDevice {
public:
    explicit MapleControllerDevice(std::shared_ptr<HostInputBackend> input);
    [[nodiscard]] MapleResponse transact(const MapleRequest& request) override;
    [[nodiscard]] std::uint64_t sampled_frames() const noexcept;
private:
    std::shared_ptr<HostInputBackend> input_;
    std::uint64_t next_frame_ = 0u;
};

struct MapleTransactionRecord {
    std::uint64_t sequence = 0u;
    std::uint8_t port = 0u;
    std::uint8_t unit = 0u;
    MapleCommand command = MapleCommand::DeviceRequest;
    MapleResponseCode response = MapleResponseCode::UnknownCommand;
};

class MapleBus final {
public:
    void attach(std::uint8_t port, std::uint8_t unit, std::shared_ptr<MapleDevice> device);
    [[nodiscard]] bool attached(std::uint8_t port, std::uint8_t unit) const;
    [[nodiscard]] MapleResponse exchange(
        std::uint8_t port,
        std::uint8_t unit,
        const MapleRequest& request
    );
    [[nodiscard]] std::span<const MapleTransactionRecord> history() const noexcept;

private:
    [[nodiscard]] static std::size_t slot(std::uint8_t port, std::uint8_t unit);
    std::array<std::shared_ptr<MapleDevice>, maple_port_count * maple_units_per_port> devices_{};
    std::vector<MapleTransactionRecord> history_;
    std::uint64_t next_sequence_ = 1u;
};

} // namespace katana::runtime
