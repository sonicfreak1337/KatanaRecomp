#include "katana/runtime/scheduler.hpp"

#include <limits>
#include <stdexcept>
#include <vector>

namespace katana::runtime {

SchedulerEventId EventScheduler::schedule_at(
    const std::uint64_t guest_cycle,
    SchedulerCallback callback
) {
    if (!callback) {
        throw std::invalid_argument("Scheduler-Ereignis benoetigt einen Callback.");
    }
    if (guest_cycle < current_cycle_) {
        throw std::invalid_argument("Scheduler-Ereignis darf nicht in der Vergangenheit liegen.");
    }
    if (next_event_id_ == std::numeric_limits<SchedulerEventId>::max()) {
        throw std::overflow_error("Scheduler-Ereignis-ID ist uebergelaufen.");
    }

    const SchedulerEventId event_id = next_event_id_++;
    const EventKey key{guest_cycle, event_id};
    events_.emplace(key, std::move(callback));
    event_keys_.emplace(event_id, key);
    return event_id;
}

SchedulerEventId EventScheduler::schedule_after(
    const std::uint64_t guest_cycles,
    SchedulerCallback callback
) {
    if (guest_cycles > std::numeric_limits<std::uint64_t>::max() - current_cycle_) {
        throw std::overflow_error("Scheduler-Zielzyklus ist uebergelaufen.");
    }
    return schedule_at(current_cycle_ + guest_cycles, std::move(callback));
}

bool EventScheduler::cancel(const SchedulerEventId event_id) noexcept {
    const auto key = event_keys_.find(event_id);
    if (key == event_keys_.end()) {
        return false;
    }
    events_.erase(key->second);
    event_keys_.erase(key);
    return true;
}

SchedulerResetObserverId EventScheduler::add_reset_observer(
    SchedulerResetCallback callback
) {
    if (!callback) {
        throw std::invalid_argument("Scheduler-Resetbeobachter benoetigt einen Callback.");
    }
    if (next_reset_observer_id_ ==
        std::numeric_limits<SchedulerResetObserverId>::max()) {
        throw std::overflow_error("Scheduler-Resetbeobachter-ID ist uebergelaufen.");
    }
    const auto observer_id = next_reset_observer_id_++;
    reset_observers_.emplace(observer_id, std::move(callback));
    return observer_id;
}

bool EventScheduler::remove_reset_observer(
    const SchedulerResetObserverId observer_id
) noexcept {
    return reset_observers_.erase(observer_id) != 0u;
}

void EventScheduler::clear() noexcept {
    events_.clear();
    event_keys_.clear();
}

void EventScheduler::reset() {
    if (advance_in_progress_) {
        throw std::logic_error(
            "Scheduler-Reset ist waehrend eines laufenden Advances nicht erlaubt."
        );
    }
    clear();
    current_cycle_ = 0u;
    processed_event_count_ = 0u;
    ++reset_generation_;

    std::vector<SchedulerResetCallback> observers;
    observers.reserve(reset_observers_.size());
    for (const auto& [id, callback] : reset_observers_) {
        static_cast<void>(id);
        observers.push_back(callback);
    }
    for (const auto& callback : observers) {
        callback();
    }
}

SchedulerAdvanceResult EventScheduler::advance_to(
    const std::uint64_t guest_cycle,
    const std::size_t event_budget
) {
    if (advance_in_progress_) {
        throw std::logic_error(
            "Rekursives Scheduler-Advance ist nicht erlaubt."
        );
    }
    if (guest_cycle < current_cycle_) {
        throw std::invalid_argument("Scheduler-Zyklusuhr darf nicht rueckwaerts laufen.");
    }

    struct AdvanceGuard final {
        explicit AdvanceGuard(bool& state) noexcept : state_(state) { state_ = true; }
        ~AdvanceGuard() { state_ = false; }
        bool& state_;
    } guard(advance_in_progress_);

    std::size_t processed = 0u;
    while (!events_.empty() && events_.begin()->first.first <= guest_cycle) {
        if (processed == event_budget) {
            return {
                SchedulerAdvanceStatus::EventBudgetExhausted,
                processed,
                current_cycle_,
            };
        }

        auto event = events_.extract(events_.begin());
        const auto [deadline, event_id] = event.key();
        event_keys_.erase(event_id);
        current_cycle_ = deadline;
        ++processed;
        ++processed_event_count_;
        event.mapped()(event_id, deadline);
    }

    current_cycle_ = guest_cycle;
    return {
        SchedulerAdvanceStatus::ReachedTarget,
        processed,
        current_cycle_,
    };
}

SchedulerAdvanceResult EventScheduler::advance_by(
    const std::uint64_t guest_cycles,
    const std::size_t event_budget
) {
    if (advance_in_progress_) {
        throw std::logic_error(
            "Rekursives Scheduler-Advance ist nicht erlaubt."
        );
    }
    if (guest_cycles > std::numeric_limits<std::uint64_t>::max() - current_cycle_) {
        throw std::overflow_error("Scheduler-Zielzyklus ist uebergelaufen.");
    }
    return advance_to(current_cycle_ + guest_cycles, event_budget);
}

std::uint64_t EventScheduler::current_cycle() const noexcept {
    return current_cycle_;
}

std::optional<std::uint64_t> EventScheduler::next_event_cycle() const noexcept {
    if (events_.empty()) {
        return std::nullopt;
    }
    return events_.begin()->first.first;
}

std::size_t EventScheduler::pending_event_count() const noexcept {
    return events_.size();
}

std::uint64_t EventScheduler::processed_event_count() const noexcept {
    return processed_event_count_;
}

std::uint64_t EventScheduler::reset_generation() const noexcept {
    return reset_generation_;
}

} // namespace katana::runtime
