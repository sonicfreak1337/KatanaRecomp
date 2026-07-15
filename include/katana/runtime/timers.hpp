#pragma once

#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace katana::runtime {

struct TmuTiming {
    std::uint64_t guest_cycles_per_peripheral_cycle = 4u;
    std::uint64_t guest_cycles_per_rtc_tick = 3'125'000u;
};

class Sh4Tmu final {
public:
    static constexpr std::size_t channel_count = 3u;
    static constexpr std::uint16_t underflow_flag = 0x0100u;
    static constexpr std::uint16_t underflow_interrupt_enable = 0x0020u;

    explicit Sh4Tmu(EventScheduler& scheduler, TmuTiming timing = {});
    ~Sh4Tmu();
    Sh4Tmu(const Sh4Tmu&) = delete;
    Sh4Tmu& operator=(const Sh4Tmu&) = delete;

    void write_constant(std::size_t channel, std::uint32_t value);
    void write_counter(std::size_t channel, std::uint32_t value);
    void write_control(std::size_t channel, std::uint16_t value);
    void write_start(std::uint8_t channel_mask);

    [[nodiscard]] std::uint32_t constant(std::size_t channel) const;
    [[nodiscard]] std::uint32_t counter(std::size_t channel);
    [[nodiscard]] std::uint16_t control(std::size_t channel) const;
    [[nodiscard]] std::uint8_t start() const noexcept;
    [[nodiscard]] bool interrupt_pending(std::size_t channel) const;
    void acknowledge_interrupt(std::size_t channel) noexcept;
    [[nodiscard]] std::uint64_t underflow_count(std::size_t channel) const;
    void reset() noexcept;

private:
    struct Channel {
        std::uint32_t constant = 0xFFFFFFFFu;
        std::uint32_t counter = 0xFFFFFFFFu;
        std::uint16_t control = 0u;
        std::uint64_t anchor_cycle = 0u;
        std::uint32_t anchor_counter = 0xFFFFFFFFu;
        std::uint64_t underflows = 0u;
        std::optional<SchedulerEventId> event;
        bool running = false;
        bool interrupt_pending = false;
    };

    [[nodiscard]] Channel& channel(std::size_t index);
    [[nodiscard]] const Channel& channel(std::size_t index) const;
    [[nodiscard]] std::uint64_t tick_period(const Channel& value) const;
    void synchronize(std::size_t index);
    void cancel_event(Channel& value) noexcept;
    void schedule_underflow(std::size_t index);
    void handle_underflow(std::size_t index);

    EventScheduler& scheduler_;
    TmuTiming timing_;
    std::array<Channel, channel_count> channels_{};
};

struct RtcDateTime {
    std::uint16_t year = 2000u;
    std::uint8_t month = 1u;
    std::uint8_t day = 1u;
    std::uint8_t day_of_week = 6u;
    std::uint8_t hour = 0u;
    std::uint8_t minute = 0u;
    std::uint8_t second = 0u;

    [[nodiscard]] bool operator==(const RtcDateTime&) const = default;
};

enum class RtcPeriodicRate : std::uint8_t {
    Disabled = 0u,
    Every1Over256Second = 1u,
    Every1Over64Second = 2u,
    Every1Over16Second = 3u,
    Every1Over4Second = 4u,
    Every1Over2Second = 5u,
    EverySecond = 6u,
    Every2Seconds = 7u,
};

class Sh4Rtc final {
public:
    explicit Sh4Rtc(
        EventScheduler& scheduler,
        std::uint64_t guest_cycles_per_second = 200'000'000u
    );
    ~Sh4Rtc();
    Sh4Rtc(const Sh4Rtc&) = delete;
    Sh4Rtc& operator=(const Sh4Rtc&) = delete;

    void set_date_time(const RtcDateTime& value);
    [[nodiscard]] const RtcDateTime& date_time() const noexcept;
    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    void reset_divider() noexcept;

    void set_periodic_rate(RtcPeriodicRate rate) noexcept;
    [[nodiscard]] RtcPeriodicRate periodic_rate() const noexcept;
    void set_carry_interrupt_enabled(bool enabled) noexcept;
    [[nodiscard]] std::uint8_t counter_64hz() const noexcept;
    [[nodiscard]] bool periodic_interrupt_pending() const noexcept;
    [[nodiscard]] bool carry_interrupt_pending() const noexcept;
    void acknowledge_periodic_interrupt() noexcept;
    void acknowledge_carry_interrupt() noexcept;
    [[nodiscard]] std::uint64_t tick_count() const noexcept;
    [[nodiscard]] std::uint64_t periodic_event_count() const noexcept;

private:
    static void validate(const RtcDateTime& value);
    static bool leap_year(std::uint16_t year) noexcept;
    static std::uint8_t days_in_month(std::uint16_t year, std::uint8_t month) noexcept;
    static std::uint16_t periodic_ticks(RtcPeriodicRate rate) noexcept;
    void schedule_tick();
    void tick();
    void increment_second() noexcept;

    EventScheduler& scheduler_;
    std::uint64_t cycles_per_tick_ = 0u;
    RtcDateTime date_time_{};
    std::optional<SchedulerEventId> event_;
    RtcPeriodicRate periodic_rate_ = RtcPeriodicRate::Disabled;
    std::uint16_t divider_256hz_ = 0u;
    std::uint64_t ticks_ = 0u;
    std::uint64_t periodic_events_ = 0u;
    bool running_ = false;
    bool periodic_pending_ = false;
    bool carry_pending_ = false;
    bool carry_enabled_ = false;
};

} // namespace katana::runtime
