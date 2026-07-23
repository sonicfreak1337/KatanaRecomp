#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_dmac_p4_base = 0xFFA00000u;
inline constexpr std::uint32_t sh4_dmac_area7_base = 0x1FA00000u;
inline constexpr std::size_t sh4_dmac_register_size = 0x44u;

struct DmaTiming {
    std::uint64_t guest_cycles_per_byte = 1u;
    std::size_t maximum_batch_units = 256u;

    [[nodiscard]] bool operator==(const DmaTiming&) const = default;
};

enum class DmaExecutionMode : std::uint8_t { SingleUnitReference, DeterministicBatch };

struct DmaPerformanceCounters {
    std::uint64_t scheduler_callbacks = 0u;
    std::uint64_t completed_batches = 0u;

    [[nodiscard]] bool operator==(const DmaPerformanceCounters&) const = default;
};

enum class DmaFaultReason : std::uint8_t {
    None,
    InvalidTransferSize,
    InvalidRequestSource,
    ProhibitedAddressMode,
    MisalignedAddress,
    MemoryAccess,
    ExternalContractMismatch,
};

struct DmaFault {
    DmaFaultReason reason = DmaFaultReason::None;
    std::size_t channel = 0u;
    std::uint32_t source = 0u;
    std::uint32_t destination = 0u;
    std::size_t transfer_size = 0u;

    [[nodiscard]] bool operator==(const DmaFault&) const = default;
};

struct Sh4DmacChannelSnapshot {
    std::uint32_t source = 0u;
    std::uint32_t destination = 0u;
    std::uint32_t count = 0u;
    std::uint32_t control = 0u;
    std::uint32_t pending_requests = 0u;
    std::uint32_t pending_on_demand_requests = 0u;
    std::uint64_t completed_units = 0u;
    bool interrupt_pending = false;

    [[nodiscard]] bool operator==(const Sh4DmacChannelSnapshot&) const = default;
};

struct Sh4DmacSnapshot {
    std::array<Sh4DmacChannelSnapshot, 4u> channels{};
    std::uint32_t operation = 0u;
    DmaTiming timing;
    DmaExecutionMode execution_mode = DmaExecutionMode::SingleUnitReference;
    std::optional<SchedulerEventId> event_id;
    std::optional<std::size_t> scheduled_channel;
    std::size_t scheduled_units = 0u;
    std::optional<DmaFault> last_fault;
    std::optional<std::size_t> last_on_demand_channel;
    std::size_t round_robin_cursor = 0u;
    DmaPerformanceCounters performance_counters;

    [[nodiscard]] bool operator==(const Sh4DmacSnapshot&) const = default;
};

class Sh4Dmac final {
  public:
    static constexpr std::size_t channel_count = 4u;
    static constexpr std::uint32_t channel_enable = 0x00000001u;
    static constexpr std::uint32_t transfer_end = 0x00000002u;
    static constexpr std::uint32_t interrupt_enable = 0x00000004u;
    static constexpr std::uint32_t master_enable = 0x00000001u;
    static constexpr std::uint32_t nmi_flag = 0x00000002u;
    static constexpr std::uint32_t address_error_flag = 0x00000004u;
    static constexpr std::uint32_t on_demand_enable = 0x00008000u;

    Sh4Dmac(EventScheduler& scheduler,
            Memory& memory,
            DmaTiming timing = {},
            DmaExecutionMode execution_mode = DmaExecutionMode::SingleUnitReference);
    ~Sh4Dmac();
    Sh4Dmac(const Sh4Dmac&) = delete;
    Sh4Dmac& operator=(const Sh4Dmac&) = delete;

    void write_source(std::size_t channel, std::uint32_t value);
    void write_destination(std::size_t channel, std::uint32_t value);
    void write_count(std::size_t channel, std::uint32_t value);
    void write_control(std::size_t channel, std::uint32_t value);
    void write_operation(std::uint32_t value);

    [[nodiscard]] std::uint32_t source(std::size_t channel) const;
    [[nodiscard]] std::uint32_t destination(std::size_t channel) const;
    [[nodiscard]] std::uint32_t count(std::size_t channel) const;
    [[nodiscard]] std::uint32_t control(std::size_t channel) const;
    [[nodiscard]] std::uint32_t operation() const noexcept;

    void request_transfer(std::size_t channel, std::uint32_t requests = 1u);
    [[nodiscard]] bool request_on_demand_transfer(std::size_t channel);
    [[nodiscard]] bool repeat_on_demand_transfer();
    [[nodiscard]] std::uint32_t pending_on_demand_requests(std::size_t channel) const;
    [[nodiscard]] std::size_t transfer_unit_size(std::size_t channel) const;
    void set_completion_observer(std::function<void(std::size_t)> observer);
    void set_fault_observer(std::function<void(const DmaFault&)> observer);
    [[nodiscard]] bool validate_external_transfer(std::size_t channel,
                                                  std::uint32_t source,
                                                  std::size_t bytes,
                                                  std::size_t unit_size,
                                                  std::uint8_t expected_request_source = 8u) noexcept;
    void complete_external_transfer(std::size_t channel, std::size_t bytes) noexcept;
    void report_external_fault(std::size_t channel,
                               DmaFaultReason reason,
                               std::size_t unit_size) noexcept;
    void signal_nmi() noexcept;
    [[nodiscard]] bool interrupt_pending(std::size_t channel) const;
    [[nodiscard]] bool address_error() const noexcept;
    [[nodiscard]] const std::optional<DmaFault>& last_fault() const noexcept;
    [[nodiscard]] std::uint64_t completed_transfer_units(std::size_t channel) const;
    [[nodiscard]] DmaExecutionMode execution_mode() const noexcept;
    void set_execution_mode(DmaExecutionMode mode);
    [[nodiscard]] const DmaPerformanceCounters& performance_counters() const noexcept;
    [[nodiscard]] Sh4DmacSnapshot snapshot() const noexcept;
    void reset_performance_counters() noexcept;
    void reset() noexcept;

  private:
    struct Channel {
        std::uint32_t source = 0u;
        std::uint32_t destination = 0u;
        std::uint32_t count = 0u;
        std::uint32_t control = 0u;
        std::uint32_t pending_requests = 0u;
        std::uint32_t pending_on_demand_requests = 0u;
        std::uint64_t completed_units = 0u;
        bool interrupt_pending = false;
    };

    [[nodiscard]] Channel& channel(std::size_t index);
    [[nodiscard]] const Channel& channel(std::size_t index) const;
    [[nodiscard]] bool enabled(std::size_t index) const noexcept;
    [[nodiscard]] bool automatic(const Channel& value) const noexcept;
    [[nodiscard]] std::optional<std::size_t> select_channel() const noexcept;
    [[nodiscard]] static std::size_t transfer_size(const Channel& value) noexcept;
    [[nodiscard]] static std::uint8_t address_mode(std::uint32_t control, unsigned shift) noexcept;
    void reevaluate();
    void discard_external_requests() noexcept;
    void discard_on_demand_requests() noexcept;
    void cancel_event() noexcept;
    void schedule(std::size_t index);
    [[nodiscard]] std::size_t batch_units(std::size_t index, std::size_t size) const noexcept;
    void handle_scheduler_reset();
    void handle_transfer(std::size_t index);
    bool transfer_one(std::size_t index, std::size_t size) noexcept;
    void update_addresses(Channel& value, std::size_t size) noexcept;
    void set_fault(std::size_t index, DmaFaultReason reason, std::size_t size) noexcept;

    EventScheduler& scheduler_;
    SchedulerLifetimeToken scheduler_lifetime_;
    Memory& memory_;
    DmaTiming timing_;
    DmaExecutionMode execution_mode_ = DmaExecutionMode::SingleUnitReference;
    SchedulerResetObserverId scheduler_reset_observer_ = 0u;
    std::array<Channel, channel_count> channels_{};
    std::uint32_t operation_ = 0u;
    std::optional<SchedulerEventId> event_;
    std::optional<std::size_t> scheduled_channel_;
    std::size_t scheduled_units_ = 0u;
    std::optional<DmaFault> last_fault_;
    std::optional<std::size_t> last_on_demand_channel_;
    mutable std::size_t round_robin_cursor_ = 0u;
    DmaPerformanceCounters performance_counters_;
    std::function<void(std::size_t)> completion_observer_;
    std::function<void(const DmaFault&)> fault_observer_;
};

[[nodiscard]] std::shared_ptr<Sh4Dmac>
map_sh4_dmac_registers(Memory& memory, EventScheduler& scheduler, DmaTiming timing = {});
void map_sh4_dmac_registers(Memory& memory, const std::shared_ptr<Sh4Dmac>& dmac);

} // namespace katana::runtime
