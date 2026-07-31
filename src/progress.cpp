#include "katana/progress.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana {
namespace detail {

class ProgressCore;

class ProgressHeartbeatService final {
  public:
    ProgressHeartbeatService();
    ~ProgressHeartbeatService();

    void register_core(const std::shared_ptr<ProgressCore>& core);
    void notify() noexcept;

  private:
    void heartbeat_loop(std::stop_token stop) noexcept;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::weak_ptr<ProgressCore>> cores_;
    bool changed_ = false;
    std::jthread worker_;
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
          epoch_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] std::uint64_t next_scope_id() noexcept {
        return next_scope_id_.fetch_add(1u, std::memory_order_relaxed);
    }

    [[nodiscard]] std::chrono::milliseconds minimum_update_interval() const noexcept {
        return minimum_update_interval_;
    }

    void emit(ProgressEvent event,
              const std::uint64_t scope_revision,
              const std::shared_ptr<ProgressDeliveryState>& delivery_state) noexcept {
        bool notify_heartbeat_service = false;
        bool drain_callbacks = false;
        try {
            std::unique_lock lock(state_mutex_);
            if (!delivery_state ||
                scope_revision <= delivery_state->latest_revision)
                return;
            if (!terminal(event.state) &&
                delivery_state->terminal.load(std::memory_order_acquire))
                return;
            delivery_state->latest_revision = scope_revision;
            const auto now = std::chrono::steady_clock::now();
            if (terminal(event.state)) {
                active_scopes_.erase(event.scope_id);
            } else {
                try {
                    active_scopes_.insert_or_assign(
                        event.scope_id,
                        ActiveScope{event, now, delivery_state});
                } catch (...) {
                    // Foreground progress remains valid if heartbeat bookkeeping cannot grow.
                }
            }
            drain_callbacks = enqueue_callback_locked(
                std::move(event), now, lock);
            notify_heartbeat_service = true;
        } catch (...) {
            // Progress reporting is observational. A broken UI/log sink must not corrupt or
            // cancel the operation it observes; cancellation has an explicit checkpoint path.
        }
        if (drain_callbacks)
            drain_callback_queue();
        if (notify_heartbeat_service)
            progress_heartbeat_service().notify();
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    emit_due_heartbeats(const std::chrono::steady_clock::time_point now) noexcept {
        bool drain_callbacks = false;
        std::optional<std::chrono::steady_clock::time_point> result;
        try {
            std::unique_lock lock(state_mutex_);
            if (active_scopes_.empty()) return std::nullopt;
            auto next_deadline = std::chrono::steady_clock::time_point::max();
            for (auto active = active_scopes_.begin();
                 active != active_scopes_.end();) {
                const auto delivery_state = active->second.delivery_state.lock();
                if (!delivery_state ||
                    delivery_state->terminal.load(std::memory_order_acquire)) {
                    active = active_scopes_.erase(active);
                    continue;
                }
                const auto deadline =
                    active->second.last_emission + heartbeat_interval_;
                if (now >= deadline) {
                    active->second.last_emission = now;
                    auto event = active->second.event;
                    event.state = ProgressState::Heartbeat;
                    drain_callbacks =
                        enqueue_callback_locked(
                            std::move(event), now, lock) ||
                        drain_callbacks;
                    next_deadline =
                        std::min(next_deadline, now + heartbeat_interval_);
                } else {
                    next_deadline = std::min(next_deadline, deadline);
                }
                ++active;
            }
            if (!active_scopes_.empty())
                result = next_deadline;
        } catch (...) {
            // A failed heartbeat allocation must not affect the observed work.
            result = now + std::min(heartbeat_interval_,
                                    std::chrono::milliseconds(100));
        }
        if (drain_callbacks)
            drain_callback_queue();
        return result;
    }

  private:
    struct ActiveScope final {
        ProgressEvent event;
        std::chrono::steady_clock::time_point last_emission;
        std::weak_ptr<ProgressDeliveryState> delivery_state;
    };

    [[nodiscard]] static bool terminal(const ProgressState state) noexcept {
        return state == ProgressState::Completed || state == ProgressState::Cached ||
               state == ProgressState::Skipped || state == ProgressState::Failed;
    }

    [[nodiscard]] bool enqueue_callback_locked(
        ProgressEvent event,
        const std::chrono::steady_clock::time_point now,
        std::unique_lock<std::mutex>& state_lock) noexcept {
        constexpr std::size_t max_pending_callbacks = 4096u;
        constexpr std::size_t low_priority_callback_limit = 3072u;
        const auto terminal_event = terminal(event.state);
        const auto low_priority =
            event.state == ProgressState::Running ||
            event.state == ProgressState::Heartbeat;
        // A terminal supersedes every still-pending observation for its
        // scope. Moving it to the tail preserves callback order and
        // guarantees a slot even when a slow observer has saturated the
        // queue with Started/Running/Heartbeat events.
        if (terminal_event) {
            std::erase_if(
                pending_callbacks_,
                [&](const ProgressEvent& pending) {
                    return pending.scope_id == event.scope_id;
                });
            if (pending_callbacks_.size() >= max_pending_callbacks) {
                const auto expendable = std::find_if(
                    pending_callbacks_.begin(),
                    pending_callbacks_.end(),
                    [&](const ProgressEvent& pending) {
                        return !terminal(pending.state);
                    });
                if (expendable != pending_callbacks_.end())
                    pending_callbacks_.erase(expendable);
            }
            // Once only terminal lifecycle events remain, dropping one would
            // strand an externally visible scope forever. Apply bounded
            // backpressure until the serialized callback drains a slot.
            // Reentrant emissions from the draining callback itself cannot
            // wait on that callback; they may temporarily exceed the bound
            // and are drained immediately after the current callback returns.
            if (pending_callbacks_.size() >= max_pending_callbacks &&
                callback_draining_ &&
                callback_draining_thread_ !=
                    std::this_thread::get_id()) {
                callback_space_.wait(
                    state_lock,
                    [&] {
                        return pending_callbacks_.size() <
                                   max_pending_callbacks ||
                               !callback_draining_;
                    });
            }
        } else if (low_priority) {
            // Keep only the newest sampled state for a scope. Started and
            // terminal lifecycle events remain distinct.
            const auto stale = std::find_if(
                pending_callbacks_.begin(),
                pending_callbacks_.end(),
                [&](const ProgressEvent& pending) {
                    return pending.scope_id == event.scope_id &&
                           (pending.state == ProgressState::Running ||
                            pending.state == ProgressState::Heartbeat);
                });
            if (stale != pending_callbacks_.end())
                pending_callbacks_.erase(stale);
        }
        // A blocked or intentionally slow observer must not turn progress
        // reporting into an unbounded retention path. The last quarter is
        // reserved for lifecycle events; terminal events additionally evict
        // pending nonterminal observations above.
        if ((!terminal_event &&
             pending_callbacks_.size() >= max_pending_callbacks) ||
            (low_priority &&
             pending_callbacks_.size() >= low_priority_callback_limit))
            return false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch_);
        event.elapsed_milliseconds =
            static_cast<std::uint64_t>(std::max(elapsed, std::chrono::milliseconds(0)).count());
        try {
            pending_callbacks_.push_back(std::move(event));
        } catch (...) {
            return false;
        }
        if (callback_draining_) return false;
        callback_draining_ = true;
        callback_draining_thread_ = std::this_thread::get_id();
        return true;
    }

    void drain_callback_queue() noexcept {
        while (true) {
            std::optional<ProgressEvent> event;
            {
                const std::lock_guard lock(state_mutex_);
                if (pending_callbacks_.empty()) {
                    callback_draining_ = false;
                    callback_draining_thread_ = {};
                    callback_space_.notify_all();
                    return;
                }
                event.emplace(std::move(pending_callbacks_.front()));
                pending_callbacks_.pop_front();
                event->sequence = next_sequence_++;
                callback_space_.notify_one();
            }
            try {
                callback_(*event);
            } catch (...) {
                // A progress consumer is never part of the observed operation's correctness
                // path. The queue keeps draining so a bad callback cannot strand later events.
            }
        }
    }

    ProgressCallback callback_;
    std::chrono::milliseconds minimum_update_interval_;
    std::chrono::milliseconds heartbeat_interval_;
    std::chrono::steady_clock::time_point epoch_;
    std::atomic<std::uint64_t> next_scope_id_{1u};
    std::mutex state_mutex_;
    std::unordered_map<std::uint64_t, ActiveScope> active_scopes_;
    std::deque<ProgressEvent> pending_callbacks_;
    std::condition_variable callback_space_;
    bool callback_draining_ = false;
    std::thread::id callback_draining_thread_;
    std::uint64_t next_sequence_ = 1u;
};

ProgressHeartbeatService::ProgressHeartbeatService()
    : worker_([this](const std::stop_token stop) {
          heartbeat_loop(stop);
      }) {}

ProgressHeartbeatService::~ProgressHeartbeatService() {
    worker_.request_stop();
    condition_.notify_all();
}

void ProgressHeartbeatService::register_core(
    const std::shared_ptr<ProgressCore>& core) {
    if (!core) return;
    {
        const std::lock_guard lock(mutex_);
        cores_.push_back(core);
        changed_ = true;
    }
    condition_.notify_all();
}

void ProgressHeartbeatService::notify() noexcept {
    try {
        {
            const std::lock_guard lock(mutex_);
            changed_ = true;
        }
        condition_.notify_all();
    } catch (...) {
        // Missing a wakeup can delay only observational output. The next
        // registered deadline or update wakes the service again.
    }
}

void ProgressHeartbeatService::heartbeat_loop(
    const std::stop_token stop) noexcept {
    auto deadline = std::chrono::steady_clock::time_point::max();
    while (!stop.stop_requested()) {
        std::vector<std::shared_ptr<ProgressCore>> live;
        try {
            {
                std::unique_lock lock(mutex_);
                if (deadline ==
                    std::chrono::steady_clock::time_point::max()) {
                    condition_.wait(lock, [&] {
                        return stop.stop_requested() || changed_;
                    });
                } else {
                    condition_.wait_until(lock, deadline, [&] {
                        return stop.stop_requested() || changed_;
                    });
                }
                if (stop.stop_requested()) return;
                changed_ = false;
                auto destination = cores_.begin();
                for (auto source = cores_.begin();
                     source != cores_.end();
                     ++source) {
                    if (auto core = source->lock()) {
                        live.push_back(std::move(core));
                        *destination++ = *source;
                    }
                }
                cores_.erase(destination, cores_.end());
            }

            const auto now = std::chrono::steady_clock::now();
            deadline = std::chrono::steady_clock::time_point::max();
            for (const auto& core : live) {
                const auto candidate =
                    core->emit_due_heartbeats(now);
                if (candidate)
                    deadline = std::min(deadline, *candidate);
            }
        } catch (...) {
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(100);
        }
    }
}

} // namespace detail

ProgressReporter::ProgressReporter(ProgressCallback callback,
                                   const std::chrono::milliseconds minimum_update_interval,
                                   const std::chrono::milliseconds heartbeat_interval) {
    if (callback) {
        core_ = std::make_shared<detail::ProgressCore>(
            std::move(callback), minimum_update_interval, heartbeat_interval);
        detail::progress_heartbeat_service().register_core(core_);
    }
}

ProgressReporter::ProgressReporter(std::shared_ptr<detail::ProgressCore> core,
                                   const std::optional<std::uint64_t> parent_scope_id) noexcept
    : core_(std::move(core)), parent_scope_id_(parent_scope_id) {}

bool ProgressReporter::enabled() const noexcept {
    return static_cast<bool>(core_);
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
    scope_mutex_ = std::make_shared<std::recursive_mutex>();
    delivery_state_ = std::make_shared<detail::ProgressDeliveryState>();
    scope_id_ = core_->next_scope_id();
    last_emission_ = std::chrono::steady_clock::now();
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        emission =
            prepare_emission_locked(ProgressState::Started, true);
    }
    if (emission) deliver(std::move(*emission));
}

ProgressScope::ProgressScope(ProgressScope&& other) noexcept
    : core_(std::move(other.core_)), scope_mutex_(std::move(other.scope_mutex_)),
      delivery_state_(std::move(other.delivery_state_)),
      operation_(other.operation_), unit_(other.unit_), total_(other.total_),
      label_(std::move(other.label_)), parent_scope_id_(other.parent_scope_id_),
      scope_id_(other.scope_id_), completed_(other.completed_), counters_(other.counters_),
      last_emission_(other.last_emission_),
      emission_revision_(other.emission_revision_),
      terminal_(other.terminal_) {
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
    last_emission_ = other.last_emission_;
    emission_revision_ = other.emission_revision_;
    terminal_ = other.terminal_;
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
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (!core_ || terminal_) return;
        if (completed < completed_ ||
            (total_ && completed > *total_)) {
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Failed, true);
        } else {
            completed_ = completed;
            emission =
                prepare_emission_locked(
                    ProgressState::Running, false);
        }
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::update(const std::uint64_t completed, ProgressCounterSnapshot counters) {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (!core_ || terminal_) return;
        counters_ = std::move(counters);
        if (completed < completed_ ||
            (total_ && completed > *total_)) {
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Failed, true);
        } else {
            completed_ = completed;
            emission =
                prepare_emission_locked(
                    ProgressState::Running, false);
        }
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::update(ProgressCounterSnapshot counters) {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (!core_ || terminal_) return;
        counters_ = std::move(counters);
        emission =
            prepare_emission_locked(
                ProgressState::Running, false);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::advance(const std::uint64_t amount) {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (!core_ || terminal_) return;
        if (amount >
            std::numeric_limits<std::uint64_t>::max() -
                completed_) {
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Failed, true);
        } else {
            completed_ += amount;
            if (total_ && completed_ > *total_) {
                completed_ -= amount;
                terminal_ = true;
                emission =
                    prepare_emission_locked(
                        ProgressState::Failed, true);
            } else {
                emission =
                    prepare_emission_locked(
                        ProgressState::Running, false);
            }
        }
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::heartbeat(ProgressCounterSnapshot counters) {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (!core_ || terminal_) return;
        counters_ = std::move(counters);
        emission =
            prepare_emission_locked(
                ProgressState::Heartbeat, false);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::complete() {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (terminal_) return;
        if (!core_) {
            terminal_ = true;
            return;
        }
        completed_ = total_.value_or(completed_);
        terminal_ = true;
        emission =
            prepare_emission_locked(
                ProgressState::Completed, true);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::complete(const std::uint64_t completed) {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (terminal_) return;
        if (!core_) {
            terminal_ = true;
            return;
        }
        if (completed < completed_ ||
            (total_ && completed != *total_)) {
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Failed, true);
        } else {
            completed_ = completed;
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Completed, true);
        }
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::cached() {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (terminal_) return;
        if (!core_) {
            terminal_ = true;
            return;
        }
        completed_ = total_.value_or(completed_);
        terminal_ = true;
        emission =
            prepare_emission_locked(
                ProgressState::Cached, true);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::skipped() {
    std::optional<PreparedEmission> emission;
    {
        const auto lock = lock_scope();
        if (terminal_) return;
        if (!core_) {
            terminal_ = true;
            return;
        }
        terminal_ = true;
        emission =
            prepare_emission_locked(
                ProgressState::Skipped, true);
    }
    if (emission) deliver(std::move(*emission));
}

void ProgressScope::fail() noexcept {
    try {
        std::optional<PreparedEmission> emission;
        {
            const auto lock = lock_scope();
            if (terminal_) return;
            terminal_ = true;
            emission =
                prepare_emission_locked(
                    ProgressState::Failed, true);
        }
        if (emission) deliver(std::move(*emission));
    } catch (...) {
        // Destruction and reporting remain non-throwing if synchronization itself fails.
    }
}

std::optional<ProgressScope::PreparedEmission>
ProgressScope::prepare_emission_locked(
    const ProgressState state,
    const bool force) noexcept {
    if (delivery_state_ &&
        (state == ProgressState::Completed ||
         state == ProgressState::Cached ||
         state == ProgressState::Skipped ||
         state == ProgressState::Failed))
        delivery_state_->terminal.store(true, std::memory_order_release);
    try {
        if (!core_ || !delivery_state_) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            now - last_emission_ <
                core_->minimum_update_interval())
            return std::nullopt;
        last_emission_ = now;
        ++emission_revision_;
        return PreparedEmission{
            core_,
            delivery_state_,
            {operation_,
             state,
             unit_,
             0u,
             0u,
             scope_id_,
             parent_scope_id_,
             completed_,
             total_,
             counters_,
             label_},
            emission_revision_};
    } catch (...) {
        // Reporting must remain observational even under allocation pressure.
        return std::nullopt;
    }
}

void ProgressScope::deliver(
    PreparedEmission emission) noexcept {
    if (!emission.core) return;
    emission.core->emit(
        std::move(emission.event),
        emission.scope_revision,
        emission.delivery_state);
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

} // namespace katana
