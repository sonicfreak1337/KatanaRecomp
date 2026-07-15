#include "katana/runtime/timers.hpp"

#include <limits>
#include <stdexcept>

namespace katana::runtime {

namespace {

std::uint64_t checked_multiply(const std::uint64_t left, const std::uint64_t right) {
    if (right != 0u && left > std::numeric_limits<std::uint64_t>::max() / right) {
        throw std::overflow_error("TMU-Ereignisfrist ist uebergelaufen.");
    }
    return left * right;
}

} // namespace

Sh4Tmu::Sh4Tmu(EventScheduler& scheduler, const TmuTiming timing)
    : scheduler_(scheduler), timing_(timing) {
    if (timing_.guest_cycles_per_peripheral_cycle == 0u ||
        timing_.guest_cycles_per_rtc_tick == 0u) {
        throw std::invalid_argument("TMU-Taktverhaeltnisse muessen groesser null sein.");
    }
}

Sh4Tmu::~Sh4Tmu() {
    for (auto& value : channels_) {
        cancel_event(value);
    }
}

Sh4Tmu::Channel& Sh4Tmu::channel(const std::size_t index) {
    if (index >= channels_.size()) {
        throw std::out_of_range("Ungueltiger TMU-Kanal.");
    }
    return channels_[index];
}

const Sh4Tmu::Channel& Sh4Tmu::channel(const std::size_t index) const {
    if (index >= channels_.size()) {
        throw std::out_of_range("Ungueltiger TMU-Kanal.");
    }
    return channels_[index];
}

std::uint64_t Sh4Tmu::tick_period(const Channel& value) const {
    static constexpr std::array<std::uint64_t, 5> divisors = {4u, 16u, 64u, 256u, 1024u};
    const auto source = static_cast<std::uint8_t>(value.control & 0x7u);
    if (source < divisors.size()) {
        return checked_multiply(timing_.guest_cycles_per_peripheral_cycle, divisors[source]);
    }
    if (source == 6u) {
        return timing_.guest_cycles_per_rtc_tick;
    }
    throw std::invalid_argument("Reservierter oder externer TMU-Takt ist nicht deterministisch konfiguriert.");
}

void Sh4Tmu::synchronize(const std::size_t index) {
    auto& value = channel(index);
    if (!value.running) {
        return;
    }
    const auto period = tick_period(value);
    const auto elapsed = scheduler_.current_cycle() - value.anchor_cycle;
    const auto ticks = elapsed / period;
    if (ticks == 0u) {
        return;
    }
    value.counter = static_cast<std::uint32_t>(value.anchor_counter - ticks);
    value.anchor_counter = value.counter;
    value.anchor_cycle += ticks * period;
}

void Sh4Tmu::cancel_event(Channel& value) noexcept {
    if (value.event) {
        static_cast<void>(scheduler_.cancel(*value.event));
        value.event.reset();
    }
}

void Sh4Tmu::schedule_underflow(const std::size_t index) {
    auto& value = channel(index);
    cancel_event(value);
    value.anchor_cycle = scheduler_.current_cycle();
    value.anchor_counter = value.counter;
    const auto ticks = static_cast<std::uint64_t>(value.counter) + 1u;
    const auto delay = checked_multiply(ticks, tick_period(value));
    value.event = scheduler_.schedule_after(delay, [this, index](const auto, const auto) {
        handle_underflow(index);
    });
}

void Sh4Tmu::handle_underflow(const std::size_t index) {
    auto& value = channel(index);
    value.event.reset();
    value.counter = value.constant;
    value.anchor_counter = value.counter;
    value.anchor_cycle = scheduler_.current_cycle();
    value.control |= underflow_flag;
    ++value.underflows;
    value.interrupt_pending = (value.control & underflow_interrupt_enable) != 0u;
    if (value.running) {
        schedule_underflow(index);
    }
}

void Sh4Tmu::write_constant(const std::size_t index, const std::uint32_t new_value) {
    channel(index).constant = new_value;
}

void Sh4Tmu::write_counter(const std::size_t index, const std::uint32_t new_value) {
    auto& value = channel(index);
    value.counter = new_value;
    value.anchor_counter = new_value;
    value.anchor_cycle = scheduler_.current_cycle();
    if (value.running) {
        schedule_underflow(index);
    }
}

void Sh4Tmu::write_control(const std::size_t index, const std::uint16_t new_value) {
    const auto source = static_cast<std::uint8_t>(new_value & 0x7u);
    if (source == 5u || source == 7u) {
        throw std::invalid_argument("Reservierter oder externer TMU-Takt ist nicht unterstuetzt.");
    }
    synchronize(index);
    auto& value = channel(index);
    const auto retained_flag = static_cast<std::uint16_t>(value.control & new_value & underflow_flag);
    value.control = static_cast<std::uint16_t>((new_value & 0x003Fu) | retained_flag);
    value.interrupt_pending =
        (value.control & (underflow_flag | underflow_interrupt_enable)) ==
        (underflow_flag | underflow_interrupt_enable);
    if (value.running) {
        schedule_underflow(index);
    }
}

void Sh4Tmu::write_start(const std::uint8_t channel_mask) {
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        auto& value = channels_[index];
        const bool should_run = (channel_mask & (1u << index)) != 0u;
        if (value.running == should_run) {
            continue;
        }
        if (!should_run) {
            synchronize(index);
            value.running = false;
            cancel_event(value);
            continue;
        }
        value.running = true;
        schedule_underflow(index);
    }
}

std::uint32_t Sh4Tmu::constant(const std::size_t index) const { return channel(index).constant; }
std::uint32_t Sh4Tmu::counter(const std::size_t index) {
    synchronize(index);
    return channel(index).counter;
}
std::uint16_t Sh4Tmu::control(const std::size_t index) const { return channel(index).control; }
std::uint8_t Sh4Tmu::start() const noexcept {
    std::uint8_t result = 0u;
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        if (channels_[index].running) {
            result |= static_cast<std::uint8_t>(1u << index);
        }
    }
    return result;
}
bool Sh4Tmu::interrupt_pending(const std::size_t index) const { return channel(index).interrupt_pending; }
void Sh4Tmu::acknowledge_interrupt(const std::size_t index) noexcept {
    if (index < channels_.size()) { channels_[index].interrupt_pending = false; }
}
std::uint64_t Sh4Tmu::underflow_count(const std::size_t index) const { return channel(index).underflows; }

void Sh4Tmu::reset() noexcept {
    for (auto& value : channels_) {
        cancel_event(value);
        value = Channel{};
    }
}

Sh4Rtc::Sh4Rtc(EventScheduler& scheduler, const std::uint64_t guest_cycles_per_second)
    : scheduler_(scheduler) {
    if (guest_cycles_per_second == 0u || guest_cycles_per_second % 256u != 0u) {
        throw std::invalid_argument("RTC-Gastzyklen pro Sekunde muessen positiv und durch 256 teilbar sein.");
    }
    cycles_per_tick_ = guest_cycles_per_second / 256u;
}

Sh4Rtc::~Sh4Rtc() { stop(); }

bool Sh4Rtc::leap_year(const std::uint16_t year) noexcept {
    return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

std::uint8_t Sh4Rtc::days_in_month(const std::uint16_t year, const std::uint8_t month) noexcept {
    static constexpr std::array<std::uint8_t, 12> days = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
    };
    return static_cast<std::uint8_t>(days[month - 1u] + (month == 2u && leap_year(year) ? 1u : 0u));
}

void Sh4Rtc::validate(const RtcDateTime& value) {
    if (value.year > 9999u || value.month < 1u || value.month > 12u ||
        value.day < 1u || value.day > days_in_month(value.year, value.month) ||
        value.day_of_week > 6u || value.hour > 23u || value.minute > 59u ||
        value.second > 59u) {
        throw std::invalid_argument("Ungueltiger RTC-Kalenderzustand.");
    }
}

void Sh4Rtc::set_date_time(const RtcDateTime& value) {
    if (running_) {
        throw std::logic_error("RTC-Kalender darf nur im gestoppten Zustand gesetzt werden.");
    }
    validate(value);
    date_time_ = value;
}

const RtcDateTime& Sh4Rtc::date_time() const noexcept { return date_time_; }

void Sh4Rtc::start() {
    if (running_) { return; }
    running_ = true;
    schedule_tick();
}

void Sh4Rtc::stop() noexcept {
    running_ = false;
    if (event_) {
        static_cast<void>(scheduler_.cancel(*event_));
        event_.reset();
    }
}

bool Sh4Rtc::running() const noexcept { return running_; }
void Sh4Rtc::reset_divider() noexcept { divider_256hz_ = 0u; }
void Sh4Rtc::set_periodic_rate(const RtcPeriodicRate rate) noexcept { periodic_rate_ = rate; }
RtcPeriodicRate Sh4Rtc::periodic_rate() const noexcept { return periodic_rate_; }
void Sh4Rtc::set_carry_interrupt_enabled(const bool enabled) noexcept {
    carry_enabled_ = enabled;
    if (!enabled) { carry_pending_ = false; }
}
std::uint8_t Sh4Rtc::counter_64hz() const noexcept {
    return static_cast<std::uint8_t>(divider_256hz_ / 4u);
}
bool Sh4Rtc::periodic_interrupt_pending() const noexcept { return periodic_pending_; }
bool Sh4Rtc::carry_interrupt_pending() const noexcept { return carry_pending_; }
void Sh4Rtc::acknowledge_periodic_interrupt() noexcept { periodic_pending_ = false; }
void Sh4Rtc::acknowledge_carry_interrupt() noexcept { carry_pending_ = false; }
std::uint64_t Sh4Rtc::tick_count() const noexcept { return ticks_; }
std::uint64_t Sh4Rtc::periodic_event_count() const noexcept { return periodic_events_; }

std::uint16_t Sh4Rtc::periodic_ticks(const RtcPeriodicRate rate) noexcept {
    switch (rate) {
    case RtcPeriodicRate::Disabled: return 0u;
    case RtcPeriodicRate::Every1Over256Second: return 1u;
    case RtcPeriodicRate::Every1Over64Second: return 4u;
    case RtcPeriodicRate::Every1Over16Second: return 16u;
    case RtcPeriodicRate::Every1Over4Second: return 64u;
    case RtcPeriodicRate::Every1Over2Second: return 128u;
    case RtcPeriodicRate::EverySecond: return 256u;
    case RtcPeriodicRate::Every2Seconds: return 512u;
    }
    return 0u;
}

void Sh4Rtc::schedule_tick() {
    event_ = scheduler_.schedule_after(cycles_per_tick_, [this](const auto, const auto) {
        tick();
    });
}

void Sh4Rtc::tick() {
    event_.reset();
    ++ticks_;
    const auto period = periodic_ticks(periodic_rate_);
    if (period != 0u && ticks_ % period == 0u) {
        periodic_pending_ = true;
        ++periodic_events_;
    }
    divider_256hz_ = static_cast<std::uint16_t>((divider_256hz_ + 1u) & 0xFFu);
    if (divider_256hz_ == 0u) {
        increment_second();
        if (carry_enabled_) { carry_pending_ = true; }
    }
    if (running_) { schedule_tick(); }
}

void Sh4Rtc::increment_second() noexcept {
    if (++date_time_.second <= 59u) { return; }
    date_time_.second = 0u;
    if (++date_time_.minute <= 59u) { return; }
    date_time_.minute = 0u;
    if (++date_time_.hour <= 23u) { return; }
    date_time_.hour = 0u;
    date_time_.day_of_week = static_cast<std::uint8_t>((date_time_.day_of_week + 1u) % 7u);
    if (++date_time_.day <= days_in_month(date_time_.year, date_time_.month)) { return; }
    date_time_.day = 1u;
    if (++date_time_.month <= 12u) { return; }
    date_time_.month = 1u;
    date_time_.year = static_cast<std::uint16_t>((date_time_.year + 1u) % 10000u);
}

} // namespace katana::runtime
