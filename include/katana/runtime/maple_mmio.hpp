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

  private:
    struct PendingResponse {
        std::uint32_t destination = 0u;
        std::vector<std::uint32_t> words;
    };

    void start_dma();
    void complete_dma(SchedulerEventId event_id);
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
    std::uint32_t system_control_ = 0u;
    std::uint32_t address_protect_ = 0u;
    std::uint32_t msb_select_ = 1u;
    std::uint32_t tx_address_ = 0u;
    std::uint32_t rx_address_ = 0u;
    std::uint32_t rx_base_ = 0u;
    std::uint64_t completed_dma_count_ = 0u;
    std::uint64_t transferred_word_count_ = 0u;
    bool hard_trigger_failed_ = false;
};

[[nodiscard]] std::shared_ptr<DreamcastMapleController>
map_dreamcast_maple_controller(Memory& memory,
                               EventScheduler& scheduler,
                               std::shared_ptr<MapleBus> bus,
                               MapleDmaTiming timing = {},
                               std::function<void()> completion_observer = {});

} // namespace katana::runtime
