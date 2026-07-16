#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace katana::runtime {

class SystemReplayLog;

using SchedulerEventId = std::uint64_t;
using SchedulerResetObserverId = std::uint64_t;
using SchedulerCallback =
    std::function<void(SchedulerEventId event_id, std::uint64_t guest_cycle)>;
using SchedulerResetCallback = std::function<void()>;

enum class SchedulerAdvanceStatus {
    ReachedTarget,
    EventBudgetExhausted,
};

struct SchedulerAdvanceResult {
    SchedulerAdvanceStatus status = SchedulerAdvanceStatus::ReachedTarget;
    std::size_t processed_events = 0u;
    std::uint64_t guest_cycle = 0u;
};

class EventScheduler {
public:
    explicit EventScheduler(SystemReplayLog* replay_log = nullptr) noexcept;
    [[nodiscard]] SchedulerEventId schedule_at(
        std::uint64_t guest_cycle,
        SchedulerCallback callback
    );
    [[nodiscard]] SchedulerEventId schedule_after(
        std::uint64_t guest_cycles,
        SchedulerCallback callback
    );

    [[nodiscard]] bool cancel(SchedulerEventId event_id) noexcept;
    [[nodiscard]] SchedulerResetObserverId add_reset_observer(
        SchedulerResetCallback callback
    );
    [[nodiscard]] bool remove_reset_observer(
        SchedulerResetObserverId observer_id
    ) noexcept;
    void clear() noexcept;
    void reset();

    [[nodiscard]] SchedulerAdvanceResult advance_to(
        std::uint64_t guest_cycle,
        std::size_t event_budget
    );
    [[nodiscard]] SchedulerAdvanceResult advance_by(
        std::uint64_t guest_cycles,
        std::size_t event_budget
    );

    [[nodiscard]] std::uint64_t current_cycle() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_event_cycle() const noexcept;
    [[nodiscard]] std::size_t pending_event_count() const noexcept;
    [[nodiscard]] std::uint64_t processed_event_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_generation() const noexcept;

private:
    using EventKey = std::pair<std::uint64_t, SchedulerEventId>;

    std::uint64_t current_cycle_ = 0u;
    SchedulerEventId next_event_id_ = 1u;
    SchedulerResetObserverId next_reset_observer_id_ = 1u;
    std::uint64_t processed_event_count_ = 0u;
    std::uint64_t reset_generation_ = 0u;
    bool advance_in_progress_ = false;
    std::map<EventKey, SchedulerCallback> events_;
    std::unordered_map<SchedulerEventId, EventKey> event_keys_;
    std::map<SchedulerResetObserverId, SchedulerResetCallback> reset_observers_;
    SystemReplayLog* replay_log_ = nullptr;
};

} // namespace katana::runtime
