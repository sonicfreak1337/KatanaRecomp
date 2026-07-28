#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t guest_cycle_contract_version = 2u;
inline constexpr std::uint64_t dreamcast_guest_cycles_per_second = 200'000'000u;

struct CrashCapsule;
class SystemReplayLog;

using SchedulerEventId = std::uint64_t;
using SchedulerResetObserverId = std::uint64_t;
using SchedulerLifetimeToken = std::weak_ptr<const void>;
using SchedulerCallback = std::function<void(SchedulerEventId event_id, std::uint64_t guest_cycle)>;
using SchedulerResetCallback = std::function<void()>;

enum class SchedulerEventKind : std::uint32_t {
    Unknown = 0u,
    DiscRead = 1u,
    Sh4Dmac = 2u,
    GdRomPacket = 3u,
    HollyG1Dma = 4u,
    HollyG2Dma = 5u,
    HollyPvrDma = 6u,
    MapleDma = 7u,
    MediaVideo = 8u,
    MediaAudio = 9u,
    PvrRender = 10u,
    PvrVblankIn = 11u,
    PvrVblankOut = 12u,
    PvrHblank = 13u,
    ScifTransmit = 14u,
    SystemAsic = 15u,
    Sh4Rtc = 16u,
    Sh4Tmu0 = 17u,
    Sh4Tmu1 = 18u,
    Sh4Tmu2 = 19u,
    AicaTick = 20u,
};

struct SchedulerPendingEventSnapshot {
    std::uint64_t guest_cycle = 0u;
    SchedulerEventId event_id = 0u;
    SchedulerEventKind kind = SchedulerEventKind::Unknown;

    [[nodiscard]] bool operator==(const SchedulerPendingEventSnapshot&) const = default;
};

struct EventSchedulerSnapshot {
    std::uint64_t current_cycle = 0u;
    SchedulerEventId next_event_id = 0u;
    SchedulerResetObserverId next_reset_observer_id = 0u;
    std::uint64_t processed_event_count = 0u;
    std::uint64_t reset_generation = 0u;
    std::optional<std::uint64_t> guest_cycle_budget;
    bool advance_in_progress = false;
    std::vector<SchedulerPendingEventSnapshot> pending_events;
    std::vector<SchedulerResetObserverId> reset_observer_ids;

    [[nodiscard]] bool operator==(const EventSchedulerSnapshot&) const = default;
};

enum class SchedulerAdvanceStatus {
    ReachedTarget,
    EventBudgetExhausted,
    GuestCycleBudgetExhausted,
};

struct SchedulerAdvanceResult {
    SchedulerAdvanceStatus status = SchedulerAdvanceStatus::ReachedTarget;
    std::size_t processed_events = 0u;
    std::uint64_t guest_cycle = 0u;
};

struct SchedulerPreparedEvent {
    std::uint64_t guest_cycle = 0u;
    SchedulerCallback callback;
    SchedulerEventKind kind = SchedulerEventKind::Unknown;
};

class EventScheduler {
  public:
    class PreparedPassiveRestore final {
      public:
        PreparedPassiveRestore(const PreparedPassiveRestore&) = delete;
        PreparedPassiveRestore& operator=(const PreparedPassiveRestore&) =
            delete;
        PreparedPassiveRestore(PreparedPassiveRestore&&) noexcept;
        PreparedPassiveRestore& operator=(
            PreparedPassiveRestore&&) noexcept;
        ~PreparedPassiveRestore();

        [[nodiscard]] std::span<const SchedulerEventId>
        assigned_event_ids() const noexcept;

      private:
        friend class EventScheduler;
        struct Data;

        PreparedPassiveRestore();
        std::unique_ptr<Data> data_;
    };

    explicit EventScheduler(SystemReplayLog* replay_log = nullptr);
    ~EventScheduler();
    [[nodiscard]] SchedulerEventId schedule_at(std::uint64_t guest_cycle,
                                               SchedulerCallback callback,
                                               SchedulerEventKind kind =
                                                   SchedulerEventKind::Unknown);
    [[nodiscard]] SchedulerEventId schedule_after(std::uint64_t guest_cycles,
                                                  SchedulerCallback callback,
                                                  SchedulerEventKind kind =
                                                      SchedulerEventKind::Unknown);
    [[nodiscard]] SchedulerEventId schedule_at_or_now(std::uint64_t requested_guest_cycle,
                                                      SchedulerCallback callback,
                                                      SchedulerEventKind kind =
                                                          SchedulerEventKind::Unknown);

    [[nodiscard]] bool cancel(SchedulerEventId event_id) noexcept;
    [[nodiscard]] SchedulerResetObserverId add_reset_observer(SchedulerResetCallback callback);
    [[nodiscard]] bool remove_reset_observer(SchedulerResetObserverId observer_id) noexcept;
    void clear() noexcept;
    void reset();
    // Clears process-local callbacks and installs an already elapsed guest
    // cycle without notifying reset observers. Device state must be restored
    // afterwards and every typed pending event must receive a fresh ID.
    void restore_time_passive(std::uint64_t guest_cycle);
    // Builds the full typed callback timeline and allocates all map nodes
    // before any live scheduler or device state changes. Input order is
    // preserved by assigned_event_ids().
    [[nodiscard]] PreparedPassiveRestore prepare_passive_restore(
        std::uint64_t guest_cycle,
        std::span<const SchedulerPreparedEvent> events) const;
    // Replaces time and pending callbacks by swapping preallocated maps.
    // The scheduler must remain quiescent between prepare and commit.
    void commit_prepared_passive_restore(
        PreparedPassiveRestore prepared) noexcept;

    [[nodiscard]] SchedulerAdvanceResult advance_to(std::uint64_t guest_cycle,
                                                    std::size_t event_budget);
    [[nodiscard]] SchedulerAdvanceResult advance_by(std::uint64_t guest_cycles,
                                                    std::size_t event_budget);

    [[nodiscard]] std::uint64_t current_cycle() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_event_cycle() const noexcept;
    [[nodiscard]] std::size_t pending_event_count() const noexcept;
    [[nodiscard]] std::uint64_t processed_event_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_generation() const noexcept;
    [[nodiscard]] EventSchedulerSnapshot snapshot() const;
    [[nodiscard]] SchedulerLifetimeToken lifetime_token() const noexcept;
    void attach_crash_capsule(CrashCapsule& capsule) noexcept;
    void detach_crash_capsule(const CrashCapsule& capsule) noexcept;
    void attach_replay_log(SystemReplayLog& replay_log);
    void set_guest_cycle_budget(std::optional<std::uint64_t> maximum_cycle);
    // Arms one immutable budget relative to the already elapsed guest time.
    // Failure is reported without throwing so noexcept executable-entry
    // callbacks can fail closed on zero, overflow or an existing budget.
    [[nodiscard]] std::optional<std::uint64_t>
    try_set_guest_cycle_budget_after_current_cycle(
        std::uint64_t guest_cycles) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> guest_cycle_budget() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> remaining_guest_cycles() const noexcept;

  private:
    using EventKey = std::pair<std::uint64_t, SchedulerEventId>;
    struct ScheduledEvent {
        SchedulerCallback callback;
        SchedulerEventKind kind = SchedulerEventKind::Unknown;
    };

    std::uint64_t current_cycle_ = 0u;
    SchedulerEventId next_event_id_ = 1u;
    SchedulerResetObserverId next_reset_observer_id_ = 1u;
    std::uint64_t processed_event_count_ = 0u;
    std::uint64_t reset_generation_ = 0u;
    std::optional<std::uint64_t> guest_cycle_budget_;
    bool advance_in_progress_ = false;
    std::map<EventKey, ScheduledEvent> events_;
    std::unordered_map<SchedulerEventId, EventKey> event_keys_;
    std::map<SchedulerResetObserverId, SchedulerResetCallback> reset_observers_;
    CrashCapsule* crash_capsule_ = nullptr;
    SystemReplayLog* replay_log_ = nullptr;
    std::shared_ptr<const void> lifetime_token_ = std::make_shared<std::uint8_t>();
};

[[nodiscard]] std::uint64_t parse_guest_cycle_budget(std::string_view text);
[[nodiscard]] std::optional<std::uint64_t> guest_cycle_budget_from_environment();

} // namespace katana::runtime
