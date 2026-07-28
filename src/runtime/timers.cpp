#include "katana/runtime/timers.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace katana::runtime {

namespace {

constexpr std::array<std::uint8_t, 8u> sh4_tmu_state_magic{
    'K', 'A', 'T', 'T', 'M', 'U', '1', '\n'};
constexpr std::array<std::uint8_t, 8u> sh4_rtc_clock_state_magic{
    'K', 'A', 'T', 'C', 'L', 'K', '1', '\n'};
constexpr std::array<std::uint8_t, 8u> sh4_rtc_state_magic{
    'K', 'A', 'T', 'R', 'T', 'C', '1', '\n'};

class TimerStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u16(const std::uint16_t value) {
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void raw(const std::span<const std::uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class TimerStateReader final {
  public:
    explicit TimerStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "Timer-Handoff-Payload besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }
    [[nodiscard]] std::uint16_t u16() {
        require(2u);
        std::uint16_t value = 0u;
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            value |= static_cast<std::uint16_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 2u;
        return value;
    }
    [[nodiscard]] std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 4u;
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        require(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 8u;
        return value;
    }
    [[nodiscard]] std::vector<std::uint8_t> raw(const std::size_t size) {
        require(size);
        std::vector<std::uint8_t> result(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return result;
    }
    void expect_end() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "Timer-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "Timer-Handoff-Payload ist abgeschnitten.");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

void expect_magic(TimerStateReader& reader,
                  const std::span<const std::uint8_t> expected) {
    const auto actual = reader.raw(expected.size());
    if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()))
        throw std::invalid_argument(
            "Timer-Handoff-Payload besitzt eine ungueltige Kennung.");
}

bool portable_rtc_leap_year(const std::uint16_t year) noexcept {
    return year % 4u == 0u &&
           (year % 100u != 0u || year % 400u == 0u);
}

std::uint8_t portable_rtc_days_in_month(const std::uint16_t year,
                                        const std::uint8_t month) noexcept {
    static constexpr std::array<std::uint8_t, 12u> days{
        31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u};
    return static_cast<std::uint8_t>(
        days[month - 1u] +
        (month == 2u && portable_rtc_leap_year(year) ? 1u : 0u));
}

void validate_portable_rtc_date_time(const RtcDateTime& value) {
    if (value.year > 9999u || value.month < 1u || value.month > 12u ||
        value.day < 1u ||
        value.day > portable_rtc_days_in_month(value.year, value.month) ||
        value.day_of_week > 6u || value.hour > 23u ||
        value.minute > 59u || value.second > 59u)
        throw std::invalid_argument("Ungueltiger RTC-Kalenderzustand.");
}

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

Sh4RtcClockDomain::Snapshot Sh4RtcClockDomain::snapshot() const {
    Snapshot result;
    result.guest_cycles_per_second = guest_cycles_per_second_;
    result.epoch_cycle = epoch_cycle_;
    result.next_observer_id = next_observer_id_;
    result.observer_ids.reserve(observers_.size());
    for (const auto& [id, observer] : observers_) {
        static_cast<void>(observer);
        result.observer_ids.push_back(id);
    }
    return result;
}

void Sh4RtcClockDomain::validate_state_restore(const Snapshot& state) const {
    if (state.guest_cycles_per_second != guest_cycles_per_second_) {
        throw std::invalid_argument(
            "RTC-Taktdomaenen-Handoff passt nicht zum Runtime-Taktvertrag.");
    }
}

void Sh4RtcClockDomain::restore_state_passive(const Snapshot& state) {
    validate_state_restore(state);
    // next_observer_id and observer_ids identify callbacks in the source
    // process. The destination bindings deliberately remain untouched.
    epoch_cycle_ = state.epoch_cycle;
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
    : scheduler_(scheduler), scheduler_lifetime_(scheduler.lifetime_token()),
      timing_(std::move(timing)) {
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
    static_cast<void>(timing_.rtc_clock->remove_phase_observer(rtc_phase_observer_));
    if (scheduler_lifetime_.expired()) return;
    static_cast<void>(scheduler_.remove_reset_observer(scheduler_reset_observer_));
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

std::uint32_t Sh4Tmu::effective_counter(const Channel& value) const {
    if (!value.running || scheduler_.current_cycle() < value.anchor_cycle)
        return value.counter;
    const auto ticks =
        uses_rtc_clock(value)
            ? timing_.rtc_clock->elapsed_ticks(value.anchor_cycle, scheduler_.current_cycle())
            : (scheduler_.current_cycle() - value.anchor_cycle) / tick_period(value);
    return static_cast<std::uint32_t>(value.anchor_counter - ticks);
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
    value.event_deadline.reset();
    value.event_rehydration_pending = false;
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
    const auto event_id = scheduler_.schedule_at(
        deadline,
        [this, index](const auto, const auto) { handle_underflow(index); },
        static_cast<SchedulerEventKind>(
            static_cast<std::uint32_t>(SchedulerEventKind::Sh4Tmu0) + index));
    value.event = event_id;
    value.event_deadline = deadline;
    value.event_rehydration_pending = false;
}

void Sh4Tmu::handle_underflow(const std::size_t index) {
    auto& value = channel(index);
    value.event.reset();
    value.event_deadline.reset();
    value.event_rehydration_pending = false;
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

Sh4TmuSnapshot Sh4Tmu::snapshot() const {
    Sh4TmuSnapshot result;
    result.scheduler_cycle = scheduler_.current_cycle();
    result.guest_cycles_per_peripheral_cycle =
        timing_.guest_cycles_per_peripheral_cycle;
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        const auto& source = channels_[index];
        auto& destination = result.channels[index];
        destination.constant = source.constant;
        destination.stored_counter = source.counter;
        destination.effective_counter = effective_counter(source);
        destination.control = source.control;
        destination.anchor_cycle = source.anchor_cycle;
        destination.anchor_counter = source.anchor_counter;
        destination.underflows = source.underflows;
        destination.event = source.event;
        destination.event_deadline = source.event_deadline;
        destination.event_rehydration_pending = source.event_rehydration_pending;
        destination.running = source.running;
    }
    return result;
}

void Sh4Tmu::validate_state_restore(const Sh4TmuSnapshot& state) const {
    validate_state_restore(state, scheduler_.current_cycle());
}

void Sh4Tmu::validate_state_restore(
    const Sh4TmuSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    if (state.scheduler_cycle != expected_scheduler_cycle) {
        throw std::invalid_argument(
            "TMU-Handoff passt nicht zum wiederhergestellten Schedulerzyklus.");
    }
    if (state.guest_cycles_per_peripheral_cycle !=
        timing_.guest_cycles_per_peripheral_cycle) {
        throw std::invalid_argument(
            "TMU-Handoff passt nicht zum Runtime-Taktvertrag.");
    }

    for (const auto& source : state.channels) {
        const auto clock_source = static_cast<std::uint8_t>(source.control & 0x7u);
        if ((source.control & ~std::uint16_t{0x013Fu}) != 0u ||
            clock_source == 5u || clock_source == 7u ||
            source.anchor_cycle > state.scheduler_cycle ||
            (source.event && source.event_rehydration_pending)) {
            throw std::invalid_argument(
                "TMU-Handoff besitzt ungueltige Register-, Anker- oder Eventdaten.");
        }
        if (!source.running &&
            (source.event || source.event_deadline ||
             source.event_rehydration_pending)) {
            throw std::invalid_argument(
                "TMU-Handoff besitzt einen inkonsistenten Laufzeit-Eventvertrag.");
        }
        if (!source.running) {
            if (source.effective_counter != source.stored_counter) {
                throw std::invalid_argument(
                    "Gestoppter TMU-Handoff besitzt einen inkonsistenten Zaehler.");
            }
            continue;
        }
        if (source.event_deadline &&
            *source.event_deadline <= state.scheduler_cycle) {
            throw std::invalid_argument(
                "TMU-Handoff-Ereignis liegt nicht hinter dem Schedulerzyklus.");
        }
        if ((source.event || source.event_rehydration_pending) &&
            !source.event_deadline) {
            throw std::invalid_argument(
                "TMU-Handoff-Ereignis besitzt keine Ereignisfrist.");
        }

        if (clock_source < 5u) {
            Channel restored;
            restored.counter = source.stored_counter;
            restored.control = source.control;
            restored.anchor_cycle = source.anchor_cycle;
            restored.anchor_counter = source.anchor_counter;
            restored.running = true;
            const auto expected_deadline =
                checked_add(source.anchor_cycle,
                            checked_multiply(
                                static_cast<std::uint64_t>(source.anchor_counter) + 1u,
                                tick_period(restored)),
                            "TMU-Handoff-Ereignisfrist ist uebergelaufen.");
            const auto elapsed =
                (state.scheduler_cycle - source.anchor_cycle) / tick_period(restored);
            const auto expected_counter =
                static_cast<std::uint32_t>(source.anchor_counter - elapsed);
            if ((source.event_deadline &&
                 *source.event_deadline != expected_deadline) ||
                source.effective_counter != expected_counter) {
                throw std::invalid_argument(
                    "TMU-Handoff besitzt eine inkonsistente Zaehlerphase.");
            }
        }
    }
}

void Sh4Tmu::restore_state_passive(const Sh4TmuSnapshot& state) {
    validate_state_restore(state);
    for (auto& destination : channels_) {
        cancel_event(destination);
    }
    for (std::size_t index = 0u; index < channels_.size(); ++index) {
        const auto& source = state.channels[index];
        auto& destination = channels_[index];
        destination.constant = source.constant;
        destination.counter = source.stored_counter;
        destination.control = source.control;
        destination.anchor_cycle = source.anchor_cycle;
        destination.anchor_counter = source.anchor_counter;
        destination.underflows = source.underflows;
        destination.event.reset();
        destination.event_deadline = source.event_deadline;
        destination.event_rehydration_pending = source.running;
        destination.running = source.running;
    }
}

SchedulerEventId Sh4Tmu::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel_index,
    const std::uint64_t token) {
    if (channel_index >= channels_.size() || token != sh4_tmu_event_token_v1) {
        throw std::invalid_argument(
            "TMU-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    }
    auto& value = channels_[channel_index];
    if (!value.running || value.event || !value.event_rehydration_pending) {
        throw std::logic_error("TMU-Handoff erwartet fuer diesen Kanal kein Event.");
    }
    if (guest_cycle < scheduler_.current_cycle() ||
        (value.event_deadline && guest_cycle != *value.event_deadline)) {
        throw std::invalid_argument(
            "TMU-Handoff-Event passt nicht zur erfassten Ereignisfrist.");
    }

    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this, channel_index](const auto, const auto) {
            handle_underflow(channel_index);
        },
        static_cast<SchedulerEventKind>(
            static_cast<std::uint32_t>(SchedulerEventKind::Sh4Tmu0) +
            channel_index));
    value.event = event_id;
    value.event_deadline = guest_cycle;
    value.event_rehydration_pending = false;
    return event_id;
}

bool Sh4Tmu::event_rehydration_pending(const std::size_t channel_index) const {
    return channel(channel_index).event_rehydration_pending;
}

std::vector<std::uint8_t> encode_sh4_tmu_state(
    const Sh4TmuSnapshot& state) {
    if (state.guest_cycles_per_peripheral_cycle == 0u)
        throw std::invalid_argument(
            "TMU-Handoff-Payload besitzt keinen gueltigen Peripherietakt.");
    for (const auto& channel : state.channels) {
        const auto source = static_cast<std::uint8_t>(channel.control & 0x7u);
        if ((channel.control & ~std::uint16_t{0x013Fu}) != 0u ||
            source == 5u || source == 7u ||
            channel.anchor_cycle > state.scheduler_cycle ||
            (channel.event && channel.event_rehydration_pending) ||
            (channel.running &&
             (!(channel.event || channel.event_rehydration_pending) ||
              !channel.event_deadline)) ||
            (!channel.running &&
             (channel.event || channel.event_deadline ||
              channel.event_rehydration_pending)))
            throw std::invalid_argument(
                "TMU-Handoff-Payload besitzt inkonsistente Kanal- oder Eventdaten.");
    }

    TimerStateWriter writer;
    writer.raw(sh4_tmu_state_magic);
    writer.u32(sh4_tmu_state_contract_version);
    writer.u64(state.scheduler_cycle);
    writer.u64(state.guest_cycles_per_peripheral_cycle);
    for (const auto& channel : state.channels) {
        writer.u32(channel.constant);
        writer.u32(channel.stored_counter);
        writer.u32(channel.effective_counter);
        writer.u16(channel.control);
        writer.u64(channel.anchor_cycle);
        writer.u32(channel.anchor_counter);
        writer.u64(channel.underflows);
        writer.boolean(channel.running);
    }
    return std::move(writer).finish();
}

Sh4TmuSnapshot decode_sh4_tmu_state(
    const std::span<const std::uint8_t> bytes) {
    TimerStateReader reader(bytes);
    expect_magic(reader, sh4_tmu_state_magic);
    if (reader.u32() != sh4_tmu_state_contract_version)
        throw std::invalid_argument(
            "TMU-Handoff-Payload besitzt eine unbekannte Vertragsversion.");

    Sh4TmuSnapshot state;
    state.scheduler_cycle = reader.u64();
    state.guest_cycles_per_peripheral_cycle = reader.u64();
    if (state.guest_cycles_per_peripheral_cycle == 0u)
        throw std::invalid_argument(
            "TMU-Handoff-Payload besitzt keinen gueltigen Peripherietakt.");
    for (auto& channel : state.channels) {
        channel.constant = reader.u32();
        channel.stored_counter = reader.u32();
        channel.effective_counter = reader.u32();
        channel.control = reader.u16();
        channel.anchor_cycle = reader.u64();
        channel.anchor_counter = reader.u32();
        channel.underflows = reader.u64();
        channel.running = reader.boolean();
        channel.event.reset();
        channel.event_deadline.reset();
        channel.event_rehydration_pending = false;

        const auto source = static_cast<std::uint8_t>(channel.control & 0x7u);
        if ((channel.control & ~std::uint16_t{0x013Fu}) != 0u ||
            source == 5u || source == 7u ||
            channel.anchor_cycle > state.scheduler_cycle ||
            (!channel.running &&
             channel.effective_counter != channel.stored_counter))
            throw std::invalid_argument(
                "TMU-Handoff-Payload besitzt ungueltige Kanalwerte.");
    }
    reader.expect_end();
    return state;
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
    : scheduler_(scheduler), scheduler_lifetime_(scheduler.lifetime_token()),
      clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("RTC benoetigt eine Taktdomaene.");
    }
    scheduler_reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

Sh4Rtc::~Sh4Rtc() {
    if (scheduler_lifetime_.expired()) return;
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
    } else if (!event_ && !event_rehydration_pending_) {
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

Sh4RtcSnapshot Sh4Rtc::snapshot() const {
    Sh4RtcSnapshot result;
    result.date_time = date_time_;
    result.clock = clock_->snapshot();
    result.scheduler_cycle = scheduler_.current_cycle();
    result.event = event_;
    result.event_deadline = event_deadline_;
    result.event_rehydration_pending = event_rehydration_pending_;
    result.periodic_rate = periodic_rate_;
    result.divider_256hz_phase = divider_256hz_phase_;
    result.counter_64hz = counter_64hz_;
    result.periodic_phase_ticks = periodic_phase_ticks_;
    result.ticks = ticks_;
    result.periodic_events = periodic_events_;
    result.calendar_running = calendar_running_;
    result.rtc_enabled = rtc_enabled_;
    result.periodic_pending = periodic_pending_;
    result.carry_flag = carry_flag_;
    result.carry_enabled = carry_enabled_;
    result.alarm_registers = alarm_registers_;
    result.alarm_pending = alarm_pending_;
    result.alarm_enabled = alarm_enabled_;
    return result;
}

void Sh4Rtc::validate_state_restore(const Sh4RtcSnapshot& state) const {
    validate_state_restore(state, scheduler_.current_cycle());
}

void Sh4Rtc::validate_state_restore(
    const Sh4RtcSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    validate(state.date_time);
    clock_->validate_state_restore(state.clock);
    if (state.scheduler_cycle != expected_scheduler_cycle ||
        state.clock.epoch_cycle > state.scheduler_cycle) {
        throw std::invalid_argument(
            "RTC-Handoff passt nicht zum wiederhergestellten Schedulerzyklus.");
    }
    if (static_cast<std::uint8_t>(state.periodic_rate) >
            static_cast<std::uint8_t>(RtcPeriodicRate::Every2Seconds) ||
        state.divider_256hz_phase > 1u || state.counter_64hz > 0x7Fu ||
        (state.event && state.event_rehydration_pending)) {
        throw std::invalid_argument(
            "RTC-Handoff besitzt ungueltige Teiler-, Enum- oder Eventdaten.");
    }
    if (!state.rtc_enabled &&
        (state.event || state.event_deadline ||
         state.event_rehydration_pending)) {
        throw std::invalid_argument(
            "RTC-Handoff besitzt einen inkonsistenten Tickeventvertrag.");
    }
    if (!state.rtc_enabled) return;
    if (state.event_deadline &&
        *state.event_deadline <= state.scheduler_cycle) {
        throw std::invalid_argument(
            "RTC-Handoff-Tickevent liegt nicht hinter dem Schedulerzyklus.");
    }
    if ((state.event || state.event_rehydration_pending) &&
        !state.event_deadline) {
        throw std::invalid_argument(
            "RTC-Handoff-Tickevent besitzt keine Ereignisfrist.");
    }

    Sh4RtcClockDomain restored_clock(state.clock.guest_cycles_per_second);
    restored_clock.restore_state_passive(state.clock);
    if (state.event_deadline &&
        *state.event_deadline >
            restored_clock.deadline_after(state.scheduler_cycle, 64u)) {
        throw std::invalid_argument(
            "RTC-Handoff-Tickevent liegt ausserhalb der naechsten Tickperiode.");
    }
}

void Sh4Rtc::restore_state_passive(const Sh4RtcSnapshot& state) {
    validate_state_restore(state);
    cancel_event();
    clock_->restore_state_passive(state.clock);
    date_time_ = state.date_time;
    event_.reset();
    event_deadline_ = state.event_deadline;
    event_rehydration_pending_ = state.rtc_enabled;
    periodic_rate_ = state.periodic_rate;
    divider_256hz_phase_ = state.divider_256hz_phase;
    counter_64hz_ = state.counter_64hz;
    periodic_phase_ticks_ = state.periodic_phase_ticks;
    ticks_ = state.ticks;
    periodic_events_ = state.periodic_events;
    calendar_running_ = state.calendar_running;
    rtc_enabled_ = state.rtc_enabled;
    periodic_pending_ = state.periodic_pending;
    carry_flag_ = state.carry_flag;
    carry_enabled_ = state.carry_enabled;
    alarm_registers_ = state.alarm_registers;
    alarm_pending_ = state.alarm_pending;
    alarm_enabled_ = state.alarm_enabled;
}

SchedulerEventId Sh4Rtc::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != sh4_rtc_event_channel || token != sh4_rtc_event_token_v1) {
        throw std::invalid_argument(
            "RTC-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    }
    if (!rtc_enabled_ || event_ || !event_rehydration_pending_) {
        throw std::logic_error("RTC-Handoff erwartet kein Tickevent.");
    }
    if (guest_cycle < scheduler_.current_cycle() ||
        (event_deadline_ && guest_cycle != *event_deadline_)) {
        throw std::invalid_argument(
            "RTC-Handoff-Tickevent passt nicht zur erfassten Ereignisfrist.");
    }

    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this](const auto, const auto) { tick(); },
        SchedulerEventKind::Sh4Rtc);
    event_ = event_id;
    event_deadline_ = guest_cycle;
    event_rehydration_pending_ = false;
    return event_id;
}

bool Sh4Rtc::event_rehydration_pending() const noexcept {
    return event_rehydration_pending_;
}

std::vector<std::uint8_t> encode_sh4_rtc_clock_state(
    const Sh4RtcClockDomain::Snapshot& state) {
    static_cast<void>(Sh4RtcClockDomain(state.guest_cycles_per_second));
    TimerStateWriter writer;
    writer.raw(sh4_rtc_clock_state_magic);
    writer.u32(sh4_rtc_clock_state_contract_version);
    writer.u64(state.guest_cycles_per_second);
    writer.u64(state.epoch_cycle);
    return std::move(writer).finish();
}

Sh4RtcClockDomain::Snapshot decode_sh4_rtc_clock_state(
    const std::span<const std::uint8_t> bytes) {
    TimerStateReader reader(bytes);
    expect_magic(reader, sh4_rtc_clock_state_magic);
    if (reader.u32() != sh4_rtc_clock_state_contract_version)
        throw std::invalid_argument(
            "RTC-Taktdomaenen-Handoff besitzt eine unbekannte Vertragsversion.");
    Sh4RtcClockDomain::Snapshot state;
    state.guest_cycles_per_second = reader.u64();
    state.epoch_cycle = reader.u64();
    static_cast<void>(Sh4RtcClockDomain(state.guest_cycles_per_second));
    // Observer IDs and allocation state are process-local.
    state.next_observer_id = 0u;
    state.observer_ids.clear();
    reader.expect_end();
    return state;
}

std::vector<std::uint8_t> encode_sh4_rtc_state(
    const Sh4RtcSnapshot& state) {
    validate_portable_rtc_date_time(state.date_time);
    static_cast<void>(Sh4RtcClockDomain(state.clock.guest_cycles_per_second));
    if (state.clock.epoch_cycle > state.scheduler_cycle ||
        static_cast<std::uint8_t>(state.periodic_rate) >
            static_cast<std::uint8_t>(RtcPeriodicRate::Every2Seconds) ||
        state.divider_256hz_phase > 1u || state.counter_64hz > 0x7Fu ||
        (state.event && state.event_rehydration_pending) ||
        (state.rtc_enabled &&
         (!(state.event || state.event_rehydration_pending) ||
          !state.event_deadline)) ||
        (!state.rtc_enabled &&
         (state.event || state.event_deadline ||
          state.event_rehydration_pending)))
        throw std::invalid_argument(
            "RTC-Handoff-Payload besitzt inkonsistente Kalender-, Teiler- oder "
            "Eventdaten.");

    TimerStateWriter writer;
    writer.raw(sh4_rtc_state_magic);
    writer.u32(sh4_rtc_state_contract_version);
    writer.u16(state.date_time.year);
    writer.u8(state.date_time.month);
    writer.u8(state.date_time.day);
    writer.u8(state.date_time.day_of_week);
    writer.u8(state.date_time.hour);
    writer.u8(state.date_time.minute);
    writer.u8(state.date_time.second);
    writer.u64(state.clock.guest_cycles_per_second);
    writer.u64(state.clock.epoch_cycle);
    writer.u64(state.scheduler_cycle);
    writer.u32(static_cast<std::uint32_t>(state.periodic_rate));
    writer.u8(state.divider_256hz_phase);
    writer.u8(state.counter_64hz);
    writer.u64(state.periodic_phase_ticks);
    writer.u64(state.ticks);
    writer.u64(state.periodic_events);
    writer.boolean(state.calendar_running);
    writer.boolean(state.rtc_enabled);
    writer.boolean(state.periodic_pending);
    writer.boolean(state.carry_flag);
    writer.boolean(state.carry_enabled);
    writer.raw(state.alarm_registers);
    writer.boolean(state.alarm_pending);
    writer.boolean(state.alarm_enabled);
    return std::move(writer).finish();
}

Sh4RtcSnapshot decode_sh4_rtc_state(
    const std::span<const std::uint8_t> bytes) {
    TimerStateReader reader(bytes);
    expect_magic(reader, sh4_rtc_state_magic);
    if (reader.u32() != sh4_rtc_state_contract_version)
        throw std::invalid_argument(
            "RTC-Handoff-Payload besitzt eine unbekannte Vertragsversion.");

    Sh4RtcSnapshot state;
    state.date_time.year = reader.u16();
    state.date_time.month = reader.u8();
    state.date_time.day = reader.u8();
    state.date_time.day_of_week = reader.u8();
    state.date_time.hour = reader.u8();
    state.date_time.minute = reader.u8();
    state.date_time.second = reader.u8();
    validate_portable_rtc_date_time(state.date_time);
    state.clock.guest_cycles_per_second = reader.u64();
    state.clock.epoch_cycle = reader.u64();
    static_cast<void>(Sh4RtcClockDomain(state.clock.guest_cycles_per_second));
    state.clock.next_observer_id = 0u;
    state.clock.observer_ids.clear();
    state.scheduler_cycle = reader.u64();
    if (state.clock.epoch_cycle > state.scheduler_cycle)
        throw std::invalid_argument(
            "RTC-Handoff-Payload besitzt einen zukuenftigen Phasenursprung.");
    const auto periodic_rate = reader.u32();
    if (periodic_rate >
        static_cast<std::uint32_t>(RtcPeriodicRate::Every2Seconds))
        throw std::invalid_argument(
            "RTC-Handoff-Payload besitzt eine ungueltige periodische Rate.");
    state.periodic_rate = static_cast<RtcPeriodicRate>(periodic_rate);
    state.divider_256hz_phase = reader.u8();
    state.counter_64hz = reader.u8();
    if (state.divider_256hz_phase > 1u || state.counter_64hz > 0x7Fu)
        throw std::invalid_argument(
            "RTC-Handoff-Payload besitzt ungueltige Teilerwerte.");
    state.periodic_phase_ticks = reader.u64();
    state.ticks = reader.u64();
    state.periodic_events = reader.u64();
    state.calendar_running = reader.boolean();
    state.rtc_enabled = reader.boolean();
    state.periodic_pending = reader.boolean();
    state.carry_flag = reader.boolean();
    state.carry_enabled = reader.boolean();
    const auto alarm_registers = reader.raw(state.alarm_registers.size());
    std::copy(alarm_registers.begin(),
              alarm_registers.end(),
              state.alarm_registers.begin());
    state.alarm_pending = reader.boolean();
    state.alarm_enabled = reader.boolean();
    state.event.reset();
    state.event_deadline.reset();
    state.event_rehydration_pending = false;
    reader.expect_end();
    return state;
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
    const auto event_id = scheduler_.schedule_at(
        deadline,
        [this](const auto, const auto) { tick(); },
        SchedulerEventKind::Sh4Rtc);
    event_ = event_id;
    event_deadline_ = deadline;
    event_rehydration_pending_ = false;
}

void Sh4Rtc::cancel_event() noexcept {
    if (event_) {
        static_cast<void>(scheduler_.cancel(*event_));
        event_.reset();
    }
    event_deadline_.reset();
    event_rehydration_pending_ = false;
}

void Sh4Rtc::handle_scheduler_reset() {
    event_.reset();
    event_deadline_.reset();
    event_rehydration_pending_ = false;
    clock_->reset_phase(scheduler_.current_cycle());
    if (rtc_enabled_) {
        schedule_tick();
    }
}

void Sh4Rtc::tick() {
    event_.reset();
    event_deadline_.reset();
    event_rehydration_pending_ = false;
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
