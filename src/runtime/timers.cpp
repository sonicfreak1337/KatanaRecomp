#include "katana/runtime/timers.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace katana::runtime {

namespace {

std::uint64_t
checked_add(const std::uint64_t left, const std::uint64_t right, const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error(message);
    }
    return left + right;
}

std::uint64_t checked_multiply(const std::uint64_t left,
                               const std::uint64_t right,
                               const char* message = "TMU-Ereignisfrist ist uebergelaufen.") {
    if (right != 0u && left > std::numeric_limits<std::uint64_t>::max() / right) {
        throw std::overflow_error(message);
    }
    return left * right;
}

} // namespace

Sh4RtcClockDomain::Sh4RtcClockDomain(const std::uint64_t guest_cycles_per_second)
    : guest_cycles_per_second_(guest_cycles_per_second) {
    if (guest_cycles_per_second_ == 0u ||
        guest_cycles_per_second_ >
            std::numeric_limits<std::uint64_t>::max() / source_frequency_hz) {
        throw std::invalid_argument(
            "RTC-Gastzyklen pro Sekunde liegen ausserhalb des sicheren Bereichs.");
    }
}

std::uint64_t Sh4RtcClockDomain::guest_cycles_per_second() const noexcept {
    return guest_cycles_per_second_;
}

std::uint64_t Sh4RtcClockDomain::ticks_at(const std::uint64_t guest_cycle) const {
    if (guest_cycle < epoch_cycle_) {
        throw std::logic_error("RTC-Taktdomaene liegt vor ihrem Phasenursprung.");
    }
    const auto elapsed = guest_cycle - epoch_cycle_;
    const auto whole_seconds = elapsed / guest_cycles_per_second_;
    const auto remainder = elapsed % guest_cycles_per_second_;
    return checked_add(
        checked_multiply(whole_seconds, source_frequency_hz, "RTC-Tickindex ist uebergelaufen."),
        checked_multiply(remainder, source_frequency_hz, "RTC-Tickphase ist uebergelaufen.") /
            guest_cycles_per_second_,
        "RTC-Tickindex ist uebergelaufen.");
}

std::uint64_t Sh4RtcClockDomain::cycle_at_or_after_tick(const std::uint64_t tick) const {
    const auto whole_seconds = tick / source_frequency_hz;
    const auto remainder = tick % source_frequency_hz;
    const auto partial_product =
        checked_multiply(remainder, guest_cycles_per_second_, "RTC-Phasenfrist ist uebergelaufen.");
    const auto partial_cycles = partial_product / source_frequency_hz +
                                (partial_product % source_frequency_hz == 0u ? 0u : 1u);
    const auto relative = checked_add(checked_multiply(whole_seconds,
                                                       guest_cycles_per_second_,
                                                       "RTC-Phasenfrist ist uebergelaufen."),
                                      partial_cycles,
                                      "RTC-Phasenfrist ist uebergelaufen.");
    return checked_add(epoch_cycle_, relative, "RTC-Phasenfrist ist uebergelaufen.");
}

std::uint64_t Sh4RtcClockDomain::elapsed_ticks(const std::uint64_t first_cycle,
                                               const std::uint64_t last_cycle) const {
    if (last_cycle < first_cycle) {
        throw std::invalid_argument("RTC-Tickintervall darf nicht rueckwaerts laufen.");
    }
    return ticks_at(last_cycle) - ticks_at(first_cycle);
}

std::uint64_t Sh4RtcClockDomain::deadline_after(const std::uint64_t guest_cycle,
                                                const std::uint64_t source_ticks) const {
    if (source_ticks == 0u) {
        return guest_cycle;
    }
    const auto target_tick =
        checked_add(ticks_at(guest_cycle), source_ticks, "RTC-Zieltick ist uebergelaufen.");
    return cycle_at_or_after_tick(target_tick);
}

Sh4RtcClockDomain::PhaseObserverId Sh4RtcClockDomain::add_phase_observer(PhaseObserver observer) {
    if (!observer) {
        throw std::invalid_argument("RTC-Phasenbeobachter benoetigt einen Callback.");
    }
    if (next_observer_id_ == std::numeric_limits<PhaseObserverId>::max()) {
        throw std::overflow_error("RTC-Phasenbeobachter-ID ist uebergelaufen.");
    }
    const auto observer_id = next_observer_id_++;
    observers_.emplace(observer_id, std::move(observer));
    return observer_id;
}

bool Sh4RtcClockDomain::remove_phase_observer(const PhaseObserverId observer_id) noexcept {
    return observers_.erase(observer_id) != 0u;
}

void Sh4RtcClockDomain::reset_phase(const std::uint64_t guest_cycle) {
    std::vector<PhaseObserver> observers;
    observers.reserve(observers_.size());
    for (const auto& [id, observer] : observers_) {
        static_cast<void>(id);
        observers.push_back(observer);
    }
    for (const auto& observer : observers) {
        observer(true, guest_cycle);
    }
    epoch_cycle_ = guest_cycle;
    for (const auto& observer : observers) {
        observer(false, guest_cycle);
    }
}

Sh4Tmu::Sh4Tmu(EventScheduler& scheduler, TmuTiming timing)
    : scheduler_(scheduler), timing_(std::move(timing)) {
    if (timing_.guest_cycles_per_peripheral_cycle == 0u) {
        throw std::invalid_argument("TMU-Peripherietakt muss groesser null sein.");
    }
    if (!timing_.rtc_clock) {
        timing_.rtc_clock = std::make_shared<Sh4RtcClockDomain>();
    }
    rtc_phase_observer_ =
        timing_.rtc_clock->add_phase_observer([this](const bool before, const std::uint64_t cycle) {
            handle_rtc_phase_reset(before, cycle);
        });
    scheduler_reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

Sh4Tmu::~Sh4Tmu() {
    static_cast<void>(scheduler_.remove_reset_observer(scheduler_reset_observer_));
    static_cast<void>(timing_.rtc_clock->remove_phase_observer(rtc_phase_observer_));
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

bool Sh4Tmu::uses_rtc_clock(const Channel& value) const noexcept {
    return (value.control & 0x7u) == 6u;
}

std::uint64_t Sh4Tmu::tick_period(const Channel& value) const {
    static constexpr std::array<std::uint64_t, 5> divisors = {4u, 16u, 64u, 256u, 1024u};
    const auto source = static_cast<std::uint8_t>(value.control & 0x7u);
    if (source < divisors.size()) {
        return checked_multiply(timing_.guest_cycles_per_peripheral_cycle, divisors[source]);
    }
    throw std::invalid_argument(
        "Reservierter, externer oder rationaler TMU-Takt besitzt keine feste Zyklusbreite.");
}

void Sh4Tmu::synchronize(const std::size_t index) {
    auto& value = channel(index);
    if (!value.running) {
        return;
    }
    if (scheduler_.current_cycle() < value.anchor_cycle) {
        value.anchor_cycle = scheduler_.current_cycle();
        value.anchor_counter = value.counter;
        return;
    }
    const auto ticks =
        uses_rtc_clock(value)
            ? timing_.rtc_clock->elapsed_ticks(value.anchor_cycle, scheduler_.current_cycle())
            : (scheduler_.current_cycle() - value.anchor_cycle) / tick_period(value);
    if (ticks == 0u) {
        return;
    }
    value.counter = static_cast<std::uint32_t>(value.anchor_counter - ticks);
    value.anchor_counter = value.counter;
    value.anchor_cycle = uses_rtc_clock(value)
                             ? timing_.rtc_clock->deadline_after(value.anchor_cycle, ticks)
                             : value.anchor_cycle + ticks * tick_period(value);
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
    const auto deadline = uses_rtc_clock(value)
                              ? timing_.rtc_clock->deadline_after(value.anchor_cycle, ticks)
                              : checked_add(value.anchor_cycle,
                                            checked_multiply(ticks, tick_period(value)),
                                            "TMU-Ereignisfrist ist uebergelaufen.");
    value.event = scheduler_.schedule_at(
        deadline, [this, index](const auto, const auto) { handle_underflow(index); });
}

void Sh4Tmu::handle_underflow(const std::size_t index) {
    auto& value = channel(index);
    value.event.reset();
    value.counter = value.constant;
    value.anchor_counter = value.counter;
    value.anchor_cycle = scheduler_.current_cycle();
    value.control |= underflow_flag;
    ++value.underflows;
    if (value.running) {
        schedule_underflow(index);
    }
}

void Sh4Tmu::handle_rtc_phase_reset(const bool before_reset, const std::uint64_t guest_cycle) {
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        auto& value = channels_[index];
        if (!value.running || !uses_rtc_clock(value)) {
            continue;
        }
        if (before_reset) {
            if (scheduler_.current_cycle() >= value.anchor_cycle) {
                synchronize(index);
            }
            cancel_event(value);
        } else {
            value.anchor_cycle = guest_cycle;
            value.anchor_counter = value.counter;
            schedule_underflow(index);
        }
    }
}

void Sh4Tmu::handle_scheduler_reset() {
    for (auto& value : channels_) {
        cancel_event(value);
    }
    timing_.rtc_clock->reset_phase(scheduler_.current_cycle());
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        auto& value = channels_[index];
        if (value.running && !uses_rtc_clock(value)) {
            value.anchor_cycle = scheduler_.current_cycle();
            value.anchor_counter = value.counter;
            schedule_underflow(index);
        }
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
    const auto retained_flag =
        static_cast<std::uint16_t>(value.control & new_value & underflow_flag);
    value.control = static_cast<std::uint16_t>((new_value & 0x003Fu) | retained_flag);
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

std::uint32_t Sh4Tmu::constant(const std::size_t index) const {
    return channel(index).constant;
}
std::uint32_t Sh4Tmu::counter(const std::size_t index) {
    synchronize(index);
    return channel(index).counter;
}
std::uint16_t Sh4Tmu::control(const std::size_t index) const {
    return channel(index).control;
}
std::uint8_t Sh4Tmu::start() const noexcept {
    std::uint8_t result = 0u;
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        if (channels_[index].running) {
            result |= static_cast<std::uint8_t>(1u << index);
        }
    }
    return result;
}
bool Sh4Tmu::interrupt_pending(const std::size_t index) const {
    const auto& value = channel(index);
    return (value.control & (underflow_flag | underflow_interrupt_enable)) ==
           (underflow_flag | underflow_interrupt_enable);
}
void Sh4Tmu::acknowledge_interrupt(const std::size_t index) noexcept {
    static_cast<void>(index);
}
std::uint64_t Sh4Tmu::underflow_count(const std::size_t index) const {
    return channel(index).underflows;
}

void Sh4Tmu::reset() noexcept {
    for (auto& value : channels_) {
        cancel_event(value);
        value = Channel{};
    }
}

Sh4Rtc::Sh4Rtc(EventScheduler& scheduler, const std::uint64_t guest_cycles_per_second)
    : Sh4Rtc(scheduler, std::make_shared<Sh4RtcClockDomain>(guest_cycles_per_second)) {}

Sh4Rtc::Sh4Rtc(EventScheduler& scheduler, std::shared_ptr<Sh4RtcClockDomain> clock)
    : scheduler_(scheduler), clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("RTC benoetigt eine Taktdomaene.");
    }
    scheduler_reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

Sh4Rtc::~Sh4Rtc() {
    static_cast<void>(scheduler_.remove_reset_observer(scheduler_reset_observer_));
    rtc_enabled_ = false;
    cancel_event();
}

bool Sh4Rtc::leap_year(const std::uint16_t year) noexcept {
    return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

std::uint8_t Sh4Rtc::days_in_month(const std::uint16_t year, const std::uint8_t month) noexcept {
    static constexpr std::array<std::uint8_t, 12> days = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    return static_cast<std::uint8_t>(days[month - 1u] + (month == 2u && leap_year(year) ? 1u : 0u));
}

void Sh4Rtc::validate(const RtcDateTime& value) {
    if (value.year > 9999u || value.month < 1u || value.month > 12u || value.day < 1u ||
        value.day > days_in_month(value.year, value.month) || value.day_of_week > 6u ||
        value.hour > 23u || value.minute > 59u || value.second > 59u) {
        throw std::invalid_argument("Ungueltiger RTC-Kalenderzustand.");
    }
}

void Sh4Rtc::set_date_time(const RtcDateTime& value) {
    if (calendar_running_) {
        throw std::logic_error("RTC-Kalender darf nur im gestoppten Zustand gesetzt werden.");
    }
    validate(value);
    date_time_ = value;
}

const RtcDateTime& Sh4Rtc::date_time() const noexcept {
    return date_time_;
}

void Sh4Rtc::start() {
    calendar_running_ = true;
    set_rtc_enabled(true);
}

void Sh4Rtc::stop() noexcept {
    calendar_running_ = false;
}

bool Sh4Rtc::running() const noexcept {
    return calendar_running_;
}

void Sh4Rtc::set_rtc_enabled(const bool enabled) {
    rtc_enabled_ = enabled;
    if (!enabled) {
        cancel_event();
    } else if (!event_) {
        schedule_tick();
    }
}

bool Sh4Rtc::rtc_enabled() const noexcept {
    return rtc_enabled_;
}

void Sh4Rtc::reset_divider() {
    cancel_event();
    divider_256hz_phase_ = 0u;
    counter_64hz_ = 0u;
    periodic_phase_ticks_ = 0u;
    periodic_pending_ = false;
    clock_->reset_phase(scheduler_.current_cycle());
    if (rtc_enabled_) {
        schedule_tick();
    }
}

void Sh4Rtc::set_periodic_rate(const RtcPeriodicRate rate) noexcept {
    periodic_rate_ = rate;
}
RtcPeriodicRate Sh4Rtc::periodic_rate() const noexcept {
    return periodic_rate_;
}
void Sh4Rtc::set_carry_interrupt_enabled(const bool enabled) noexcept {
    carry_enabled_ = enabled;
}
void Sh4Rtc::set_alarm_interrupt_enabled(const bool enabled) noexcept {
    alarm_enabled_ = enabled;
}
void Sh4Rtc::write_alarm_register(const std::size_t index, const std::uint8_t value) {
    if (index >= alarm_registers_.size()) throw std::out_of_range("RTC-Alarmregister ist ungueltig.");
    alarm_registers_[index] = value;
}
std::uint8_t Sh4Rtc::alarm_register(const std::size_t index) const {
    if (index >= alarm_registers_.size()) throw std::out_of_range("RTC-Alarmregister ist ungueltig.");
    return alarm_registers_[index];
}
bool Sh4Rtc::carry_flag() const noexcept {
    return carry_flag_;
}
std::uint8_t Sh4Rtc::counter_64hz() const noexcept {
    return counter_64hz_;
}
bool Sh4Rtc::periodic_interrupt_pending() const noexcept {
    return periodic_pending_;
}
bool Sh4Rtc::carry_interrupt_pending() const noexcept {
    return carry_flag_ && carry_enabled_;
}
bool Sh4Rtc::alarm_interrupt_pending() const noexcept {
    return alarm_pending_ && alarm_enabled_;
}
bool Sh4Rtc::alarm_flag() const noexcept {
    return alarm_pending_;
}
void Sh4Rtc::acknowledge_periodic_interrupt() noexcept {
    periodic_pending_ = false;
}
void Sh4Rtc::acknowledge_carry_interrupt() noexcept {
    carry_flag_ = false;
}
void Sh4Rtc::acknowledge_alarm_interrupt() noexcept {
    alarm_pending_ = false;
}
std::uint64_t Sh4Rtc::tick_count() const noexcept {
    return ticks_;
}
std::uint64_t Sh4Rtc::periodic_event_count() const noexcept {
    return periodic_events_;
}

std::uint16_t Sh4Rtc::periodic_ticks(const RtcPeriodicRate rate) noexcept {
    switch (rate) {
    case RtcPeriodicRate::Disabled:
        return 0u;
    case RtcPeriodicRate::Every1Over256Second:
        return 1u;
    case RtcPeriodicRate::Every1Over64Second:
        return 4u;
    case RtcPeriodicRate::Every1Over16Second:
        return 16u;
    case RtcPeriodicRate::Every1Over4Second:
        return 64u;
    case RtcPeriodicRate::Every1Over2Second:
        return 128u;
    case RtcPeriodicRate::EverySecond:
        return 256u;
    case RtcPeriodicRate::Every2Seconds:
        return 512u;
    }
    return 0u;
}

void Sh4Rtc::schedule_tick() {
    const auto deadline = clock_->deadline_after(scheduler_.current_cycle(), 64u);
    event_ = scheduler_.schedule_at(deadline, [this](const auto, const auto) { tick(); });
}

void Sh4Rtc::cancel_event() noexcept {
    if (event_) {
        static_cast<void>(scheduler_.cancel(*event_));
        event_.reset();
    }
}

void Sh4Rtc::handle_scheduler_reset() {
    event_.reset();
    clock_->reset_phase(scheduler_.current_cycle());
    if (rtc_enabled_) {
        schedule_tick();
    }
}

void Sh4Rtc::tick() {
    event_.reset();
    ++ticks_;
    ++periodic_phase_ticks_;
    const auto period = periodic_ticks(periodic_rate_);
    if (period != 0u && periodic_phase_ticks_ % period == 0u) {
        periodic_pending_ = true;
        ++periodic_events_;
    }

    divider_256hz_phase_ ^= 1u;
    if (divider_256hz_phase_ == 0u) {
        counter_64hz_ = static_cast<std::uint8_t>((counter_64hz_ + 1u) & 0x7Fu);
        if (counter_64hz_ == 0u) {
            carry_flag_ = true;
            if (calendar_running_) {
                increment_second();
                update_alarm();
            }
        }
    }
    if (rtc_enabled_) {
        schedule_tick();
    }
}

void Sh4Rtc::update_alarm() noexcept {
    const auto bcd = [](const std::uint8_t value) {
        return static_cast<std::uint8_t>((value / 10u) << 4u | (value % 10u));
    };
    const std::array<std::uint8_t, 6u> current{bcd(date_time_.second),
                                               bcd(date_time_.minute),
                                               bcd(date_time_.hour),
                                               date_time_.day_of_week,
                                               bcd(date_time_.day),
                                               bcd(date_time_.month)};
    bool any_enabled = false;
    for (std::size_t index = 0u; index < alarm_registers_.size(); ++index) {
        if ((alarm_registers_[index] & 0x80u) == 0u) continue;
        any_enabled = true;
        if ((alarm_registers_[index] & 0x7Fu) != current[index]) return;
    }
    if (any_enabled) alarm_pending_ = true;
}

void Sh4Rtc::increment_second() noexcept {
    if (++date_time_.second <= 59u) {
        return;
    }
    date_time_.second = 0u;
    if (++date_time_.minute <= 59u) {
        return;
    }
    date_time_.minute = 0u;
    if (++date_time_.hour <= 23u) {
        return;
    }
    date_time_.hour = 0u;
    date_time_.day_of_week = static_cast<std::uint8_t>((date_time_.day_of_week + 1u) % 7u);
    if (++date_time_.day <= days_in_month(date_time_.year, date_time_.month)) {
        return;
    }
    date_time_.day = 1u;
    if (++date_time_.month <= 12u) {
        return;
    }
    date_time_.month = 1u;
    date_time_.year = static_cast<std::uint16_t>((date_time_.year + 1u) % 10000u);
}

namespace {

std::uint8_t to_bcd(const std::uint32_t value) {
    return static_cast<std::uint8_t>(((value / 10u) << 4u) | (value % 10u));
}

std::uint32_t from_bcd(const std::uint32_t value) {
    const auto low = value & 0xFu;
    const auto high = (value >> 4u) & 0xFu;
    if (low > 9u || high > 9u) throw std::invalid_argument("RTC-BCD-Wert ist ungueltig.");
    return high * 10u + low;
}

void map_timer_aliases(Memory& memory,
                       const std::string& name,
                       const std::uint32_t p4,
                       const std::uint32_t area7,
                       const std::shared_ptr<MemoryDevice>& device) {
    memory.map_region(name + "-p4", p4, device);
    memory.map_region(name + "-area7", area7, device);
}

} // namespace

void map_sh4_tmu_registers(Memory& memory, const std::shared_ptr<Sh4Tmu>& tmu) {
    if (!tmu) throw std::invalid_argument("TMU-MMIO braucht eine zustandsfuehrende Instanz.");
    const auto tocr = std::make_shared<std::uint8_t>(std::uint8_t{0u});
    const auto device = std::make_shared<MmioMemoryDevice>(
        sh4_tmu_register_size,
        [tmu, tocr](const auto offset, const auto width) -> std::uint32_t {
            if (offset == 0x00u && width == MemoryAccessWidth::Byte) return *tocr;
            if (offset == 0x04u && width == MemoryAccessWidth::Byte) return tmu->start();
            if (offset >= 0x08u && offset <= 0x28u) {
                const auto channel = static_cast<std::size_t>((offset - 0x08u) / 0x0Cu);
                const auto reg = (offset - 0x08u) % 0x0Cu;
                if (channel >= Sh4Tmu::channel_count)
                    throw std::out_of_range("TMU-Kanal ist ungueltig.");
                if (reg == 0u && width == MemoryAccessWidth::Word)
                    return tmu->constant(channel);
                if (reg == 4u && width == MemoryAccessWidth::Word)
                    return tmu->counter(channel);
                if (reg == 8u && width == MemoryAccessWidth::Halfword)
                    return tmu->control(channel);
            }
            if (offset == 0x2Cu)
                throw std::runtime_error("TMU-Eingangscapture besitzt keine externe Quelle.");
            throw std::invalid_argument("Ungueltige TMU-Registerbreite oder Offset.");
        },
        [tmu, tocr](const auto offset, const auto value, const auto width) {
            if (offset == 0x00u && width == MemoryAccessWidth::Byte) {
                *tocr = static_cast<std::uint8_t>(value & 1u);
                return;
            }
            if (offset == 0x04u && width == MemoryAccessWidth::Byte) {
                tmu->write_start(static_cast<std::uint8_t>(value & 7u));
                return;
            }
            if (offset >= 0x08u && offset <= 0x28u) {
                const auto channel = static_cast<std::size_t>((offset - 0x08u) / 0x0Cu);
                const auto reg = (offset - 0x08u) % 0x0Cu;
                if (channel >= Sh4Tmu::channel_count)
                    throw std::out_of_range("TMU-Kanal ist ungueltig.");
                if (reg == 0u && width == MemoryAccessWidth::Word) {
                    tmu->write_constant(channel, value);
                    return;
                }
                if (reg == 4u && width == MemoryAccessWidth::Word) {
                    tmu->write_counter(channel, value);
                    return;
                }
                if (reg == 8u && width == MemoryAccessWidth::Halfword) {
                    tmu->write_control(channel, static_cast<std::uint16_t>(value));
                    return;
                }
            }
            throw std::invalid_argument("Ungueltige TMU-Registerbreite oder Offset.");
        });
    map_timer_aliases(memory, "sh4-tmu", sh4_tmu_p4_base, sh4_tmu_area7_base, device);
}

void map_sh4_rtc_registers(Memory& memory, const std::shared_ptr<Sh4Rtc>& rtc) {
    if (!rtc) throw std::invalid_argument("RTC-MMIO braucht eine zustandsfuehrende Instanz.");
    const auto rcr1 = std::make_shared<std::uint8_t>(std::uint8_t{0u});
    const auto rcr2 = std::make_shared<std::uint8_t>(std::uint8_t{0u});
    const auto device = std::make_shared<MmioMemoryDevice>(
        sh4_rtc_register_size,
        [rtc, rcr1, rcr2](const auto offset, const auto width) -> std::uint32_t {
            const auto& value = rtc->date_time();
            if (offset == 0x1Cu && width == MemoryAccessWidth::Halfword)
                return static_cast<std::uint32_t>(to_bcd(value.year / 100u)) << 8u |
                       to_bcd(value.year % 100u);
            if (width != MemoryAccessWidth::Byte)
                throw std::invalid_argument("RTC-Zaehler verlangen 8-Bit-Zugriffe.");
            switch (offset) {
            case 0x00u:
                return rtc->counter_64hz();
            case 0x04u:
                return to_bcd(value.second);
            case 0x08u:
                return to_bcd(value.minute);
            case 0x0Cu:
                return to_bcd(value.hour);
            case 0x10u:
                return value.day_of_week;
            case 0x14u:
                return to_bcd(value.day);
            case 0x18u:
                return to_bcd(value.month);
            case 0x20u:
            case 0x24u:
            case 0x28u:
            case 0x2Cu:
            case 0x30u:
            case 0x34u:
                return rtc->alarm_register((offset - 0x20u) / 4u);
            case 0x38u:
                return (*rcr1 & 0x18u) | (rtc->carry_flag() ? 0x80u : 0u) |
                       (rtc->alarm_flag() ? 1u : 0u);
            case 0x3Cu:
                return (*rcr2 & 0x76u) | (rtc->running() ? 1u : 0u) |
                       (rtc->rtc_enabled() ? 8u : 0u);
            default:
                throw std::runtime_error("Ungueltiger RTC-Registeroffset.");
            }
        },
        [rtc, rcr1, rcr2](const auto offset, const auto raw, const auto width) {
            if (offset == 0x1Cu && width == MemoryAccessWidth::Halfword) {
                auto value = rtc->date_time();
                value.year = static_cast<std::uint16_t>(from_bcd((raw >> 8u) & 0xFFu) * 100u +
                                                        from_bcd(raw & 0xFFu));
                rtc->set_date_time(value);
                return;
            }
            if (width != MemoryAccessWidth::Byte)
                throw std::invalid_argument("RTC-Zaehler verlangen 8-Bit-Zugriffe.");
            if (offset >= 0x04u && offset <= 0x18u) {
                auto value = rtc->date_time();
                switch (offset) {
                case 0x04u:
                    value.second = static_cast<std::uint8_t>(from_bcd(raw));
                    break;
                case 0x08u:
                    value.minute = static_cast<std::uint8_t>(from_bcd(raw));
                    break;
                case 0x0Cu:
                    value.hour = static_cast<std::uint8_t>(from_bcd(raw));
                    break;
                case 0x10u:
                    value.day_of_week = static_cast<std::uint8_t>(raw & 7u);
                    break;
                case 0x14u:
                    value.day = static_cast<std::uint8_t>(from_bcd(raw));
                    break;
                case 0x18u:
                    value.month = static_cast<std::uint8_t>(from_bcd(raw));
                    break;
                }
                rtc->set_date_time(value);
                return;
            }
            if (offset >= 0x20u && offset <= 0x34u && (offset & 3u) == 0u) {
                rtc->write_alarm_register((offset - 0x20u) / 4u,
                                          static_cast<std::uint8_t>(raw));
                return;
            }
            if (offset == 0x38u) {
                *rcr1 = static_cast<std::uint8_t>(raw & 0x18u);
                rtc->set_carry_interrupt_enabled((raw & 0x10u) != 0u);
                rtc->set_alarm_interrupt_enabled((raw & 0x08u) != 0u);
                if ((raw & 0x80u) == 0u) rtc->acknowledge_carry_interrupt();
                if ((raw & 0x01u) == 0u) rtc->acknowledge_alarm_interrupt();
                return;
            }
            if (offset == 0x3Cu) {
                *rcr2 = static_cast<std::uint8_t>(raw & 0x76u);
                rtc->set_periodic_rate(static_cast<RtcPeriodicRate>((raw >> 4u) & 7u));
                if ((raw & 0x80u) == 0u) rtc->acknowledge_periodic_interrupt();
                rtc->set_rtc_enabled((raw & 8u) != 0u);
                if ((raw & 2u) != 0u) rtc->reset_divider();
                if ((raw & 1u) != 0u)
                    rtc->start();
                else
                    rtc->stop();
                return;
            }
            throw std::runtime_error("Ungueltiger RTC-Registeroffset.");
        });
    map_timer_aliases(memory, "sh4-rtc", sh4_rtc_p4_base, sh4_rtc_area7_base, device);
}

} // namespace katana::runtime
