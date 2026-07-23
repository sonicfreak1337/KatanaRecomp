#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_scif_p4_base = 0xFFE80000u;
inline constexpr std::uint32_t sh4_scif_area7_base = 0x1FE80000u;
inline constexpr std::size_t sh4_scif_register_size = 0x28u;

enum class Sh4ScifInterrupt : std::uint8_t { Error, Receive, Break, Transmit };

struct Sh4ScifSnapshot {
    std::optional<SchedulerEventId> transmit_event;
    std::vector<std::uint8_t> transmit_fifo;
    std::vector<std::uint8_t> receive_fifo;
    std::vector<std::uint8_t> transmitted_bytes;
    std::uint16_t mode = 0u;
    std::uint8_t bit_rate = 0u;
    std::uint16_t control = 0u;
    std::uint16_t status = 0u;
    std::uint16_t status_last_read = 0u;
    std::uint16_t fifo_control = 0u;
    std::uint16_t port = 0u;
    std::uint16_t line_status = 0u;

    [[nodiscard]] bool operator==(const Sh4ScifSnapshot&) const = default;
};

class Sh4Scif final {
  public:
    using InterruptObserver = std::function<void(Sh4ScifInterrupt, bool)>;
    using TransmitObserver = std::function<void(std::uint8_t)>;

    explicit Sh4Scif(EventScheduler& scheduler,
                     InterruptObserver interrupt_observer = {},
                     TransmitObserver transmit_observer = {});
    ~Sh4Scif();
    Sh4Scif(const Sh4Scif&) = delete;
    Sh4Scif& operator=(const Sh4Scif&) = delete;

    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width);
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    void inject_receive(std::uint8_t value);
    void inject_break() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t transmit_fifo_size() const noexcept;
    [[nodiscard]] std::size_t receive_fifo_size() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& transmitted_bytes() const noexcept;
    [[nodiscard]] Sh4ScifSnapshot snapshot() const;

  private:
    static constexpr std::uint16_t status_error = 0x0080u;
    static constexpr std::uint16_t status_transmit_end = 0x0040u;
    static constexpr std::uint16_t status_transmit_empty = 0x0020u;
    static constexpr std::uint16_t status_break = 0x0010u;
    static constexpr std::uint16_t status_framing_error = 0x0008u;
    static constexpr std::uint16_t status_parity_error = 0x0004u;
    static constexpr std::uint16_t status_receive_full = 0x0002u;
    static constexpr std::uint16_t status_receive_ready = 0x0001u;

    [[nodiscard]] std::uint64_t frame_cycles() const;
    [[nodiscard]] std::size_t receive_trigger() const noexcept;
    [[nodiscard]] std::size_t transmit_trigger() const noexcept;
    void refresh_status() noexcept;
    void update_interrupts() noexcept;
    void schedule_transmit();
    void complete_transmit(SchedulerEventId event_id);
    void cancel_transmit() noexcept;
    void handle_scheduler_reset() noexcept;

    EventScheduler& scheduler_;
    InterruptObserver interrupt_observer_;
    TransmitObserver transmit_observer_;
    SchedulerLifetimeToken scheduler_lifetime_;
    SchedulerResetObserverId reset_observer_ = 0u;
    std::optional<SchedulerEventId> transmit_event_;
    std::deque<std::uint8_t> transmit_fifo_;
    std::deque<std::uint8_t> receive_fifo_;
    std::vector<std::uint8_t> transmitted_bytes_;
    std::uint16_t mode_ = 0u;
    std::uint8_t bit_rate_ = 0xFFu;
    std::uint16_t control_ = 0u;
    std::uint16_t status_ = status_transmit_end | status_transmit_empty;
    std::uint16_t status_last_read_ = 0u;
    std::uint16_t fifo_control_ = 0u;
    std::uint16_t port_ = 0u;
    std::uint16_t line_status_ = 0u;
};

[[nodiscard]] std::shared_ptr<Sh4Scif>
map_sh4_scif(Memory& memory,
             EventScheduler& scheduler,
             Sh4Scif::InterruptObserver interrupt_observer = {},
             Sh4Scif::TransmitObserver transmit_observer = {});

} // namespace katana::runtime
