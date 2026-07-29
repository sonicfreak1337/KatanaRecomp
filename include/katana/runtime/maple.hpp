#pragma once

#include "katana/runtime/persistent_storage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace katana::runtime {

inline constexpr std::size_t maple_port_count = 4u;
inline constexpr std::size_t maple_units_per_port = 6u;

enum class MapleCommand : std::uint8_t {
    DeviceRequest = 0x01u,
    GetCondition = 0x09u,
    GetMemoryInformation = 0x0Au,
    BlockRead = 0x0Bu,
    BlockWrite = 0x0Cu,
    BlockSync = 0x0Du,
    GetLastError = BlockSync
};

enum class MapleResponseCode : std::uint8_t {
    DeviceInfo = 0x05u,
    DataTransfer = 0x08u,
    Ack = 0x07u,
    FileError = 0xFBu,
    UnknownCommand = 0xFDu,
    FunctionNotSupported = 0xFEu
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
    [[nodiscard]] virtual MapleResponse transact_at(const MapleRequest& request,
                                                    std::uint64_t guest_cycle);
};

enum class ControllerButton : std::uint16_t {
    C = 1u << 0u,
    B = 1u << 1u,
    A = 1u << 2u,
    Start = 1u << 3u,
    DpadUp = 1u << 4u,
    DpadDown = 1u << 5u,
    DpadLeft = 1u << 6u,
    DpadRight = 1u << 7u,
    Z = 1u << 8u,
    Y = 1u << 9u,
    X = 1u << 10u,
    D = 1u << 11u,
    Dpad2Up = 1u << 12u,
    Dpad2Down = 1u << 13u,
    Dpad2Left = 1u << 14u,
    Dpad2Right = 1u << 15u
};

struct ControllerState {
    std::uint16_t pressed_buttons = 0u;
    std::uint8_t right_trigger = 0u;
    std::uint8_t left_trigger = 0u;
    std::uint8_t joystick_x = 0x80u;
    std::uint8_t joystick_y = 0x80u;
    std::uint8_t joystick2_x = 0x80u;
    std::uint8_t joystick2_y = 0x80u;

    [[nodiscard]] bool operator==(const ControllerState&) const = default;
};

class HostInputBackend {
  public:
    virtual ~HostInputBackend() = default;
    [[nodiscard]] virtual ControllerState sample(std::uint64_t frame) = 0;
    [[nodiscard]] virtual ControllerState sample_at(std::uint64_t frame,
                                                    std::uint64_t guest_cycle);
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
    [[nodiscard]] MapleResponse transact_at(const MapleRequest& request,
                                            std::uint64_t guest_cycle) override;
    [[nodiscard]] std::uint64_t sampled_frames() const noexcept;
    void restore_sampled_frames(std::uint64_t next_frame) noexcept;

  private:
    std::shared_ptr<HostInputBackend> input_;
    std::uint64_t next_frame_ = 0u;
};

inline constexpr std::size_t vmu_block_size = 512u;
inline constexpr std::size_t vmu_block_count = 256u;
inline constexpr std::size_t vmu_storage_size = vmu_block_size * vmu_block_count;
inline constexpr std::uint32_t vmu_memory_function = 0x02000000u;

struct MapleVmuPendingWrite {
    std::array<std::uint8_t, vmu_block_size> bytes{};
    std::uint16_t block = 0u;
    std::uint8_t partition = 0u;
    // The next phase accepted by the device. A live transaction has consumed
    // one through four strictly ordered 128-byte phases.
    std::uint8_t next_phase = 0u;

    [[nodiscard]] bool operator==(const MapleVmuPendingWrite&) const = default;
};

struct MapleVmuSnapshot {
    std::size_t size = 0u;
    bool write_protected = false;
    bool working_copy_dirty = false;
    bool persistent_working_copy = false;

    [[nodiscard]] bool operator==(const MapleVmuSnapshot&) const = default;
};

// Complete guest-visible VMU state. This is intentionally separate from the
// cheap MapleVmuSnapshot used by runtime probes: handoff restore must carry the
// bytes and may never reconstruct them from a probe digest.
struct MapleVmuStateSnapshot {
    std::vector<std::uint8_t> source_image;
    std::vector<std::uint8_t> working_image;
    bool write_protected = false;
    bool working_copy_dirty = false;
    bool persistent_working_copy = false;
    std::optional<MapleVmuPendingWrite> pending_write;

    [[nodiscard]] bool operator==(const MapleVmuStateSnapshot&) const = default;
};

class MapleVmuDevice;

class PreparedMapleVmuStateRestore final {
  public:
    PreparedMapleVmuStateRestore(const PreparedMapleVmuStateRestore&) = delete;
    PreparedMapleVmuStateRestore& operator=(const PreparedMapleVmuStateRestore&) = delete;
    PreparedMapleVmuStateRestore(PreparedMapleVmuStateRestore&&) noexcept = default;
    PreparedMapleVmuStateRestore&
    operator=(PreparedMapleVmuStateRestore&&) noexcept = default;

  private:
    friend class MapleVmuDevice;
    PreparedMapleVmuStateRestore() = default;

    const MapleVmuDevice* owner_ = nullptr;
    bool write_protected_ = false;
    bool replace_working_copy_ = false;
    bool replace_write_protection_ = false;
    bool replace_pending_write_ = false;
    std::vector<std::uint8_t> working_;
    std::optional<MapleVmuPendingWrite> pending_write_;
    std::optional<PreparedPersistentImageRestore> persistent_;
};

class MapleVmuDevice final : public MapleDevice {
  public:
    explicit MapleVmuDevice(std::span<const std::uint8_t> image = {});
    explicit MapleVmuDevice(std::shared_ptr<PersistentImage> image);
    [[nodiscard]] MapleResponse transact(const MapleRequest& request) override;
    void set_write_protected(bool value) noexcept;
    [[nodiscard]] bool write_protected() const noexcept;
    [[nodiscard]] std::uint8_t read_byte(std::size_t offset) const;
    [[nodiscard]] std::uint8_t source_byte(std::size_t offset) const;
    void save_working_copy();
    [[nodiscard]] bool working_copy_dirty() const noexcept;
    [[nodiscard]] bool persistent_working_copy() const noexcept;
    [[nodiscard]] MapleVmuSnapshot snapshot() const noexcept;
    [[nodiscard]] MapleVmuStateSnapshot state_snapshot() const;
    void validate_state_restore(const MapleVmuStateSnapshot& state) const;
    [[nodiscard]] PreparedMapleVmuStateRestore prepare_state_restore(
        const MapleVmuStateSnapshot& state,
        PersistenceHandoffPolicy policy) const;
    void commit_prepared_state_restore(PreparedMapleVmuStateRestore prepared) noexcept;
    void restore_state(const MapleVmuStateSnapshot& state);

  private:
    [[nodiscard]] MapleResponse memory_information(const MapleRequest& request) const;
    [[nodiscard]] MapleResponse read_block(const MapleRequest& request) const;
    [[nodiscard]] MapleResponse write_block(const MapleRequest& request);
    [[nodiscard]] MapleResponse sync_block(const MapleRequest& request);
    std::vector<std::uint8_t> source_;
    std::vector<std::uint8_t> working_;
    std::shared_ptr<PersistentImage> persistent_image_;
    std::optional<MapleVmuPendingWrite> pending_write_;
    bool write_protected_ = false;
};

struct MapleTransactionRecord {
    std::uint64_t sequence = 0u;
    std::uint8_t port = 0u;
    std::uint8_t unit = 0u;
    MapleCommand command = MapleCommand::DeviceRequest;
    MapleResponseCode response = MapleResponseCode::UnknownCommand;

    [[nodiscard]] bool operator==(const MapleTransactionRecord&) const = default;
};

struct MapleBusSnapshot {
    std::array<bool, maple_port_count * maple_units_per_port> attached{};
    std::vector<MapleTransactionRecord> history;
    std::uint64_t next_sequence = 0u;

    [[nodiscard]] bool operator==(const MapleBusSnapshot&) const = default;
};

struct MapleControllerDeviceStateSnapshot {
    std::uint64_t next_frame = 0u;

    [[nodiscard]] bool operator==(
        const MapleControllerDeviceStateSnapshot&) const = default;
};

using MaplePeripheralStateSnapshot =
    std::variant<MapleControllerDeviceStateSnapshot, MapleVmuStateSnapshot>;

struct MapleAttachedPeripheralStateSnapshot {
    std::uint8_t port = 0u;
    std::uint8_t unit = 0u;
    MaplePeripheralStateSnapshot state;

    [[nodiscard]] bool operator==(
        const MapleAttachedPeripheralStateSnapshot&) const = default;
};

// Lossless bus state for a product handoff. Unsupported/custom peripherals
// are rejected instead of being silently represented as an attached bit.
struct MapleBusStateSnapshot {
    std::array<bool, maple_port_count * maple_units_per_port> attached{};
    std::vector<MapleAttachedPeripheralStateSnapshot> peripherals;
    std::vector<MapleTransactionRecord> history;
    std::uint64_t next_sequence = 0u;

    [[nodiscard]] bool operator==(const MapleBusStateSnapshot&) const = default;
};

class MapleBus;

class PreparedMapleBusStateRestore final {
  public:
    PreparedMapleBusStateRestore(const PreparedMapleBusStateRestore&) = delete;
    PreparedMapleBusStateRestore& operator=(const PreparedMapleBusStateRestore&) = delete;
    PreparedMapleBusStateRestore(PreparedMapleBusStateRestore&&) noexcept = default;
    PreparedMapleBusStateRestore&
    operator=(PreparedMapleBusStateRestore&&) noexcept = default;

  private:
    friend class MapleBus;
    PreparedMapleBusStateRestore() = default;

    struct ControllerRestore {
        MapleControllerDevice* target = nullptr;
        std::uint64_t next_frame = 0u;
    };
    struct VmuRestore {
        MapleVmuDevice* target = nullptr;
        PreparedMapleVmuStateRestore prepared;
    };

    const MapleBus* owner_ = nullptr;
    std::vector<ControllerRestore> controllers_;
    std::vector<VmuRestore> vmus_;
    std::vector<MapleTransactionRecord> history_;
    std::uint64_t next_sequence_ = 1u;
    bool replace_diagnostic_history_ = false;
};

class MapleBus final {
  public:
    explicit MapleBus(std::function<void()> completion_observer = {});
    void attach(std::uint8_t port, std::uint8_t unit, std::shared_ptr<MapleDevice> device);
    [[nodiscard]] bool attached(std::uint8_t port, std::uint8_t unit) const;
    // The high bits identify the physical port. A main peripheral advertises
    // every attached sub-peripheral; a sub-peripheral uses only its unit bit.
    [[nodiscard]] std::uint8_t
    response_sender_address(std::uint8_t port, std::uint8_t unit) const;
    [[nodiscard]] MapleResponse
    exchange(std::uint8_t port, std::uint8_t unit, const MapleRequest& request);
    [[nodiscard]] MapleResponse exchange_at(std::uint8_t port,
                                            std::uint8_t unit,
                                            const MapleRequest& request,
                                            std::uint64_t guest_cycle);
    [[nodiscard]] MapleResponse
    exchange_without_completion(std::uint8_t port, std::uint8_t unit, const MapleRequest& request);
    [[nodiscard]] MapleResponse exchange_without_completion_at(std::uint8_t port,
                                                               std::uint8_t unit,
                                                               const MapleRequest& request,
                                                               std::uint64_t guest_cycle);
    [[nodiscard]] std::span<const MapleTransactionRecord> history() const noexcept;
    [[nodiscard]] MapleBusSnapshot snapshot() const;
    [[nodiscard]] MapleBusStateSnapshot state_snapshot() const;
    void validate_state_restore(const MapleBusStateSnapshot& state) const;
    [[nodiscard]] PreparedMapleBusStateRestore prepare_state_restore(
        const MapleBusStateSnapshot& state,
        PersistenceHandoffPolicy policy) const;
    void commit_prepared_state_restore(PreparedMapleBusStateRestore prepared) noexcept;
    void restore_state(const MapleBusStateSnapshot& state);

  private:
    [[nodiscard]] static std::size_t slot(std::uint8_t port, std::uint8_t unit);
    [[nodiscard]] MapleResponse exchange_impl(std::uint8_t port,
                                              std::uint8_t unit,
                                              const MapleRequest& request,
                                              bool notify_completion,
                                              std::uint64_t guest_cycle);
    std::array<std::shared_ptr<MapleDevice>, maple_port_count * maple_units_per_port> devices_{};
    std::vector<MapleTransactionRecord> history_;
    std::uint64_t next_sequence_ = 1u;
    std::function<void()> completion_observer_;
};

} // namespace katana::runtime
