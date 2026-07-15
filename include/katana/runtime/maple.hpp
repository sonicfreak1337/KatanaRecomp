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
