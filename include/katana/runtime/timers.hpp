#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_rtc_p4_base = 0xFFC80000u;
inline constexpr std::uint32_t sh4_rtc_area7_base = 0x1FC80000u;
inline constexpr std::size_t sh4_rtc_register_size = 0x40u;
inline constexpr std::uint32_t sh4_tmu_p4_base = 0xFFD80000u;
inline constexpr std::uint32_t sh4_tmu_area7_base = 0x1FD80000u;
inline constexpr std::size_t sh4_tmu_register_size = 0x30u;

class Sh4RtcClockDomain final {
  public:
    static constexpr std::uint64_t source_frequency_hz = 16'384u;
    using PhaseObserverId = std::uint64_t;
    using PhaseObserver = std::function<void(bool before_reset, std::uint64_t guest_cycle)>;

    explicit Sh4RtcClockDomain(
        std::uint64_t guest_cycles_per_second = dreamcast_guest_cycles_per_second);

    [[nodiscard]] std::uint64_t guest_cycles_per_second() const noexcept;
    [[nodiscard]] std::uint64_t elapsed_ticks(std::uint64_t first_cycle,
                                              std::uint64_t last_cycle) const;
    [[nodiscard]] std::uint64_t deadline_after(std::uint64_t guest_cycle,
                                               std::uint64_t source_ticks) const;
    [[nodiscard]] PhaseObserverId add_phase_observer(PhaseObserver observer);
    [[nodiscard]] bool remove_phase_observer(PhaseObserverId observer_id) noexcept;
    void reset_phase(std::uint64_t guest_cycle);

  private:
    [[nodiscard]] std::uint64_t ticks_at(std::uint64_t guest_cycle) const;
    [[nodiscard]] std::uint64_t cycle_at_or_after_tick(std::uint64_t tick) const;

    std::uint64_t guest_cycles_per_second_ = 0u;
    std::uint64_t epoch_cycle_ = 0u;
    PhaseObserverId next_observer_id_ = 1u;
    std::map<PhaseObserverId, PhaseObserver> observers_;
};

struct TmuTiming {
    std::uint64_t guest_cycles_per_peripheral_cycle = 4u;
    std::shared_ptr<Sh4RtcClockDomain> rtc_clock;
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
    };

    [[nodiscard]] Channel& channel(std::size_t index);
    [[nodiscard]] const Channel& channel(std::size_t index) const;
    [[nodiscard]] std::uint64_t tick_period(const Channel& value) const;
    [[nodiscard]] bool uses_rtc_clock(const Channel& value) const noexcept;
    void synchronize(std::size_t index);
    void cancel_event(Channel& value) noexcept;
    void schedule_underflow(std::size_t index);
    void handle_underflow(std::size_t index);
    void handle_scheduler_reset();
    void handle_rtc_phase_reset(bool before_reset, std::uint64_t guest_cycle);

    EventScheduler& scheduler_;
    TmuTiming timing_;
    SchedulerResetObserverId scheduler_reset_observer_ = 0u;
    Sh4RtcClockDomain::PhaseObserverId rtc_phase_observer_ = 0u;
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
    explicit Sh4Rtc(EventScheduler& scheduler,
                    std::uint64_t guest_cycles_per_second = dreamcast_guest_cycles_per_second);
    Sh4Rtc(EventScheduler& scheduler, std::shared_ptr<Sh4RtcClockDomain> clock);
    ~Sh4Rtc();
    Sh4Rtc(const Sh4Rtc&) = delete;
    Sh4Rtc& operator=(const Sh4Rtc&) = delete;

    void set_date_time(const RtcDateTime& value);
    [[nodiscard]] const RtcDateTime& date_time() const noexcept;
    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    void set_rtc_enabled(bool enabled);
    [[nodiscard]] bool rtc_enabled() const noexcept;
    void reset_divider();

    void set_periodic_rate(RtcPeriodicRate rate) noexcept;
    [[nodiscard]] RtcPeriodicRate periodic_rate() const noexcept;
    void set_carry_interrupt_enabled(bool enabled) noexcept;
    void set_alarm_interrupt_enabled(bool enabled) noexcept;
    void write_alarm_register(std::size_t index, std::uint8_t value);
    [[nodiscard]] std::uint8_t alarm_register(std::size_t index) const;
    [[nodiscard]] bool carry_flag() const noexcept;
    [[nodiscard]] std::uint8_t counter_64hz() const noexcept;
    [[nodiscard]] bool periodic_interrupt_pending() const noexcept;
    [[nodiscard]] bool carry_interrupt_pending() const noexcept;
    [[nodiscard]] bool alarm_interrupt_pending() const noexcept;
    [[nodiscard]] bool alarm_flag() const noexcept;
    void acknowledge_periodic_interrupt() noexcept;
    void acknowledge_carry_interrupt() noexcept;
    void acknowledge_alarm_interrupt() noexcept;
    [[nodiscard]] std::uint64_t tick_count() const noexcept;
    [[nodiscard]] std::uint64_t periodic_event_count() const noexcept;

  private:
    static void validate(const RtcDateTime& value);
    static bool leap_year(std::uint16_t year) noexcept;
    static std::uint8_t days_in_month(std::uint16_t year, std::uint8_t month) noexcept;
    static std::uint16_t periodic_ticks(RtcPeriodicRate rate) noexcept;
    void schedule_tick();
    void cancel_event() noexcept;
    void handle_scheduler_reset();
    void tick();
    void increment_second() noexcept;
    void update_alarm() noexcept;

    EventScheduler& scheduler_;
    std::shared_ptr<Sh4RtcClockDomain> clock_;
    SchedulerResetObserverId scheduler_reset_observer_ = 0u;
    RtcDateTime date_time_{};
    std::optional<SchedulerEventId> event_;
    RtcPeriodicRate periodic_rate_ = RtcPeriodicRate::Disabled;
    std::uint8_t divider_256hz_phase_ = 0u;
    std::uint8_t counter_64hz_ = 0u;
    std::uint64_t periodic_phase_ticks_ = 0u;
    std::uint64_t ticks_ = 0u;
    std::uint64_t periodic_events_ = 0u;
    bool calendar_running_ = false;
    bool rtc_enabled_ = false;
    bool periodic_pending_ = false;
    bool carry_flag_ = false;
    bool carry_enabled_ = false;
    std::array<std::uint8_t, 6u> alarm_registers_{};
    bool alarm_pending_ = false;
    bool alarm_enabled_ = false;
};

void map_sh4_tmu_registers(Memory& memory, const std::shared_ptr<Sh4Tmu>& tmu);
void map_sh4_rtc_registers(Memory& memory, const std::shared_ptr<Sh4Rtc>& rtc);

} // namespace katana::runtime
