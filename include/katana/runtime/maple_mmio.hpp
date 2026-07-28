#pragma once

#include "katana/runtime/maple.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t maple_mmio_physical_base = 0x005F6C00u;
inline constexpr std::uint32_t maple_mmio_register_size = 0x100u;
inline constexpr std::uint32_t dreamcast_maple_state_contract_version = 1u;
inline constexpr std::uint32_t dreamcast_maple_dma_event_channel = 0u;
inline constexpr std::uint64_t dreamcast_maple_dma_event_token_v1 = 0u;

namespace maple_register {
inline constexpr std::uint32_t DmaCommandTable = 0x04u;
inline constexpr std::uint32_t DmaTriggerSelect = 0x10u;
inline constexpr std::uint32_t DmaEnable = 0x14u;
inline constexpr std::uint32_t DmaStart = 0x18u;
inline constexpr std::uint32_t SystemControl = 0x80u;
inline constexpr std::uint32_t Status = 0x84u;
inline constexpr std::uint32_t HardTriggerClear = 0x88u;
inline constexpr std::uint32_t DmaAddressProtect = 0x8Cu;
inline constexpr std::uint32_t MsbSelect = 0xE8u;
inline constexpr std::uint32_t TxAddressCounter = 0xF4u;
inline constexpr std::uint32_t RxAddressCounter = 0xF8u;
inline constexpr std::uint32_t RxBaseAddress = 0xFCu;
} // namespace maple_register

struct MapleDmaTiming {
    std::uint64_t cycles_per_word = 100u;

    [[nodiscard]] bool operator==(const MapleDmaTiming&) const = default;
};

enum class MapleDmaState : std::uint8_t {
    Disabled,
    Armed,
    Active,
    Completed,
    Failed,
};

enum class MapleDmaError : std::uint8_t {
    None,
    InvalidConfiguration,
    ProtectedRange,
    InvalidDescriptor,
    UnsupportedDescriptor,
    ResponseRange,
    SchedulerFailure,
    AtomicCommitFailure,
    InternalLifecycle,
};

enum class MapleDmaEventPublicationState : std::uint8_t {
    NotRequested,
    Publishing,
    Published,
    Failed,
};

enum class MapleDmaEventPublicationError : std::uint8_t {
    None,
    ObserverException,
};

struct DreamcastMaplePendingResponseSnapshot {
    std::uint32_t destination = 0u;
    std::vector<std::uint32_t> words;

    [[nodiscard]] bool operator==(const DreamcastMaplePendingResponseSnapshot&) const = default;
};

struct DreamcastMapleControllerSnapshot {
    MapleDmaTiming timing{};
    std::optional<SchedulerEventId> completion_event;
    // SchedulerEventId is process-local. Handoff artifacts carry the stable
    // channel/token pair above and receive a fresh ID during rehydration.
    bool completion_event_rehydration_pending = false;
    std::vector<DreamcastMaplePendingResponseSnapshot> pending_responses;
    std::uint32_t command_table = 0u;
    std::uint32_t trigger_select = 0u;
    std::uint32_t enabled = 0u;
    std::uint32_t active = 0u;
    MapleDmaState state = MapleDmaState::Disabled;
    MapleDmaError error = MapleDmaError::None;
    std::optional<std::uint32_t> error_address;
    MapleDmaEventPublicationState event_publication_state =
        MapleDmaEventPublicationState::NotRequested;
    MapleDmaEventPublicationError event_publication_error =
        MapleDmaEventPublicationError::None;
    std::uint64_t event_publication_failure_count = 0u;
    std::uint32_t system_control = 0u;
    std::uint32_t address_protect = 0u;
    std::uint32_t msb_select = 0u;
    std::uint32_t tx_address = 0u;
    std::uint32_t rx_address = 0u;
    std::uint32_t rx_base = 0u;
    std::uint64_t completed_dma_count = 0u;
    std::uint64_t transferred_word_count = 0u;
    std::uint64_t failed_dma_count = 0u;
    bool hard_trigger_failed = false;

    [[nodiscard]] bool operator==(
        const DreamcastMapleControllerSnapshot&) const = default;
};

class DreamcastMapleController;

class PreparedDreamcastMapleControllerRestore final {
  public:
    PreparedDreamcastMapleControllerRestore(
        const PreparedDreamcastMapleControllerRestore&) = delete;
    PreparedDreamcastMapleControllerRestore&
    operator=(const PreparedDreamcastMapleControllerRestore&) = delete;
    PreparedDreamcastMapleControllerRestore(
        PreparedDreamcastMapleControllerRestore&&) noexcept = default;
    PreparedDreamcastMapleControllerRestore&
    operator=(PreparedDreamcastMapleControllerRestore&&) noexcept = default;

  private:
    friend class DreamcastMapleController;
    PreparedDreamcastMapleControllerRestore() = default;

    const DreamcastMapleController* owner_ = nullptr;
    DreamcastMapleControllerSnapshot state_;
};

struct DreamcastMapleStateSnapshot {
    MapleBusStateSnapshot bus;
    DreamcastMapleControllerSnapshot controller;

    [[nodiscard]] bool operator==(const DreamcastMapleStateSnapshot&) const = default;
};

class DreamcastMapleController final {
  public:
    DreamcastMapleController(Memory& memory,
                             EventScheduler& scheduler,
                             std::shared_ptr<MapleBus> bus,
                             MapleDmaTiming timing = {},
                             std::function<void()> completion_observer = {});
    ~DreamcastMapleController();
    DreamcastMapleController(const DreamcastMapleController&) = delete;
    DreamcastMapleController& operator=(const DreamcastMapleController&) = delete;

    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t completed_dma_count() const noexcept;
    [[nodiscard]] std::uint64_t transferred_word_count() const noexcept;
    void hardware_trigger() noexcept;
    [[nodiscard]] bool hard_trigger_failed() const noexcept;
    [[nodiscard]] MapleDmaState state() const noexcept;
    [[nodiscard]] MapleDmaError error() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> error_address() const noexcept;
    [[nodiscard]] MapleDmaEventPublicationState
    event_publication_state() const noexcept;
    [[nodiscard]] MapleDmaEventPublicationError
    event_publication_error() const noexcept;
    [[nodiscard]] std::uint64_t event_publication_failure_count() const noexcept;
    [[nodiscard]] DreamcastMapleControllerSnapshot snapshot() const;
    void validate_state_restore(
        const DreamcastMapleControllerSnapshot& state) const;
    [[nodiscard]] PreparedDreamcastMapleControllerRestore
    prepare_state_restore(const DreamcastMapleControllerSnapshot& state) const;
    void commit_prepared_state_restore(
        PreparedDreamcastMapleControllerRestore prepared) noexcept;
    // Passive restore deliberately discards captured SchedulerEventIds.
    // A restored active transfer cannot execute until its typed event has
    // been rehydrated with rehydrate_scheduled_event().
    void restore_state_passive(
        const DreamcastMapleControllerSnapshot& state);
    [[nodiscard]] SchedulerEventId rehydrate_scheduled_event(
        std::uint64_t guest_cycle,
        std::uint32_t channel,
        std::uint64_t token);
    [[nodiscard]] bool event_rehydration_pending() const noexcept;

  private:
    using PendingResponse = DreamcastMaplePendingResponseSnapshot;

    void start_dma();
    void complete_dma(SchedulerEventId event_id) noexcept;
    void cancel_pending() noexcept;
    void fail(MapleDmaError error, std::optional<std::uint32_t> address = {}) noexcept;
    void publish_dma_event() noexcept;
    void clear_event_publication() noexcept;
    void handle_scheduler_reset() noexcept;
    [[nodiscard]] bool protected_address(std::uint32_t address, std::size_t size) const noexcept;
    [[nodiscard]] std::pair<std::uint8_t, std::uint8_t>
    decode_recipient(std::uint8_t bus, std::uint8_t recipient) const;

    Memory& memory_;
    EventScheduler& scheduler_;
    std::shared_ptr<MapleBus> bus_;
    MapleDmaTiming timing_;
    std::function<void()> completion_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> completion_event_;
    std::vector<PendingResponse> pending_responses_;
    std::uint32_t command_table_ = 0u;
    std::uint32_t trigger_select_ = 0u;
    std::uint32_t enabled_ = 0u;
    std::uint32_t active_ = 0u;
    MapleDmaState state_ = MapleDmaState::Disabled;
    MapleDmaError error_ = MapleDmaError::None;
    std::optional<std::uint32_t> error_address_;
    MapleDmaEventPublicationState event_publication_state_ =
        MapleDmaEventPublicationState::NotRequested;
    MapleDmaEventPublicationError event_publication_error_ =
        MapleDmaEventPublicationError::None;
    std::uint64_t event_publication_failure_count_ = 0u;
    std::uint32_t system_control_ = 0u;
    std::uint32_t address_protect_ = 0u;
    std::uint32_t msb_select_ = 1u;
    std::uint32_t tx_address_ = 0u;
    std::uint32_t rx_address_ = 0u;
    std::uint32_t rx_base_ = 0u;
    std::uint64_t completed_dma_count_ = 0u;
    std::uint64_t transferred_word_count_ = 0u;
    std::uint64_t failed_dma_count_ = 0u;
    bool hard_trigger_failed_ = false;
    bool completion_event_rehydration_pending_ = false;
};

[[nodiscard]] DreamcastMapleStateSnapshot
snapshot_dreamcast_maple_state(const MapleBus& bus,
                               const DreamcastMapleController& controller);

class PreparedDreamcastMapleStateRestore final {
  public:
    PreparedDreamcastMapleStateRestore(
        const PreparedDreamcastMapleStateRestore&) = delete;
    PreparedDreamcastMapleStateRestore&
    operator=(const PreparedDreamcastMapleStateRestore&) = delete;
    PreparedDreamcastMapleStateRestore(
        PreparedDreamcastMapleStateRestore&&) noexcept = default;
    PreparedDreamcastMapleStateRestore&
    operator=(PreparedDreamcastMapleStateRestore&&) noexcept = default;

  private:
    friend PreparedDreamcastMapleStateRestore
    prepare_dreamcast_maple_state_restore(
        const MapleBus&,
        const DreamcastMapleController&,
        const DreamcastMapleStateSnapshot&,
        PersistenceHandoffPolicy);
    friend void commit_dreamcast_maple_state_restore(
        MapleBus&,
        DreamcastMapleController&,
        PreparedDreamcastMapleStateRestore) noexcept;

    PreparedDreamcastMapleStateRestore(
        PreparedMapleBusStateRestore bus,
        PreparedDreamcastMapleControllerRestore controller) noexcept
        : bus_(std::move(bus)), controller_(std::move(controller)) {}

    PreparedMapleBusStateRestore bus_;
    PreparedDreamcastMapleControllerRestore controller_;
};

[[nodiscard]] PreparedDreamcastMapleStateRestore
prepare_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state,
    PersistenceHandoffPolicy policy);
void commit_dreamcast_maple_state_restore(
    MapleBus& bus,
    DreamcastMapleController& controller,
    PreparedDreamcastMapleStateRestore prepared) noexcept;
void validate_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state);
void validate_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state,
    PersistenceHandoffPolicy policy);
void restore_dreamcast_maple_state_passive(
    MapleBus& bus,
    DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state);

// Stable private-payload codec used by GameEntryDeviceKind::Maple. The
// process-local SchedulerEventId is intentionally omitted; the corresponding
// GameEntryScheduledEvent carries kind MapleDma, channel 0 and token 0.
[[nodiscard]] std::vector<std::uint8_t>
encode_dreamcast_maple_state(const DreamcastMapleStateSnapshot& state);
[[nodiscard]] DreamcastMapleStateSnapshot
decode_dreamcast_maple_state(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::shared_ptr<DreamcastMapleController>
map_dreamcast_maple_controller(Memory& memory,
                               EventScheduler& scheduler,
                               std::shared_ptr<MapleBus> bus,
                               MapleDmaTiming timing = {},
                               std::function<void()> completion_observer = {});

} // namespace katana::runtime
