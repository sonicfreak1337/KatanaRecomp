#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"
#include "katana/runtime/system_asic.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace katana::runtime {

class Sh4Dmac;

inline constexpr std::uint32_t g1_mmio_physical_base = 0x005F7400u;
inline constexpr std::uint32_t g2_mmio_physical_base = 0x005F7800u;
inline constexpr std::uint32_t pvr_dma_mmio_physical_base = 0x005F7C00u;
inline constexpr std::uint32_t holly_dma_register_size = 0x100u;

struct HollyDmaTiming {
    std::uint64_t cycles_per_byte = 4u;

    [[nodiscard]] bool operator==(const HollyDmaTiming&) const = default;
};

enum class HollyDmaFaultReason : std::uint8_t {
    None,
    IllegalAddress,
    Overrun,
    Timeout,
    InvalidLength,
    InvalidDirection,
    InvalidTrigger,
    MissingBackend,
    SchedulerFailure,
    TransferFailure,
    HandshakeMismatch,
};

struct HollyDmaFault {
    HollyDmaFaultReason reason = HollyDmaFaultReason::None;
    std::optional<SystemAsicEvent> event;
    std::size_t channel = 0u;
    std::uint32_t peripheral_address = 0u;
    std::uint32_t system_address = 0u;
    std::uint32_t remaining = 0u;

    [[nodiscard]] bool operator==(const HollyDmaFault&) const = default;
};

enum class G1DmaFaultPhase : std::uint8_t {
    Start,
    Chunk,
};

struct G1DmaFault {
    HollyDmaFaultReason reason = HollyDmaFaultReason::None;
    std::uint32_t fault_address = 0u;
    std::uint32_t transferred_bytes = 0u;
    std::uint32_t residue = 0u;
    G1DmaFaultPhase phase = G1DmaFaultPhase::Start;
    bool operator==(const G1DmaFault&) const = default;
};

struct HollyDmaChannelState {
    std::uint32_t peripheral_address = 0u;
    std::uint32_t system_address = 0u;
    std::uint32_t length = 0u;
    std::uint32_t direction = 0u;
    std::uint32_t trigger_select = 0u;
    std::uint32_t enabled = 0u;
    std::uint32_t active = 0u;
    std::uint32_t suspend = 0u;
    std::uint32_t peripheral_counter = 0u;
    std::uint32_t system_counter = 0u;
    std::uint32_t remaining = 0u;
    std::uint64_t completion_cycle = 0u;
    std::uint64_t remaining_cycles = 0u;
    std::optional<SchedulerEventId> completion_event;
    HollyDmaFaultReason fault = HollyDmaFaultReason::None;
    std::uint64_t fault_count = 0u;

    [[nodiscard]] bool operator==(const HollyDmaChannelState&) const = default;
};

struct DreamcastG2DmaSnapshot {
    std::array<HollyDmaChannelState, 4u> channels{};
    HollyDmaTiming timing;
    std::uint32_t address_protect = 0u;
    std::uint32_t ds_timeout = 0u;
    std::uint32_t tr_timeout = 0u;
    std::uint32_t modem_timeout = 0u;
    std::uint32_t modem_wait = 0u;
    std::uint64_t completed_dma_count = 0u;
    std::optional<HollyDmaFault> last_fault;
    SchedulerResetObserverId reset_observer = 0u;
    bool completion_observer_bound = false;

    [[nodiscard]] bool operator==(const DreamcastG2DmaSnapshot&) const = default;
};

struct DreamcastG1DmaSnapshot {
    HollyDmaChannelState channel;
    HollyDmaTiming timing;
    std::uint32_t bios_handoff_live_address = 0u;
    std::uint32_t system_mode = 0u;
    std::uint32_t gdrom_read_access_timing = 0u;
    std::uint32_t address_protect = 0u;
    std::optional<HollyDmaFault> last_fault;
    std::optional<G1DmaFault> last_g1_fault;
    SchedulerResetObserverId reset_observer = 0u;
    bool transfer_handler_bound = false;
    bool completion_observer_bound = false;
    bool range_validator_bound = false;
    bool fault_observer_bound = false;

    [[nodiscard]] bool operator==(const DreamcastG1DmaSnapshot&) const = default;
};

struct DreamcastPvrDmaSnapshot {
    HollyDmaChannelState channel;
    HollyDmaTiming timing;
    std::uint32_t address_protect = 0u;
    std::optional<HollyDmaFault> last_fault;
    SchedulerResetObserverId reset_observer = 0u;
    std::size_t dmac_channel = 0u;
    bool dmac_bound = false;
    bool dmac_contract_required = false;
    bool completion_observer_bound = false;

    [[nodiscard]] bool operator==(const DreamcastPvrDmaSnapshot&) const = default;
};

class DreamcastG2DmaController final {
  public:
    DreamcastG2DmaController(Memory& memory,
                             EventScheduler& scheduler,
                             HollyDmaTiming timing = {},
                             std::function<void(SystemAsicEvent)> completion_observer = {});
    ~DreamcastG2DmaController();
    DreamcastG2DmaController(const DreamcastG2DmaController&) = delete;
    DreamcastG2DmaController& operator=(const DreamcastG2DmaController&) = delete;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t completed_dma_count() const noexcept;
    [[nodiscard]] const HollyDmaChannelState& channel_state(std::size_t channel) const;
    [[nodiscard]] const std::optional<HollyDmaFault>& last_fault() const noexcept;
    [[nodiscard]] DreamcastG2DmaSnapshot snapshot() const;
    void hardware_trigger(std::size_t channel);
    void interrupt_trigger(SystemAsicEvent event);

  private:
    void arm(std::size_t channel);
    void start(std::size_t channel);
    void schedule_completion(std::size_t channel, std::uint64_t cycles);
    void set_suspended(std::size_t channel, bool suspended);
    void complete(std::size_t channel, SchedulerEventId event_id);
    void cancel_events() noexcept;
    void handle_scheduler_reset() noexcept;
    void fail(std::size_t channel, HollyDmaFaultReason reason, SystemAsicEvent event) noexcept;
    [[nodiscard]] bool protected_system_range(std::uint32_t address,
                                              std::size_t size) const noexcept;
    Memory& memory_;
    EventScheduler& scheduler_;
    HollyDmaTiming timing_;
    std::function<void(SystemAsicEvent)> completion_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::array<HollyDmaChannelState, 4u> channels_{};
    std::uint32_t address_protect_ = 0x00007F00u;
    std::uint32_t ds_timeout_ = 0u;
    std::uint32_t tr_timeout_ = 0u;
    std::uint32_t modem_timeout_ = 0u;
    std::uint32_t modem_wait_ = 0u;
    std::uint64_t completed_dma_count_ = 0u;
    std::optional<HollyDmaFault> last_fault_;
};

class DreamcastG1BusController final {
  public:
    static constexpr std::uint32_t transfer_alignment = 32u;
    static constexpr std::uint32_t transfer_chunk_bytes = 2048u;
    static constexpr std::uint32_t maximum_transfer_bytes = 0x02000000u;
    static constexpr std::uint32_t address_protect_key = 0x8843u;
    using TransferHandler =
        std::function<void(std::uint32_t address, std::uint32_t length, std::uint32_t direction)>;
    using RangeValidator = std::function<bool(std::uint32_t address, std::size_t length)>;
    using FaultObserver = std::function<void(const G1DmaFault&)>;
    DreamcastG1BusController(EventScheduler& scheduler,
                             HollyDmaTiming timing,
                             TransferHandler transfer_handler,
                             std::function<void(SystemAsicEvent)> completion_observer,
                             RangeValidator range_validator = {});
    ~DreamcastG1BusController();
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    [[nodiscard]] bool
    begin_transfer(std::uint32_t address, std::uint32_t length, std::uint32_t direction);
    void abort_transfer() noexcept;
    void configure_bios_handoff(std::uint32_t live_address) noexcept;
    void restore_bios_handoff() noexcept;
    void reset() noexcept;
    [[nodiscard]] HollyDmaChannelState state() const noexcept;
    [[nodiscard]] const std::optional<HollyDmaFault>& last_fault() const noexcept;
    [[nodiscard]] const std::optional<G1DmaFault>& last_g1_fault() const noexcept;
    void set_fault_observer(FaultObserver observer);
    [[nodiscard]] std::uint32_t gdrom_read_access_timing() const noexcept;
    [[nodiscard]] std::uint32_t address_protect() const noexcept;
    [[nodiscard]] DreamcastG1DmaSnapshot snapshot() const noexcept;

  private:
    void schedule_chunk(G1DmaFaultPhase failure_phase);
    void complete_chunk(SchedulerEventId event_id);
    void handle_scheduler_reset() noexcept;
    void fail(HollyDmaFaultReason reason,
              std::optional<SystemAsicEvent> event = std::nullopt,
              G1DmaFaultPhase phase = G1DmaFaultPhase::Start,
              std::optional<std::uint32_t> fault_address = std::nullopt) noexcept;
    [[nodiscard]] bool protected_system_range(std::uint32_t address,
                                              std::uint32_t size) const noexcept;
    EventScheduler& scheduler_;
    HollyDmaTiming timing_;
    TransferHandler transfer_handler_;
    std::function<void(SystemAsicEvent)> completion_observer_;
    RangeValidator range_validator_;
    FaultObserver fault_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> completion_event_;
    std::uint32_t configured_address_ = 0u;
    std::uint32_t configured_length_ = 0u;
    std::uint32_t live_address_ = 0u;
    std::uint32_t transferred_length_ = 0u;
    std::uint32_t remaining_length_ = 0u;
    std::uint32_t bios_handoff_live_address_ = 0u;
    std::uint32_t dma_direction_ = 0u;
    std::uint32_t dma_enabled_ = 0u;
    std::uint32_t dma_active_ = 0u;
    std::uint32_t system_mode_ = 1u;
    std::uint32_t gdrom_read_access_timing_ = 0u;
    std::uint32_t address_protect_ = 0x0000407Fu;
    std::uint64_t next_chunk_cycle_ = 0u;
    HollyDmaFaultReason fault_ = HollyDmaFaultReason::None;
    std::uint64_t fault_count_ = 0u;
    std::optional<HollyDmaFault> last_fault_;
    std::optional<G1DmaFault> last_g1_fault_;
};

class DreamcastPvrDmaController final {
  public:
    DreamcastPvrDmaController(Memory& memory,
                              EventScheduler& scheduler,
                              HollyDmaTiming timing = {},
                              std::function<void(SystemAsicEvent)> completion_observer = {});
    ~DreamcastPvrDmaController();
    DreamcastPvrDmaController(const DreamcastPvrDmaController&) = delete;
    DreamcastPvrDmaController& operator=(const DreamcastPvrDmaController&) = delete;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void hardware_trigger();
    void bind_sh4_dmac(std::shared_ptr<Sh4Dmac> dmac, std::size_t channel = 0u);
    void reset() noexcept;
    [[nodiscard]] HollyDmaChannelState state() const noexcept;
    [[nodiscard]] const std::optional<HollyDmaFault>& last_fault() const noexcept;
    [[nodiscard]] DreamcastPvrDmaSnapshot snapshot() const noexcept;

  private:
    void start();
    void complete(SchedulerEventId event_id);
    void cancel() noexcept;
    void handle_scheduler_reset() noexcept;
    void fail(HollyDmaFaultReason reason, SystemAsicEvent event) noexcept;
    [[nodiscard]] bool protected_system_range(std::uint32_t address,
                                              std::size_t size) const noexcept;
    Memory& memory_;
    EventScheduler& scheduler_;
    HollyDmaTiming timing_;
    std::function<void(SystemAsicEvent)> completion_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> completion_event_;
    std::uint32_t pvr_address_ = 0u;
    std::uint32_t system_address_ = 0u;
    std::uint32_t length_ = 0u;
    std::uint32_t direction_ = 0u;
    std::uint32_t trigger_select_ = 0u;
    std::uint32_t enabled_ = 0u;
    std::uint32_t active_ = 0u;
    std::uint32_t address_protect_ = 0x00007F00u;
    std::uint32_t pvr_counter_ = 0u;
    std::uint32_t system_counter_ = 0u;
    std::uint32_t remaining_ = 0u;
    HollyDmaFaultReason fault_ = HollyDmaFaultReason::None;
    std::uint64_t fault_count_ = 0u;
    std::optional<HollyDmaFault> last_fault_;
    std::weak_ptr<Sh4Dmac> dmac_;
    std::size_t dmac_channel_ = 0u;
    bool dmac_contract_required_ = false;
};

struct DreamcastHollyDmaControllers {
    std::shared_ptr<DreamcastG1BusController> g1;
    std::shared_ptr<DreamcastG2DmaController> g2;
    std::shared_ptr<DreamcastPvrDmaController> pvr;
};

[[nodiscard]] DreamcastHollyDmaControllers
map_dreamcast_holly_dma(Memory& memory,
                        EventScheduler& scheduler,
                        HollyDmaTiming timing = {},
                        std::function<void(SystemAsicEvent)> completion_observer = {},
                        DreamcastG1BusController::TransferHandler g1_transfer_handler = {});

} // namespace katana::runtime
