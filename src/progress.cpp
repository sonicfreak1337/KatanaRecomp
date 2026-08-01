#include "katana/progress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana {
namespace detail {

class ProgressCore;

struct ProgressWorkerSignal final {
    void notify() noexcept {
        revision.fetch_add(1u, std::memory_order_release);
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<std::uint64_t> revision{0u};
};

class ProgressHeartbeatService final {
  public:
    ProgressHeartbeatService();
    ~ProgressHeartbeatService();

    [[nodiscard]] bool register_core(const std::shared_ptr<ProgressCore>& core) noexcept;

  private:
    struct WorkerSlot;
    void reap_finished_locked() noexcept;

    std::mutex mutex_;
    std::vector<std::unique_ptr<WorkerSlot>> workers_;
    bool stopping_ = false;
};

[[nodiscard]] ProgressHeartbeatService& progress_heartbeat_service() {
    static ProgressHeartbeatService service;
    return service;
}

struct ProgressDeliveryState final {
    // Protected by the owning ProgressCore's state mutex. Keeping the
    // revision on the scope-owned state avoids permanent per-scope
    // tombstones in a long-lived reporter.
    std::uint64_t latest_revision = 0u;
    std::atomic_bool terminal = false;
};

class ProgressCore final {
  public:
    ProgressCore(ProgressCallback callback,
                 const std::chrono::milliseconds minimum_update_interval,
                 const std::chrono::milliseconds heartbeat_interval)
        : callback_(std::move(callback)),
          minimum_update_interval_(std::max(minimum_update_interval, std::chrono::milliseconds(0))),
          heartbeat_interval_(std::clamp(
              heartbeat_interval, std::chrono::milliseconds(1), std::chrono::milliseconds(1000))),
          epoch_(std::chrono::steady_clock::now()),
          worker_signal_(std::make_shared<ProgressWorkerSignal>()) {}

    ~ProgressCore() {
        worker_signal_->notify();
    }

    [[nodiscard]] std::shared_ptr<ProgressWorkerSignal> worker_signal() const noexcept {
        return worker_signal_;
    }

    [[nodiscard]] std::uint64_t next_scope_id() noexcept {
        return next_scope_id_.fetch_add(1u, std::memory_order_relaxed);
    }

    [[nodiscard]] std::chrono::milliseconds minimum_update_interval() const noexcept {
        return minimum_update_interval_;
    }

    [[nodiscard]] std::uint64_t dropped_observations() const noexcept {
        return dropped_observations_.load(std::memory_order_acquire);
    }

    void record_external_observation_loss(
        const std::uint64_t amount) noexcept {
        record_dropped_observations(amount);
    }

    [[nodiscard]] bool flush() noexcept {
        try {
            std::unique_lock lock(state_mutex_);
            if (active_callback_admission_ != 0u &&
                callback_thread_ == std::this_thread::get_id())
                return false;
            const auto target_admission = last_callback_admission_;
            callback_fence_.wait(
                lock,
                [&] { return callback_fence_satisfied_locked(target_admission); });
            return observation_stream_loss_free_locked();
        } catch (...) {
            record_dropped_observations();
            return false;
        }
    }

    [[nodiscard]] bool seal_and_flush() noexcept {
        try {
            std::unique_lock lock(state_mutex_);
            // Sealing is permanent. A callback cannot wait for its own
            // admission, so reject this context before making any lifecycle
            // mutation; returning false must not silently poison the reporter.
            if (active_callback_admission_ != 0u &&
                callback_thread_ == std::this_thread::get_id())
                return false;
            if (!sealed_) {
                sealed_ = true;
                if (active_scope_count_ != 0u)
                    seal_incomplete_ = true;
                // No heartbeat may be admitted after the seal. Scope-owned
                // delivery state remains authoritative for any late method
                // calls, while the bookkeeping snapshots can be released.
                active_scopes_.clear();
            }
            producer_fence_.wait(lock, [&] {
                return active_producers_ == 0u;
            });
            const auto target_admission = last_callback_admission_;
            callback_fence_.wait(lock, [&] {
                return callback_fence_satisfied_locked(target_admission);
            });
            return telemetry_complete_locked();
        } catch (...) {
            record_dropped_observations();
            return false;
        }
    }

    [[nodiscard]] bool reserve_scope_start() noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            if (sealed_) {
                record_dropped_observations();
                return false;
            }
            if (active_scope_count_ ==
                std::numeric_limits<std::uint64_t>::max()) {
                record_dropped_observations();
                return false;
            }
            if (!reserve_producer_locked()) return false;
            ++active_scope_count_;
            return true;
        } catch (...) {
            record_dropped_observations();
            return false;
        }
    }

    [[nodiscard]] bool reserve_scope_update() noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            if (sealed_) {
                record_dropped_observations();
                return false;
            }
            return reserve_producer_locked();
        } catch (...) {
            record_dropped_observations();
            return false;
        }
    }

    [[nodiscard]] bool reserve_scope_terminal(
        const bool registered_scope) noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            if (!registered_scope || sealed_) {
                record_dropped_observations();
                return false;
            }
            if (active_scope_count_ == 0u) {
                record_dropped_observations();
                return false;
            }
            if (!reserve_producer_locked()) return false;
            --active_scope_count_;
            return true;
        } catch (...) {
            record_dropped_observations();
            return false;
        }
    }

    void discard_reserved_emission() noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            record_dropped_observations();
            resolve_producer_locked();
        } catch (...) {
            record_dropped_observations();
        }
    }

    void discard_reserved_scope_start() noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            record_dropped_observations();
            if (active_scope_count_ == 0u)
                record_dropped_observations();
            else
                --active_scope_count_;
            resolve_producer_locked();
        } catch (...) {
            record_dropped_observations();
        }
    }

    void discard_registered_scope() noexcept {
        try {
            std::scoped_lock lock(state_mutex_);
            record_dropped_observations();
            if (active_scope_count_ == 0u)
                record_dropped_observations();
            else
                --active_scope_count_;
        } catch (...) {
            record_dropped_observations();
        }
    }

    void emit_reserved(
        ProgressEvent event,
        const std::uint64_t scope_revision,
        const std::shared_ptr<ProgressDeliveryState>& delivery_state) noexcept {
        bool queued_callback = false;
        try {
            std::unique_lock lock(state_mutex_);
            if (!delivery_state ||
                scope_revision <= delivery_state->latest_revision ||
                (!terminal(event.state) &&
                 delivery_state->terminal.load(std::memory_order_acquire))) {
                resolve_producer_locked();
                return;
            }
            delivery_state->latest_revision = scope_revision;
            const auto now = std::chrono::steady_clock::now();
            if (terminal(event.state)) {
                active_scopes_.erase(event.scope_id);
            } else if (!sealed_) {
                try {
                    auto scope_started = now;
                    if (const auto active = active_scopes_.find(event.scope_id);
                        active != active_scopes_.end()) {
                        scope_started = active->second.scope_started;
                    } else {
                        using MillisecondsRep = std::chrono::milliseconds::rep;
                        const auto maximum =
                            static_cast<std::uint64_t>(std::numeric_limits<MillisecondsRep>::max());
                        const auto elapsed = std::chrono::milliseconds(static_cast<MillisecondsRep>(
                            std::min(event.scope_elapsed_milliseconds, maximum)));
                        scope_started = now - elapsed;
                    }
                    active_scopes_.insert_or_assign(
                        event.scope_id, ActiveScope{event, now, scope_started, delivery_state});
                } catch (...) {
                    // Foreground progress remains valid if heartbeat bookkeeping cannot grow.
                    record_dropped_observations();
                }
            }
            queued_callback =
                enqueue_callback_locked(std::move(event), now);
            resolve_producer_locked();
        } catch (...) {
            // Progress reporting is observational. A broken UI/log sink must not corrupt or
            // cancel the operation it observes; cancellation has an explicit checkpoint path.
            record_dropped_observations();
            try {
                std::scoped_lock lock(state_mutex_);
                resolve_producer_locked();
            } catch (...) {
            }
        }
        if (queued_callback) worker_signal_->notify();
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    emit_due_heartbeats(const std::chrono::steady_clock::time_point now) noexcept {
        bool queued_callback = false;
        std::optional<std::chrono::steady_clock::time_point> result;
        try {
            std::unique_lock lock(state_mutex_);
            if (sealed_ || active_scopes_.empty()) return std::nullopt;
            auto next_deadline = std::chrono::steady_clock::time_point::max();
            for (auto active = active_scopes_.begin(); active != active_scopes_.end();) {
                const auto delivery_state = active->second.delivery_state.lock();
                if (!delivery_state || delivery_state->terminal.load(std::memory_order_acquire)) {
                    active = active_scopes_.erase(active);
                    continue;
                }
                const auto deadline = active->second.last_emission + heartbeat_interval_;
                if (now >= deadline) {
                    active->second.last_emission = now;
                    ProgressEvent event;
                    try {
                        event = active->second.event;
                    } catch (...) {
                        // Keep the loss and the heartbeat attempt in the same
                        // state-mutex epoch. Otherwise seal_and_flush() could
                        // acquire the mutex after unwinding, return success,
                        // and only then observe this allocation failure.
                        record_dropped_observations();
                        next_deadline =
                            std::min(next_deadline, now + heartbeat_interval_);
                        ++active;
                        continue;
                    }
                    event.state = ProgressState::Heartbeat;
                    const auto scope_elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - active->second.scope_started);
                    event.scope_elapsed_milliseconds = static_cast<std::uint64_t>(
                        std::max(scope_elapsed, std::chrono::milliseconds(0)).count());
                    queued_callback =
                        enqueue_callback_locked(std::move(event), now) ||
                        queued_callback;
                    next_deadline = std::min(next_deadline, now + heartbeat_interval_);
                } else {
                    next_deadline = std::min(next_deadline, deadline);
                }
                ++active;
            }
            if (!active_scopes_.empty()) result = next_deadline;
        } catch (...) {
            // A failed heartbeat allocation must not affect the observed work.
            record_dropped_observations();
            result = now + std::min(heartbeat_interval_, std::chrono::milliseconds(100));
        }
        if (queued_callback) worker_signal_->notify();
        return result;
    }

    [[nodiscard]] bool drain_callback_batch() noexcept {
        constexpr std::size_t maximum_batch = 256u;
        for (std::size_t index = 0u; index < maximum_batch; ++index) {
            std::optional<PendingCallback> pending;
            {
                const std::lock_guard lock(state_mutex_);
                if (pending_callbacks_.empty()) return false;
                pending.emplace(std::move(pending_callbacks_.front()));
                pending_callbacks_.pop_front();
                active_callback_admission_ = pending->admission;
                callback_thread_ = std::this_thread::get_id();
                pending->event.sequence = next_sequence_++;
            }
            auto delivered = true;
            try {
                callback_(pending->event);
            } catch (...) {
                delivered = false;
                record_dropped_observations();
            }
            {
                const std::lock_guard lock(state_mutex_);
                if (delivered)
                    ++delivered_callbacks_;
                else
                    ++discarded_callbacks_;
                last_resolved_admission_ = pending->admission;
                active_callback_admission_ = 0u;
                callback_thread_ = {};
                callback_fence_.notify_all();
            }
        }
        const std::lock_guard lock(state_mutex_);
        return !pending_callbacks_.empty();
    }

  private:
    struct PendingCallback final {
        ProgressEvent event;
        std::uint64_t admission = 0u;
    };

    struct ActiveScope final {
        ProgressEvent event;
        std::chrono::steady_clock::time_point last_emission;
        std::chrono::steady_clock::time_point scope_started;
        std::weak_ptr<ProgressDeliveryState> delivery_state;
    };

    [[nodiscard]] static bool terminal(const ProgressState state) noexcept {
        return state == ProgressState::Completed || state == ProgressState::Cached ||
               state == ProgressState::Skipped || state == ProgressState::Failed;
    }

    [[nodiscard]] bool
    callback_fence_satisfied_locked(const std::uint64_t target_admission) const noexcept {
        return last_resolved_admission_ >= target_admission;
    }

    [[nodiscard]] bool telemetry_complete_locked() const noexcept {
        return !seal_incomplete_ && observation_stream_loss_free_locked() &&
               admitted_producers_ == resolved_producers_ &&
               last_callback_admission_ ==
                   delivered_callbacks_ + discarded_callbacks_;
    }

    [[nodiscard]] bool observation_stream_loss_free_locked() const noexcept {
        return dropped_observations_.load(std::memory_order_relaxed) == 0u &&
               discarded_callbacks_ == 0u;
    }

    void record_dropped_observations(const std::uint64_t amount = 1u) noexcept {
        if (amount == 0u) return;
        auto observed = dropped_observations_.load(std::memory_order_relaxed);
        while (observed != std::numeric_limits<std::uint64_t>::max()) {
            const auto desired = amount > std::numeric_limits<std::uint64_t>::max() - observed
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : observed + amount;
            if (dropped_observations_.compare_exchange_weak(
                    observed, desired, std::memory_order_relaxed, std::memory_order_relaxed))
                return;
        }
    }

    [[nodiscard]] bool reserve_producer_locked() noexcept {
        if (active_producers_ ==
                std::numeric_limits<std::uint64_t>::max() ||
            admitted_producers_ ==
                std::numeric_limits<std::uint64_t>::max()) {
            record_dropped_observations();
            return false;
        }
        ++active_producers_;
        ++admitted_producers_;
        return true;
    }

    void resolve_producer_locked() noexcept {
        if (active_producers_ == 0u) {
            record_dropped_observations();
            return;
        }
        --active_producers_;
        ++resolved_producers_;
        if (active_producers_ == 0u)
            producer_fence_.notify_all();
    }

    [[nodiscard]] bool enqueue_callback_locked(
        ProgressEvent event,
        const std::chrono::steady_clock::time_point now) noexcept {
        constexpr std::size_t max_pending_callbacks = 4096u;
        constexpr std::size_t low_priority_callback_limit = 3072u;
        const auto low_priority =
            event.state == ProgressState::Running || event.state == ProgressState::Heartbeat;
        // Producers never drain callbacks and never wait for an observer.
        // The final quarter is reserved for lifecycle events; once even that
        // is exhausted, loss is sticky and the terminal contract fails closed.
        if (pending_callbacks_.size() >= max_pending_callbacks ||
            (low_priority && pending_callbacks_.size() >= low_priority_callback_limit)) {
            record_dropped_observations();
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch_);
        event.elapsed_milliseconds =
            static_cast<std::uint64_t>(std::max(elapsed, std::chrono::milliseconds(0)).count());
        event.dropped_observations = dropped_observations_.load(std::memory_order_relaxed);
        event.telemetry_complete =
            event.telemetry_complete && event.dropped_observations == 0u &&
            progress_cache_accounting_valid(event.counters) &&
            progress_activity_accounting_valid(event.counters);
        try {
            const auto admission = next_callback_admission_;
            pending_callbacks_.push_back(PendingCallback{std::move(event), admission});
            last_callback_admission_ = admission;
            ++next_callback_admission_;
        } catch (...) {
            record_dropped_observations();
            return false;
        }
        return true;
    }

    ProgressCallback callback_;
    std::chrono::milliseconds minimum_update_interval_;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::steady_clock::time_point epoch_;
    std::atomic<std::uint64_t> next_scope_id_{1u};
    std::mutex state_mutex_;
    std::unordered_map<std::uint64_t, ActiveScope> active_scopes_;
    std::deque<PendingCallback> pending_callbacks_;
    std::condition_variable callback_fence_;
    std::condition_variable producer_fence_;
    bool sealed_ = false;
    bool seal_incomplete_ = false;
    std::uint64_t active_scope_count_ = 0u;
    std::uint64_t active_producers_ = 0u;
    std::uint64_t admitted_producers_ = 0u;
    std::uint64_t resolved_producers_ = 0u;
    std::uint64_t next_callback_admission_ = 1u;
    std::uint64_t last_callback_admission_ = 0u;
    std::uint64_t last_resolved_admission_ = 0u;
    std::uint64_t active_callback_admission_ = 0u;
    std::thread::id callback_thread_;
    std::uint64_t delivered_callbacks_ = 0u;
    std::uint64_t discarded_callbacks_ = 0u;
    std::uint64_t next_sequence_ = 1u;
    std::atomic<std::uint64_t> dropped_observations_{0u};
    std::shared_ptr<ProgressWorkerSignal> worker_signal_;
};

namespace {
inline constexpr std::size_t maximum_progress_worker_slots = 64u;
}

struct ProgressHeartbeatService::WorkerSlot final {
    explicit WorkerSlot(const std::shared_ptr<ProgressCore>& core)
        : core_(core), signal_(core->worker_signal()),
          worker_([this](const std::stop_token stop) { worker_loop(stop); }) {}

    ~WorkerSlot() {
        request_stop();
    }

    void request_stop() noexcept {
        worker_.request_stop();
        signal_->notify();
    }

    void worker_loop(const std::stop_token stop) noexcept {
        auto deadline = std::chrono::steady_clock::time_point::max();
        while (!stop.stop_requested()) {
            auto core = core_.lock();
            if (!core) break;
            const auto observed_revision =
                signal_->revision.load(std::memory_order_acquire);
            const auto callbacks_before_heartbeat =
                core->drain_callback_batch();
            const auto candidate =
                core->emit_due_heartbeats(std::chrono::steady_clock::now());
            deadline = candidate.value_or(
                std::chrono::steady_clock::time_point::max());
            const auto callbacks_after_heartbeat =
                core->drain_callback_batch();
            core.reset();

            if (stop.stop_requested()) break;
            if (callbacks_before_heartbeat || callbacks_after_heartbeat)
                continue;

            std::unique_lock lock(signal_->mutex);
            const auto changed = [&] {
                return stop.stop_requested() || core_.expired() ||
                       signal_->revision.load(std::memory_order_acquire) !=
                           observed_revision;
            };
            if (deadline == std::chrono::steady_clock::time_point::max())
                signal_->condition.wait(lock, changed);
            else
                signal_->condition.wait_until(lock, deadline, changed);
        }
        finished_.store(true, std::memory_order_release);
    }

    std::weak_ptr<ProgressCore> core_;
    std::shared_ptr<ProgressWorkerSignal> signal_;
    std::atomic_bool finished_{false};
    std::jthread worker_;
};

ProgressHeartbeatService::ProgressHeartbeatService() {
    workers_.reserve(maximum_progress_worker_slots);
}

ProgressHeartbeatService::~ProgressHeartbeatService() {
    std::vector<std::unique_ptr<WorkerSlot>> workers;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        for (const auto& worker : workers_)
            worker->request_stop();
        workers.swap(workers_);
    }
    workers.clear();
}

void ProgressHeartbeatService::reap_finished_locked() noexcept {
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(), [](const auto& worker) {
            return worker->finished_.load(std::memory_order_acquire) ||
                   worker->core_.expired();
        }),
        workers_.end());
}

bool ProgressHeartbeatService::register_core(
    const std::shared_ptr<ProgressCore>& core) noexcept {
    if (!core) return false;
    try {
        const std::lock_guard lock(mutex_);
        if (stopping_) return false;
        reap_finished_locked();
        if (workers_.size() >= maximum_progress_worker_slots)
            return false;
        workers_.push_back(std::make_unique<WorkerSlot>(core));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace detail

ProgressReporter::ProgressReporter(ProgressCallback callback,
                                   const std::chrono::milliseconds minimum_update_interval,
                                   const std::chrono::milliseconds heartbeat_interval) {
    if (callback) {
        core_ = std::make_shared<detail::ProgressCore>(
            std::move(callback), minimum_update_interval, heartbeat_interval);
        if (!detail::progress_heartbeat_service().register_core(core_)) {
            core_.reset();
            throw std::runtime_error(
                "Progress-Worker-Limit oder Worker-Start fehlgeschlagen.");
        }
    }
}

ProgressReporter::ProgressReporter(std::shared_ptr<detail::ProgressCore> core,
                                   const std::optional<std::uint64_t> parent_scope_id) noexcept
    : core_(std::move(core)), parent_scope_id_(parent_scope_id) {}

bool ProgressReporter::enabled() const noexcept {
    return static_cast<bool>(core_);
}

std::uint64_t ProgressReporter::dropped_observations() const noexcept {
    return core_ ? core_->dropped_observations() : 0u;
}

void ProgressReporter::record_observation_loss(
    const std::uint64_t amount) const noexcept {
    if (core_) core_->record_external_observation_loss(amount);
}

bool ProgressReporter::flush() const noexcept {
    return !core_ || core_->flush();
}

bool ProgressReporter::seal_and_flush() const noexcept {
    return !core_ || core_->seal_and_flush();
}

ProgressScope ProgressReporter::begin(const ProgressOperation operation,
                                      const ProgressUnit unit,
                                      const std::optional<std::uint64_t> total,
                                      std::string label) const {
    return ProgressScope(core_, operation, unit, total, std::move(label), parent_scope_id_);
}

ProgressScope::ProgressScope(std::shared_ptr<detail::ProgressCore> core,
                             const ProgressOperation operation,
                             const ProgressUnit unit,
                             const std::optional<std::uint64_t> total,
                             std::string label,
                             const std::optional<std::uint64_t> parent_scope_id)
    : core_(std::move(core)), operation_(operation), unit_(unit), total_(total),
      label_(std::move(label)), parent_scope_id_(parent_scope_id) {
    if (!core_) return;
    // Enter the producer epoch before any allocation owned by begin(). This
    // makes a concurrent seal wait for constructor success or failure instead
    // of returning a false complete result between allocation and admission.
    if (!core_->reserve_scope_start()) {
        terminal_ = true;
        return;
    }
    start_reservation_pending_ = true;
    scope_registered_ = true;
    try {
        scope_mutex_ = std::make_shared<std::recursive_mutex>();
        delivery_state_ = std::make_shared<detail::ProgressDeliveryState>();
        scope_id_ = core_->next_scope_id();
        scope_started_ = std::chrono::steady_clock::now();
        last_emission_ = scope_started_;
        const auto lock = lock_scope();
        auto emission =
            prepare_emission_locked(ProgressState::Started, true);
        if (emission) deliver(std::move(*emission));
        if (!scope_registered_) terminal_ = true;
    } catch (...) {
        if (start_reservation_pending_)
            core_->discard_reserved_scope_start();
        else
            core_->discard_registered_scope();
        start_reservation_pending_ = false;
        scope_registered_ = false;
        terminal_ = true;
        core_.reset();
        scope_mutex_.reset();
        delivery_state_.reset();
    }
}

ProgressScope::ProgressScope(ProgressScope&& other) noexcept
    : core_(std::move(other.core_)), scope_mutex_(std::move(other.scope_mutex_)),
      delivery_state_(std::move(other.delivery_state_)), operation_(other.operation_),
      unit_(other.unit_), total_(other.total_), label_(std::move(other.label_)),
      parent_scope_id_(other.parent_scope_id_), scope_id_(other.scope_id_),
      completed_(other.completed_), counters_(other.counters_),
      scope_started_(other.scope_started_), last_emission_(other.last_emission_),
      emission_revision_(other.emission_revision_),
      start_reservation_pending_(other.start_reservation_pending_),
      scope_registered_(other.scope_registered_), terminal_(other.terminal_) {
    other.start_reservation_pending_ = false;
    other.scope_registered_ = false;
    other.terminal_ = true;
}

ProgressScope& ProgressScope::operator=(ProgressScope&& other) noexcept {
    if (this == &other) return *this;
    abandon();
    core_ = std::move(other.core_);
    scope_mutex_ = std::move(other.scope_mutex_);
    delivery_state_ = std::move(other.delivery_state_);
    operation_ = other.operation_;
    unit_ = other.unit_;
    total_ = other.total_;
    label_ = std::move(other.label_);
    parent_scope_id_ = other.parent_scope_id_;
    scope_id_ = other.scope_id_;
    completed_ = other.completed_;
    counters_ = other.counters_;
    scope_started_ = other.scope_started_;
    last_emission_ = other.last_emission_;
    emission_revision_ = other.emission_revision_;
    start_reservation_pending_ = other.start_reservation_pending_;
    scope_registered_ = other.scope_registered_;
    terminal_ = other.terminal_;
    other.start_reservation_pending_ = false;
    other.scope_registered_ = false;
    other.terminal_ = true;
    return *this;
}

ProgressScope::~ProgressScope() {
    abandon();
}

bool ProgressScope::enabled() const noexcept {
    return static_cast<bool>(core_);
}

std::uint64_t ProgressScope::completed() const {
    const auto lock = lock_scope();
    return completed_;
}

std::optional<std::uint64_t> ProgressScope::total() const {
    const auto lock = lock_scope();
    return total_;
}

ProgressReporter ProgressScope::child_reporter() const {
    const auto lock = lock_scope();
    return ProgressReporter(core_, core_ ? std::optional(scope_id_) : std::nullopt);
}

void ProgressScope::update(const std::uint64_t completed) {
    const auto lock = lock_scope();
    if (!core_ || terminal_) return;
    std::optional<PreparedEmission> emission;
    if (completed < completed_ || (total_ && completed > *total_)) {
        terminal_ = true;
        emission = prepare_emission_locked(ProgressState::Failed, true);
    } else {
        completed_ = completed;
        emission = prepare_emission_locked(ProgressState::Running, false);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::update(const std::uint64_t completed, ProgressCounterSnapshot counters) {
    const auto lock = lock_scope();
    if (!core_ || terminal_) return;
    counters_ = std::move(counters);
    std::optional<PreparedEmission> emission;
    if (completed < completed_ || (total_ && completed > *total_)) {
        terminal_ = true;
        emission = prepare_emission_locked(ProgressState::Failed, true);
    } else {
        completed_ = completed;
        emission = prepare_emission_locked(ProgressState::Running, false);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::update(ProgressCounterSnapshot counters) {
    const auto lock = lock_scope();
    if (!core_ || terminal_) return;
    counters_ = std::move(counters);
    auto emission = prepare_emission_locked(ProgressState::Running, false);
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::advance(const std::uint64_t amount) {
    const auto lock = lock_scope();
    if (!core_ || terminal_) return;
    std::optional<PreparedEmission> emission;
    if (amount > std::numeric_limits<std::uint64_t>::max() - completed_) {
        terminal_ = true;
        emission = prepare_emission_locked(ProgressState::Failed, true);
    } else {
        completed_ += amount;
        if (total_ && completed_ > *total_) {
            completed_ -= amount;
            terminal_ = true;
            emission = prepare_emission_locked(ProgressState::Failed, true);
        } else {
            emission = prepare_emission_locked(ProgressState::Running, false);
        }
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::heartbeat(ProgressCounterSnapshot counters) {
    const auto lock = lock_scope();
    if (!core_ || terminal_) return;
    counters_ = std::move(counters);
    auto emission = prepare_emission_locked(ProgressState::Heartbeat, false);
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::complete() {
    const auto lock = lock_scope();
    if (terminal_) return;
    if (!core_) {
        terminal_ = true;
        return;
    }
    completed_ = total_.value_or(completed_);
    terminal_ = true;
    auto emission = prepare_emission_locked(ProgressState::Completed, true);
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::complete(const std::uint64_t completed) {
    const auto lock = lock_scope();
    if (terminal_) return;
    if (!core_) {
        terminal_ = true;
        return;
    }
    std::optional<PreparedEmission> emission;
    if (completed < completed_ || (total_ && completed != *total_)) {
        terminal_ = true;
        emission = prepare_emission_locked(ProgressState::Failed, true);
    } else {
        completed_ = completed;
        terminal_ = true;
        emission = prepare_emission_locked(ProgressState::Completed, true);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::cached() {
    const auto lock = lock_scope();
    if (terminal_) return;
    if (!core_) {
        terminal_ = true;
        return;
    }
    completed_ = total_.value_or(completed_);
    terminal_ = true;
    auto emission = prepare_emission_locked(ProgressState::Cached, true);
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::skipped() {
    const auto lock = lock_scope();
    if (terminal_) return;
    if (!core_) {
        terminal_ = true;
        return;
    }
    terminal_ = true;
    auto emission = prepare_emission_locked(ProgressState::Skipped, true);
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::fail() noexcept {
    try {
        const auto lock = lock_scope();
        if (terminal_) return;
        terminal_ = true;
        auto emission = prepare_emission_locked(ProgressState::Failed, true);
        if (emission) deliver(std::move(*emission));
    } catch (...) {
        // Destruction and reporting remain non-throwing if synchronization itself fails.
    }
}

std::optional<ProgressScope::PreparedEmission>
ProgressScope::prepare_emission_locked(const ProgressState state, const bool force) noexcept {
    if (delivery_state_ && (state == ProgressState::Completed || state == ProgressState::Cached ||
                            state == ProgressState::Skipped || state == ProgressState::Failed))
        delivery_state_->terminal.store(true, std::memory_order_release);
    try {
        if (!core_ || !delivery_state_) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emission_ < core_->minimum_update_interval()) return std::nullopt;
        const auto terminal_event =
            state == ProgressState::Completed || state == ProgressState::Cached ||
            state == ProgressState::Skipped || state == ProgressState::Failed;
        bool reserved = false;
        if (state == ProgressState::Started) {
            if (start_reservation_pending_) {
                reserved = true;
                start_reservation_pending_ = false;
            } else {
                reserved = core_->reserve_scope_start();
            }
            scope_registered_ = reserved;
        } else if (terminal_event) {
            reserved = core_->reserve_scope_terminal(scope_registered_);
            if (reserved) scope_registered_ = false;
        } else {
            reserved = core_->reserve_scope_update();
        }
        if (!reserved) return std::nullopt;
        last_emission_ = now;
        ++emission_revision_;
        const auto scope_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - scope_started_);
        ProgressEvent event;
        event.operation = operation_;
        event.state = state;
        event.unit = unit_;
        event.scope_id = scope_id_;
        event.parent_scope_id = parent_scope_id_;
        event.completed = completed_;
        event.total = total_;
        event.counters = counters_;
        event.label = label_;
        event.scope_elapsed_milliseconds = static_cast<std::uint64_t>(
            std::max(scope_elapsed, std::chrono::milliseconds(0)).count());
        return PreparedEmission{core_, delivery_state_, std::move(event), emission_revision_};
    } catch (...) {
        // The producer reservation is allocation-free and therefore survives
        // a Started/Terminal event construction failure. Resolve it as sticky
        // loss so seal_and_flush can never report invented completeness.
        if (core_) core_->discard_reserved_emission();
        return std::nullopt;
    }
}

void ProgressScope::deliver(PreparedEmission emission) noexcept {
    if (!emission.core) return;
    emission.core->emit_reserved(
        std::move(emission.event), emission.scope_revision, emission.delivery_state);
}

void ProgressScope::abandon() noexcept {
    fail();
}

std::unique_lock<std::recursive_mutex> ProgressScope::lock_scope() const {
    if (!scope_mutex_) return {};
    return std::unique_lock(*scope_mutex_);
}

std::string_view progress_operation_name(const ProgressOperation operation) noexcept {
    switch (operation) {
    case ProgressOperation::InputProvenance:
        return "input-provenance";
    case ProgressOperation::GdiOpen:
        return "gdi-open";
    case ProgressOperation::GdiTrackHash:
        return "gdi-track-hash";
    case ProgressOperation::PackedDiscOpen:
        return "packed-disc-open";
    case ProgressOperation::PackedDiscContentIdentity:
        return "packed-disc-content-identity";
    case ProgressOperation::PackedDiscWrite:
        return "packed-disc-write";
    case ProgressOperation::PackedDiscVerify:
        return "packed-disc-verify";
    case ProgressOperation::DiscInstall:
        return "disc-install";
    case ProgressOperation::DiscInstallSourceVerify:
        return "disc-install-source-verify";
    case ProgressOperation::DiscLoad:
        return "disc-load";
    case ProgressOperation::BootImage:
        return "boot-image";
    case ProgressOperation::ProgramValidation:
        return "program-validation";
    case ProgressOperation::ControlFlowAnalysis:
        return "control-flow-analysis";
    case ProgressOperation::FunctionValueAnalysis:
        return "function-value-analysis";
    case ProgressOperation::CandidateResolution:
        return "candidate-resolution";
    case ProgressOperation::LatentAotAnalysis:
        return "latent-aot-analysis";
    case ProgressOperation::IrGeneration:
        return "ir-generation";
    case ProgressOperation::IrOptimization:
        return "ir-optimization";
    case ProgressOperation::SourceGeneration:
        return "source-generation";
    case ProgressOperation::MetadataGeneration:
        return "metadata-generation";
    case ProgressOperation::ArtifactWrite:
        return "artifact-write";
    case ProgressOperation::Configure:
        return "configure";
    case ProgressOperation::HostRuntimeBuild:
        return "host-runtime-build";
    case ProgressOperation::Compilation:
        return "compilation";
    case ProgressOperation::Linking:
        return "linking";
    case ProgressOperation::Packaging:
        return "packaging";
    case ProgressOperation::RuntimeStartup:
        return "runtime-startup";
    case ProgressOperation::PortBuild:
        return "port-build";
    case ProgressOperation::ControlFlowRound:
        return "control-flow-round";
    case ProgressOperation::CandidateContractIteration:
        return "candidate-contract-iteration";
    }
    return "unknown";
}

std::string_view progress_state_name(const ProgressState state) noexcept {
    switch (state) {
    case ProgressState::Started:
        return "started";
    case ProgressState::Running:
        return "running";
    case ProgressState::Heartbeat:
        return "heartbeat";
    case ProgressState::Completed:
        return "completed";
    case ProgressState::Cached:
        return "cached";
    case ProgressState::Skipped:
        return "skipped";
    case ProgressState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view progress_unit_name(const ProgressUnit unit) noexcept {
    switch (unit) {
    case ProgressUnit::None:
        return "none";
    case ProgressUnit::Bytes:
        return "bytes";
    case ProgressUnit::Tracks:
        return "tracks";
    case ProgressUnit::Sectors:
        return "sectors";
    case ProgressUnit::Chunks:
        return "chunks";
    case ProgressUnit::Files:
        return "files";
    case ProgressUnit::Functions:
        return "functions";
    case ProgressUnit::Modules:
        return "modules";
    case ProgressUnit::Partitions:
        return "partitions";
    case ProgressUnit::TranslationUnits:
        return "translation-units";
    case ProgressUnit::Steps:
        return "steps";
    }
    return "unknown";
}

namespace {

using OptionalCounterMember = std::optional<std::uint64_t> ProgressCounterSnapshot::*;

struct NamedCounterMember final {
    std::string_view name;
    OptionalCounterMember member;
};

constexpr std::array<NamedCounterMember, 61u> progress_counter_members{{
    {"iteration", &ProgressCounterSnapshot::iteration},
    {"pass", &ProgressCounterSnapshot::pass},
    {"active_workers", &ProgressCounterSnapshot::active_workers},
    {"evaluation_requests", &ProgressCounterSnapshot::evaluation_requests},
    {"active_evaluation_requests",
     &ProgressCounterSnapshot::active_evaluation_requests},
    {"evaluation_request_nanoseconds",
     &ProgressCounterSnapshot::evaluation_request_nanoseconds},
    {"maximum_evaluation_request_nanoseconds",
     &ProgressCounterSnapshot::maximum_evaluation_request_nanoseconds},
    {"cache_key_builds", &ProgressCounterSnapshot::cache_key_builds},
    {"active_cache_key_builds",
     &ProgressCounterSnapshot::active_cache_key_builds},
    {"cache_key_build_nanoseconds",
     &ProgressCounterSnapshot::cache_key_build_nanoseconds},
    {"maximum_cache_key_build_nanoseconds",
     &ProgressCounterSnapshot::maximum_cache_key_build_nanoseconds},
    {"cache_waits", &ProgressCounterSnapshot::cache_waits},
    {"active_cache_waits", &ProgressCounterSnapshot::active_cache_waits},
    {"cache_wait_nanoseconds",
     &ProgressCounterSnapshot::cache_wait_nanoseconds},
    {"maximum_cache_wait_nanoseconds",
     &ProgressCounterSnapshot::maximum_cache_wait_nanoseconds},
    {"cache_replays", &ProgressCounterSnapshot::cache_replays},
    {"active_cache_replays",
     &ProgressCounterSnapshot::active_cache_replays},
    {"cache_replay_nanoseconds",
     &ProgressCounterSnapshot::cache_replay_nanoseconds},
    {"maximum_cache_replay_nanoseconds",
     &ProgressCounterSnapshot::maximum_cache_replay_nanoseconds},
    {"physical_evaluations",
     &ProgressCounterSnapshot::physical_evaluations},
    {"active_physical_evaluations",
     &ProgressCounterSnapshot::active_physical_evaluations},
    {"physical_evaluation_nanoseconds",
     &ProgressCounterSnapshot::physical_evaluation_nanoseconds},
    {"maximum_physical_evaluation_nanoseconds",
     &ProgressCounterSnapshot::maximum_physical_evaluation_nanoseconds},
    {"cache_commits", &ProgressCounterSnapshot::cache_commits},
    {"active_cache_commits",
     &ProgressCounterSnapshot::active_cache_commits},
    {"cache_commit_nanoseconds",
     &ProgressCounterSnapshot::cache_commit_nanoseconds},
    {"maximum_cache_commit_nanoseconds",
     &ProgressCounterSnapshot::maximum_cache_commit_nanoseconds},
    {"queued_work", &ProgressCounterSnapshot::queued_work},
    {"discovered", &ProgressCounterSnapshot::discovered},
    {"started", &ProgressCounterSnapshot::started},
    {"requeued", &ProgressCounterSnapshot::requeued},
    {"cache_hits", &ProgressCounterSnapshot::cache_hits},
    {"cache_misses", &ProgressCounterSnapshot::cache_misses},
    {"planned_work", &ProgressCounterSnapshot::planned_work},
    {"ready_work", &ProgressCounterSnapshot::ready_work},
    {"committed_work", &ProgressCounterSnapshot::committed_work},
    {"configured_workers", &ProgressCounterSnapshot::configured_workers},
    {"added_work", &ProgressCounterSnapshot::added_work},
    {"head_of_line_index", &ProgressCounterSnapshot::head_of_line_index},
    {"head_of_line_elapsed_milliseconds",
     &ProgressCounterSnapshot::head_of_line_elapsed_milliseconds},
    {"ready_ahead", &ProgressCounterSnapshot::ready_ahead},
    {"cache_lookups", &ProgressCounterSnapshot::cache_lookups},
    {"cache_ready_hits", &ProgressCounterSnapshot::cache_ready_hits},
    {"cache_in_flight_coalesces", &ProgressCounterSnapshot::cache_in_flight_coalesces},
    {"cache_replay_fallback_recomputes",
     &ProgressCounterSnapshot::cache_replay_fallback_recomputes},
    {"cache_diagnostic_bypass_evaluations",
     &ProgressCounterSnapshot::cache_diagnostic_bypass_evaluations},
    {"cache_evictions", &ProgressCounterSnapshot::cache_evictions},
    {"cache_entries", &ProgressCounterSnapshot::cache_entries},
    {"cache_retained_payload_bytes",
     &ProgressCounterSnapshot::cache_retained_payload_bytes},
    {"cache_miss_cold", &ProgressCounterSnapshot::cache_miss_cold},
    {"cache_miss_evicted", &ProgressCounterSnapshot::cache_miss_evicted},
    {"cache_miss_oversize_or_no_exact_replay",
     &ProgressCounterSnapshot::cache_miss_oversize_or_no_exact_replay},
    {"cache_miss_function_shape_changed",
     &ProgressCounterSnapshot::cache_miss_function_shape_changed},
    {"cache_miss_projected_ingress_changed",
     &ProgressCounterSnapshot::cache_miss_projected_ingress_changed},
    {"cache_miss_summary_dependency_changed",
     &ProgressCounterSnapshot::cache_miss_summary_dependency_changed},
    {"cache_miss_abi_contract_changed", &ProgressCounterSnapshot::cache_miss_abi_contract_changed},
    {"cache_miss_resolution_lens_changed",
     &ProgressCounterSnapshot::cache_miss_resolution_lens_changed},
    {"cache_miss_inventory_sink_changed",
     &ProgressCounterSnapshot::cache_miss_inventory_sink_changed},
    {"cache_miss_isolation_partition_changed",
     &ProgressCounterSnapshot::cache_miss_isolation_partition_changed},
    {"cache_miss_contextual_summary_changed",
     &ProgressCounterSnapshot::cache_miss_contextual_summary_changed},
    {"cache_miss_tail_ingress_changed", &ProgressCounterSnapshot::cache_miss_tail_ingress_changed},
}};

constexpr std::array<OptionalCounterMember, 12u> cache_miss_reason_members{{
    &ProgressCounterSnapshot::cache_miss_cold,
    &ProgressCounterSnapshot::cache_miss_evicted,
    &ProgressCounterSnapshot::cache_miss_oversize_or_no_exact_replay,
    &ProgressCounterSnapshot::cache_miss_function_shape_changed,
    &ProgressCounterSnapshot::cache_miss_projected_ingress_changed,
    &ProgressCounterSnapshot::cache_miss_summary_dependency_changed,
    &ProgressCounterSnapshot::cache_miss_abi_contract_changed,
    &ProgressCounterSnapshot::cache_miss_resolution_lens_changed,
    &ProgressCounterSnapshot::cache_miss_inventory_sink_changed,
    &ProgressCounterSnapshot::cache_miss_isolation_partition_changed,
    &ProgressCounterSnapshot::cache_miss_contextual_summary_changed,
    &ProgressCounterSnapshot::cache_miss_tail_ingress_changed,
}};

struct ActivityCounterMembers final {
    OptionalCounterMember count;
    OptionalCounterMember active;
    OptionalCounterMember cumulative_nanoseconds;
    OptionalCounterMember maximum_nanoseconds;
};

constexpr std::array<ActivityCounterMembers, 6u>
    activity_counter_members{{
        {&ProgressCounterSnapshot::evaluation_requests,
         &ProgressCounterSnapshot::active_evaluation_requests,
         &ProgressCounterSnapshot::evaluation_request_nanoseconds,
         &ProgressCounterSnapshot::maximum_evaluation_request_nanoseconds},
        {&ProgressCounterSnapshot::cache_key_builds,
         &ProgressCounterSnapshot::active_cache_key_builds,
         &ProgressCounterSnapshot::cache_key_build_nanoseconds,
         &ProgressCounterSnapshot::maximum_cache_key_build_nanoseconds},
        {&ProgressCounterSnapshot::cache_waits,
         &ProgressCounterSnapshot::active_cache_waits,
         &ProgressCounterSnapshot::cache_wait_nanoseconds,
         &ProgressCounterSnapshot::maximum_cache_wait_nanoseconds},
        {&ProgressCounterSnapshot::cache_replays,
         &ProgressCounterSnapshot::active_cache_replays,
         &ProgressCounterSnapshot::cache_replay_nanoseconds,
         &ProgressCounterSnapshot::maximum_cache_replay_nanoseconds},
        {&ProgressCounterSnapshot::physical_evaluations,
         &ProgressCounterSnapshot::active_physical_evaluations,
         &ProgressCounterSnapshot::physical_evaluation_nanoseconds,
         &ProgressCounterSnapshot::maximum_physical_evaluation_nanoseconds},
        {&ProgressCounterSnapshot::cache_commits,
         &ProgressCounterSnapshot::active_cache_commits,
         &ProgressCounterSnapshot::cache_commit_nanoseconds,
         &ProgressCounterSnapshot::maximum_cache_commit_nanoseconds},
    }};

[[nodiscard]] bool checked_add(const std::uint64_t value, std::uint64_t& sum) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - sum) return false;
    sum += value;
    return true;
}

void append_unsigned(std::string& destination, const std::uint64_t value) {
    std::array<char, 32u> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) destination.append(buffer.data(), end);
}

void append_json_string(std::string& destination, const std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    destination.push_back('"');
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            destination += "\\\"";
            break;
        case '\\':
            destination += "\\\\";
            break;
        case '\b':
            destination += "\\b";
            break;
        case '\f':
            destination += "\\f";
            break;
        case '\n':
            destination += "\\n";
            break;
        case '\r':
            destination += "\\r";
            break;
        case '\t':
            destination += "\\t";
            break;
        default:
            if (byte < 0x20u) {
                destination += "\\u00";
                destination.push_back(hex[byte >> 4u]);
                destination.push_back(hex[byte & 0x0fu]);
            } else {
                destination.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    destination.push_back('"');
}

void append_json_optional_unsigned(std::string& destination,
                                   const std::optional<std::uint64_t> value) {
    if (value)
        append_unsigned(destination, *value);
    else
        destination += "null";
}

[[nodiscard]] std::uint64_t progress_percent_milli(const ProgressEvent& event) noexcept {
    if (!event.total) return 0u;
    if (*event.total == 0u) {
        return event.state == ProgressState::Completed || event.state == ProgressState::Cached
                   ? 100'000u
                   : 0u;
    }
    const auto percent = static_cast<long double>(event.completed) * 100'000.0L /
                         static_cast<long double>(*event.total);
    return static_cast<std::uint64_t>(std::min<long double>(100'000.0L, percent));
}

} // namespace

bool progress_cache_accounting_valid(const ProgressCounterSnapshot& counters) noexcept {
    const auto split_accounting_present = counters.cache_lookups.has_value() ||
                                          counters.cache_ready_hits.has_value() ||
                                          counters.cache_in_flight_coalesces.has_value();
    if (split_accounting_present) {
        if (!counters.cache_lookups || !counters.cache_ready_hits ||
            !counters.cache_in_flight_coalesces || !counters.cache_misses)
            return false;
        std::uint64_t accounted = 0u;
        if (!checked_add(*counters.cache_ready_hits, accounted) ||
            !checked_add(*counters.cache_in_flight_coalesces, accounted) ||
            !checked_add(*counters.cache_misses, accounted) || accounted != *counters.cache_lookups)
            return false;
        std::uint64_t split_hits = 0u;
        if (!checked_add(*counters.cache_ready_hits, split_hits) ||
            !checked_add(*counters.cache_in_flight_coalesces, split_hits))
            return false;
        if (counters.cache_hits &&
            (*counters.cache_hits != split_hits ||
             split_hits > *counters.cache_lookups ||
             *counters.cache_misses !=
                 *counters.cache_lookups - split_hits))
            return false;
    }

    bool miss_reasons_present = false;
    std::uint64_t explained_misses = 0u;
    for (const auto member : cache_miss_reason_members) {
        const auto& reason = counters.*member;
        if (!reason) continue;
        miss_reasons_present = true;
        if (!checked_add(*reason, explained_misses)) return false;
    }
    if (miss_reasons_present) {
        if (!counters.cache_misses || explained_misses != *counters.cache_misses) return false;
    } else if (split_accounting_present && counters.cache_misses && *counters.cache_misses != 0u) {
        return false;
    }
    return true;
}

bool progress_activity_accounting_valid(
    const ProgressCounterSnapshot& counters) noexcept {
    for (const auto& members : activity_counter_members) {
        const auto& count = counters.*members.count;
        const auto& active = counters.*members.active;
        const auto& cumulative = counters.*members.cumulative_nanoseconds;
        const auto& maximum = counters.*members.maximum_nanoseconds;
        const auto any = count.has_value() || active.has_value() ||
                         cumulative.has_value() || maximum.has_value();
        if (!any) continue;
        if (!count || !active || !cumulative || !maximum) return false;
        if (*active > *count || *maximum > *cumulative) return false;
        if (*count == 0u) {
            if (*active != 0u || *cumulative != 0u || *maximum != 0u)
                return false;
            continue;
        }
        // Counts and durations are sampled from lock-free producer atomics.
        // A just-started scope may already be counted before its eventual
        // duration is published, so live snapshots cannot derive a minimum
        // cumulative duration from count-active. The stable invariants remain
        // active<=count and maximum<=cumulative; terminal producer tests prove
        // nonzero time for every exercised domain.
    }
    return true;
}

bool progress_event_telemetry_complete(const ProgressEvent& event) noexcept {
    return event.telemetry_complete && event.dropped_observations == 0u &&
           progress_cache_accounting_valid(event.counters) &&
           progress_activity_accounting_valid(event.counters);
}

std::string format_progress_event_json(const ProgressEvent& event) {
    std::string result;
    result.reserve(1536u);
    result += "{\"schema\":";
    append_json_string(result, progress_event_schema);
    result += ",\"schema_version\":";
    append_unsigned(result, progress_event_schema_version);
    result += ",\"operation\":";
    append_json_string(result, progress_operation_name(event.operation));
    result += ",\"state\":";
    append_json_string(result, progress_state_name(event.state));
    result += ",\"unit\":";
    append_json_string(result, progress_unit_name(event.unit));
    result += ",\"sequence\":";
    append_unsigned(result, event.sequence);
    result += ",\"elapsed_ms\":";
    append_unsigned(result, event.elapsed_milliseconds);
    result += ",\"scope_elapsed_ms\":";
    append_unsigned(result, event.scope_elapsed_milliseconds);
    result += ",\"scope_id\":";
    append_unsigned(result, event.scope_id);
    result += ",\"parent_scope_id\":";
    append_json_optional_unsigned(result, event.parent_scope_id);
    result += ",\"completed\":";
    append_unsigned(result, event.completed);
    result += ",\"total\":";
    append_json_optional_unsigned(result, event.total);
    result += ",\"counters\":{";
    bool first_counter = true;
    for (std::size_t index = 0u; index < progress_counter_members.size(); ++index) {
        const auto& field = progress_counter_members[index];
        if (field.member ==
                &ProgressCounterSnapshot::head_of_line_index &&
            event.counters.growing_workset) {
            if (!first_counter) result.push_back(',');
            append_json_string(result, "growing_workset");
            result += ':';
            result += *event.counters.growing_workset ? "true" : "false";
            first_counter = false;
        }
        const auto& value = event.counters.*(field.member);
        if (!value) continue;
        if (!first_counter) result.push_back(',');
        append_json_string(result, field.name);
        result.push_back(':');
        append_unsigned(result, *value);
        first_counter = false;
    }
    result += "},\"dropped_observations\":";
    append_unsigned(result, event.dropped_observations);
    result += ",\"telemetry_complete\":";
    result += progress_event_telemetry_complete(event) ? "true" : "false";
    result += ",\"label\":";
    append_json_string(result, event.label);
    result.push_back('}');
    return result;
}

std::string format_progress_event_human(const ProgressEvent& event) {
    std::string result = "KATANA_PROGRESS operation=";
    result += progress_operation_name(event.operation);
    result += " state=";
    result += progress_state_name(event.state);
    result += " schema=";
    result += progress_event_schema;
    result += " schema_version=";
    append_unsigned(result, progress_event_schema_version);
    result += " elapsed_ms=";
    append_unsigned(result, event.elapsed_milliseconds);
    result += " scope_elapsed_ms=";
    append_unsigned(result, event.scope_elapsed_milliseconds);
    result += " scope=";
    append_unsigned(result, event.scope_id);
    if (event.parent_scope_id) {
        result += " parent=";
        append_unsigned(result, *event.parent_scope_id);
    }
    result += " unit=";
    result += progress_unit_name(event.unit);
    result += " completed=";
    append_unsigned(result, event.completed);
    if (event.total) {
        result += " total=";
        append_unsigned(result, *event.total);
        result += " percent_milli=";
        append_unsigned(result, progress_percent_milli(event));
    }
    for (std::size_t index = 0u; index < progress_counter_members.size(); ++index) {
        const auto& field = progress_counter_members[index];
        if (field.member ==
                &ProgressCounterSnapshot::head_of_line_index &&
            event.counters.growing_workset) {
            result += " growing_workset=";
            result += *event.counters.growing_workset ? "true" : "false";
        }
        const auto& value = event.counters.*(field.member);
        if (!value) continue;
        result.push_back(' ');
        result += field.name;
        result.push_back('=');
        append_unsigned(result, *value);
    }
    result += " dropped_observations=";
    append_unsigned(result, event.dropped_observations);
    result += " telemetry_complete=";
    result += progress_event_telemetry_complete(event) ? "true" : "false";
    if (!event.label.empty()) {
        result += " label=";
        append_json_string(result, event.label);
    }
    return result;
}

} // namespace katana
