#include "katana/runtime/scheduler.hpp"

#include <limits>
#include <stdexcept>

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

void EventScheduler::clear() noexcept {
    events_.clear();
    event_keys_.clear();
}

void EventScheduler::reset() noexcept {
    clear();
    current_cycle_ = 0u;
    next_event_id_ = 1u;
    processed_event_count_ = 0u;
}

SchedulerAdvanceResult EventScheduler::advance_to(
    const std::uint64_t guest_cycle,
    const std::size_t event_budget
) {
    if (guest_cycle < current_cycle_) {
        throw std::invalid_argument("Scheduler-Zyklusuhr darf nicht rueckwaerts laufen.");
    }

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

} // namespace katana::runtime
