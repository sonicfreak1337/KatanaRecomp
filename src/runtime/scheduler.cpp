#include "katana/runtime/scheduler.hpp"

#include "katana/runtime/system_replay.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace katana::runtime {

EventScheduler::EventScheduler(SystemReplayLog* replay_log) : replay_log_(replay_log) {}

EventScheduler::~EventScheduler() {
    lifetime_token_.reset();
}

SchedulerEventId EventScheduler::schedule_at(const std::uint64_t guest_cycle,
                                             SchedulerCallback callback,
                                             const SchedulerEventKind kind) {
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
    events_.emplace(key, ScheduledEvent{std::move(callback), kind});
    event_keys_.emplace(event_id, key);
    return event_id;
}

SchedulerEventId EventScheduler::schedule_after(const std::uint64_t guest_cycles,
                                                SchedulerCallback callback,
                                                const SchedulerEventKind kind) {
    if (guest_cycles > std::numeric_limits<std::uint64_t>::max() - current_cycle_) {
        throw std::overflow_error("Scheduler-Zielzyklus ist uebergelaufen.");
    }
    return schedule_at(current_cycle_ + guest_cycles, std::move(callback), kind);
}

SchedulerEventId EventScheduler::schedule_at_or_now(const std::uint64_t requested_guest_cycle,
                                                    SchedulerCallback callback,
                                                    const SchedulerEventKind kind) {
    return schedule_at(
        std::max(requested_guest_cycle, current_cycle_), std::move(callback), kind);
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

SchedulerResetObserverId EventScheduler::add_reset_observer(SchedulerResetCallback callback) {
    if (!callback) {
        throw std::invalid_argument("Scheduler-Resetbeobachter benoetigt einen Callback.");
    }
    if (next_reset_observer_id_ == std::numeric_limits<SchedulerResetObserverId>::max()) {
        throw std::overflow_error("Scheduler-Resetbeobachter-ID ist uebergelaufen.");
    }
    const auto observer_id = next_reset_observer_id_++;
    reset_observers_.emplace(observer_id, std::move(callback));
    return observer_id;
}

bool EventScheduler::remove_reset_observer(const SchedulerResetObserverId observer_id) noexcept {
    return reset_observers_.erase(observer_id) != 0u;
}

void EventScheduler::clear() noexcept {
    events_.clear();
    event_keys_.clear();
}

void EventScheduler::reset() {
    if (advance_in_progress_) {
        throw std::logic_error(
            "Scheduler-Reset ist waehrend eines laufenden Advances nicht erlaubt.");
    }
    if (reset_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Scheduler-Resetgeneration ist uebergelaufen.");
    }
    clear();
    current_cycle_ = 0u;
    processed_event_count_ = 0u;
    ++reset_generation_;
    if (replay_log_ != nullptr) {
        try {
            SystemReplayEvent event{0u,
                                    0u,
                                    SystemReplayEventKind::SchedulerCallback,
                                    "scheduler-reset",
                                    std::nullopt,
                                    std::nullopt,
                                    reset_generation_,
                                    0u,
                                    false};
            event.time_epoch = reset_generation_;
            static_cast<void>(replay_log_->try_record(std::move(event)));
        } catch (...) {
            replay_log_->note_dropped_event();
        }
    }

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

SchedulerAdvanceResult EventScheduler::advance_to(const std::uint64_t guest_cycle,
                                                  const std::size_t event_budget) {
    if (advance_in_progress_) {
        throw std::logic_error("Rekursives Scheduler-Advance ist nicht erlaubt.");
    }
    if (guest_cycle < current_cycle_) {
        throw std::invalid_argument("Scheduler-Zyklusuhr darf nicht rueckwaerts laufen.");
    }

    struct AdvanceGuard final {
        explicit AdvanceGuard(bool& state) noexcept : state_(state) {
            state_ = true;
        }
        ~AdvanceGuard() {
            state_ = false;
        }
        bool& state_;
    } guard(advance_in_progress_);

    const auto effective_target =
        guest_cycle_budget_ ? std::min(guest_cycle, *guest_cycle_budget_) : guest_cycle;
    std::size_t processed = 0u;
    while (!events_.empty() && events_.begin()->first.first <= effective_target) {
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
        if (replay_log_ != nullptr) {
            try {
                SystemReplayEvent replay_event{0u,
                                               deadline,
                                               SystemReplayEventKind::SchedulerCallback,
                                               system_replay_scheduler_event_code(
                                                   event.mapped().kind),
                                               std::nullopt,
                                               static_cast<std::uint32_t>(
                                                   event.mapped().kind),
                                               event_id,
                                               processed_event_count_,
                                               false};
                replay_event.time_epoch = reset_generation_;
                static_cast<void>(replay_log_->try_record(std::move(replay_event)));
            } catch (...) {
                replay_log_->note_dropped_event();
            }
        }
        event.mapped().callback(event_id, deadline);
    }

    current_cycle_ = effective_target;
    return {
        effective_target == guest_cycle ? SchedulerAdvanceStatus::ReachedTarget
                                        : SchedulerAdvanceStatus::GuestCycleBudgetExhausted,
        processed,
        current_cycle_,
    };
}

SchedulerAdvanceResult EventScheduler::advance_by(const std::uint64_t guest_cycles,
                                                  const std::size_t event_budget) {
    if (advance_in_progress_) {
        throw std::logic_error("Rekursives Scheduler-Advance ist nicht erlaubt.");
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

EventSchedulerSnapshot EventScheduler::snapshot() const {
    EventSchedulerSnapshot result;
    result.current_cycle = current_cycle_;
    result.next_event_id = next_event_id_;
    result.next_reset_observer_id = next_reset_observer_id_;
    result.processed_event_count = processed_event_count_;
    result.reset_generation = reset_generation_;
    result.guest_cycle_budget = guest_cycle_budget_;
    result.advance_in_progress = advance_in_progress_;
    result.pending_events.reserve(events_.size());
    for (const auto& [key, event] : events_) {
        result.pending_events.push_back({key.first, key.second, event.kind});
    }
    result.reset_observer_ids.reserve(reset_observers_.size());
    for (const auto& [id, callback] : reset_observers_) {
        static_cast<void>(callback);
        result.reset_observer_ids.push_back(id);
    }
    return result;
}

SchedulerLifetimeToken EventScheduler::lifetime_token() const noexcept {
    return lifetime_token_;
}

void EventScheduler::attach_replay_log(SystemReplayLog& replay_log) {
    if (replay_log_ != nullptr) {
        throw std::logic_error("Scheduler besitzt bereits eine Systemreplay-Aufzeichnung.");
    }
    if (current_cycle_ != 0u || processed_event_count_ != 0u || reset_generation_ != 0u) {
        throw std::logic_error(
            "Systemreplay muss vor dem ersten Schedulerfortschritt angebunden werden.");
    }
    if (replay_log.sealed()) {
        throw std::invalid_argument(
            "Versiegelter Systemreplay kann nicht an den Scheduler gebunden werden.");
    }
    if (!replay_log.events().empty() || replay_log.dropped_events() != 0u) {
        throw std::invalid_argument(
            "Scheduler-Replay muss leer und vollstaendig beginnen.");
    }
    replay_log.enable_coverage(static_cast<SystemReplayCoverageMask>(
        SystemReplayCoverage::SchedulerCallback));
    replay_log_ = &replay_log;
}

void EventScheduler::set_guest_cycle_budget(const std::optional<std::uint64_t> maximum_cycle) {
    if (maximum_cycle && *maximum_cycle == 0u) {
        throw std::invalid_argument("Gastzyklusbudget muss groesser null sein.");
    }
    if (guest_cycle_budget_ && maximum_cycle != guest_cycle_budget_) {
        throw std::logic_error("Gastzyklusbudget eines Laufs ist unveraenderlich.");
    }
    if (!guest_cycle_budget_ && maximum_cycle && current_cycle_ != 0u) {
        throw std::logic_error("Gastzyklusbudget muss vor dem ersten Advance gesetzt werden.");
    }
    if (maximum_cycle && *maximum_cycle < current_cycle_) {
        throw std::invalid_argument(
            "Gastzyklusbudget darf nicht vor dem aktuellen Schedulerzyklus liegen.");
    }
    guest_cycle_budget_ = maximum_cycle;
}

std::optional<std::uint64_t> EventScheduler::guest_cycle_budget() const noexcept {
    return guest_cycle_budget_;
}

std::optional<std::uint64_t> EventScheduler::remaining_guest_cycles() const noexcept {
    if (!guest_cycle_budget_) return std::nullopt;
    return *guest_cycle_budget_ - current_cycle_;
}

std::uint64_t parse_guest_cycle_budget(const std::string_view text) {
    std::uint64_t value = 0u;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value == 0u) {
        throw std::invalid_argument(
            "KATANA_GUEST_CYCLE_BUDGET muss eine positive 64-Bit-Ganzzahl sein.");
    }
    return value;
}

std::optional<std::uint64_t> guest_cycle_budget_from_environment() {
#ifdef _WIN32
    char* configured = nullptr;
    std::size_t configured_size = 0u;
    const auto error = _dupenv_s(&configured, &configured_size, "KATANA_GUEST_CYCLE_BUDGET");
    const std::unique_ptr<char, decltype(&std::free)> owned(configured, &std::free);
    if (error != 0 || !owned) return std::nullopt;
    return parse_guest_cycle_budget(owned.get());
#else
    const auto* const configured = std::getenv("KATANA_GUEST_CYCLE_BUDGET");
    if (configured == nullptr) return std::nullopt;
    return parse_guest_cycle_budget(configured);
#endif
}

} // namespace katana::runtime
