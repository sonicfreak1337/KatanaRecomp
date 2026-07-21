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

inline constexpr std::uint32_t g1_mmio_physical_base = 0x005F7400u;
inline constexpr std::uint32_t g2_mmio_physical_base = 0x005F7800u;
inline constexpr std::uint32_t pvr_dma_mmio_physical_base = 0x005F7C00u;
inline constexpr std::uint32_t holly_dma_register_size = 0x100u;

struct HollyDmaTiming {
    std::uint64_t cycles_per_byte = 4u;
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
};

class DreamcastG1BusController final {
  public:
    using TransferHandler =
        std::function<void(std::uint32_t address, std::uint32_t length, std::uint32_t direction)>;
    DreamcastG1BusController(EventScheduler& scheduler,
                             HollyDmaTiming timing,
                             TransferHandler transfer_handler,
                             std::function<void(SystemAsicEvent)> completion_observer);
    ~DreamcastG1BusController();
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] HollyDmaChannelState state() const noexcept;

  private:
    void start();
    void complete(SchedulerEventId event_id);
    void handle_scheduler_reset() noexcept;
    EventScheduler& scheduler_;
    HollyDmaTiming timing_;
    TransferHandler transfer_handler_;
    std::function<void(SystemAsicEvent)> completion_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> completion_event_;
    std::uint32_t dma_address_ = 0u;
    std::uint32_t dma_length_ = 0u;
    std::uint32_t dma_direction_ = 0u;
    std::uint32_t dma_enabled_ = 0u;
    std::uint32_t dma_active_ = 0u;
    std::uint32_t system_mode_ = 1u;
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
    void reset() noexcept;
    [[nodiscard]] HollyDmaChannelState state() const noexcept;

  private:
    void start();
    void complete(SchedulerEventId event_id);
    void cancel() noexcept;
    void handle_scheduler_reset() noexcept;
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
