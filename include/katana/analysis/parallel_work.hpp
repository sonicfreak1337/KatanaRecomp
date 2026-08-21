#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace katana::analysis {

enum class AnalysisWorkPhase : std::uint8_t {
    Unspecified,
    Seed,
    ControlFlow,
    FunctionValue,
    GuardedInventory,
    Resolution,
    LatentAot,
};

enum class AnalysisWorkSubjectKind : std::uint8_t {
    Generic,
    Scc,
    Context,
    Root,
    Module,
};

enum class AnalysisWorkPriorityKind : std::uint8_t {
    SeedRelease,
    CriticalPrefix,
    Unblocking,
    Throughput,
};

struct AnalysisWorkDescriptor final {
    AnalysisWorkPhase phase = AnalysisWorkPhase::Unspecified;
    std::uint64_t dependency_epoch = 0u;
    AnalysisWorkSubjectKind subject_kind =
        AnalysisWorkSubjectKind::Generic;
    std::uint64_t subject = 0u;
    std::size_t estimated_cost = 1u;
    std::size_t fanout = 0u;
    AnalysisWorkPriorityKind priority =
        AnalysisWorkPriorityKind::Throughput;
    std::optional<std::uint64_t> critical_prefix;
    std::size_t transient_bytes = 0u;
    std::size_t quantum = 8u;
};

enum class AnalysisWorkDisposition : std::uint8_t {
    Complete,
    Yield,
};

inline constexpr std::size_t maximum_analysis_work_quantum = 1'024u;

class AnalysisMemoryBudgetExceeded final : public std::runtime_error {
  public:
    AnalysisMemoryBudgetExceeded(const std::size_t requested,
                                 const std::size_t capacity)
        : std::runtime_error(
              "Analysis-Workitem ueberschreitet das globale Speicherbudget."),
          requested_(requested), capacity_(capacity) {}

    [[nodiscard]] std::size_t requested() const noexcept {
        return requested_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

  private:
    std::size_t requested_ = 0u;
    std::size_t capacity_ = 0u;
};

class AnalysisMemoryBudget final {
  public:
    class Lease final {
      public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              bytes_(std::exchange(other.bytes_, 0u)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) return *this;
            release();
            owner_ = std::exchange(other.owner_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0u);
            return *this;
        }

        ~Lease() { release(); }

        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }

        [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

        void release() noexcept {
            if (owner_ == nullptr) return;
            owner_->release(bytes_);
            owner_ = nullptr;
            bytes_ = 0u;
        }

      private:
        Lease(AnalysisMemoryBudget& owner, const std::size_t bytes) noexcept
            : owner_(&owner), bytes_(bytes) {}

        AnalysisMemoryBudget* owner_ = nullptr;
        std::size_t bytes_ = 0u;

        friend class AnalysisMemoryBudget;
    };

    // A child keeps its own logical cap while every live lease is charged to
    // its parent as well.  This lets a phase reserve only memory it actually
    // retains instead of pessimistically pinning its complete local arena in
    // the process-wide executor for its entire lifetime.  Parents must
    // outlive their children; construction is one-way, so a budget graph
    // cannot contain a cycle.
    explicit AnalysisMemoryBudget(
        const std::size_t capacity,
        AnalysisMemoryBudget* const parent = nullptr,
        const std::size_t parent_headroom_bytes = 0u)
        : capacity_(capacity),
          parent_(parent),
          parent_headroom_bytes_(parent_headroom_bytes) {}

    AnalysisMemoryBudget(const AnalysisMemoryBudget&) = delete;
    AnalysisMemoryBudget& operator=(const AnalysisMemoryBudget&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return used_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t peak() const noexcept {
        return peak_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return capacity_ - used();
    }

    [[nodiscard]] bool parent_accounted() const noexcept {
        return parent_ != nullptr;
    }

    [[nodiscard]] std::optional<Lease> try_acquire(
        const std::size_t bytes) {
        if (!try_acquire_accounted(bytes, parent_headroom_bytes_))
            return std::nullopt;
        return Lease(*this, bytes);
    }

  private:
    [[nodiscard]] bool try_acquire_accounted(
        const std::size_t bytes,
        const std::size_t root_headroom_bytes) {
        if (bytes > capacity_)
            throw AnalysisMemoryBudgetExceeded(bytes, capacity_);
        if (parent_ != nullptr &&
            !parent_->try_acquire_accounted(bytes, root_headroom_bytes))
            return false;
        auto current = used_.load(std::memory_order_acquire);
        for (;;) {
            const auto retained_headroom = parent_ == nullptr
                                               ? std::min(
                                                     root_headroom_bytes,
                                                     capacity_)
                                               : 0u;
            if (current > capacity_ - retained_headroom ||
                bytes > capacity_ - retained_headroom - current) {
                if (parent_ != nullptr) parent_->release(bytes);
                return false;
            }
            if (used_.compare_exchange_weak(
                    current,
                    current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        const auto acquired = current + bytes;
        auto peak = peak_.load(std::memory_order_relaxed);
        while (peak < acquired &&
               !peak_.compare_exchange_weak(
                   peak,
                   acquired,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
        return true;
    }

    void release(const std::size_t bytes) noexcept {
        if (bytes != 0u)
            used_.fetch_sub(bytes, std::memory_order_release);
        if (parent_ != nullptr) parent_->release(bytes);
    }

    const std::size_t capacity_ = 0u;
    AnalysisMemoryBudget* const parent_ = nullptr;
    const std::size_t parent_headroom_bytes_ = 0u;
    std::atomic_size_t used_ = 0u;
    std::atomic_size_t peak_ = 0u;
};

[[nodiscard]] inline std::size_t configured_analysis_memory_budget_bytes() {
    constexpr std::uint64_t default_capacity = 8ull * 1024ull * 1024ull * 1024ull;
    const auto bounded_default = static_cast<std::size_t>(std::min<std::uint64_t>(
        default_capacity,
        std::numeric_limits<std::size_t>::max()));
#ifdef _WIN32
    char* configured_raw = nullptr;
    std::size_t configured_size = 0u;
    if (_dupenv_s(&configured_raw,
                  &configured_size,
                  "KATANA_ANALYSIS_MEMORY_BUDGET_BYTES") != 0)
        throw std::runtime_error(
            "KATANA_ANALYSIS_MEMORY_BUDGET_BYTES konnte nicht gelesen werden.");
    const std::unique_ptr<char, decltype(&std::free)> configured_owner(
        configured_raw, &std::free);
    const auto* const configured = configured_owner.get();
#else
    const auto* const configured =
        std::getenv("KATANA_ANALYSIS_MEMORY_BUDGET_BYTES");
#endif
    if (configured == nullptr || *configured == '\0') return bounded_default;
    const std::string_view value(configured);
    std::uint64_t parsed = 0u;
    const auto conversion = std::from_chars(
        value.data(), value.data() + value.size(), parsed, 10);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size() || parsed == 0u ||
        parsed > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "KATANA_ANALYSIS_MEMORY_BUDGET_BYTES ist ungueltig.");
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] inline AnalysisMemoryBudget& global_analysis_memory_budget() {
    static AnalysisMemoryBudget budget(
        configured_analysis_memory_budget_bytes());
    return budget;
}

struct ParallelWorkExecutorSnapshot final {
    std::size_t running = 0u;
    std::size_t waiting = 0u;
    std::size_t idle = 0u;
    std::size_t queued = 0u;
    std::size_t memory_blocked = 0u;
    std::size_t continuations = 0u;
    std::size_t memory_capacity = 0u;
    std::size_t memory_used = 0u;
    std::size_t memory_peak = 0u;
};

class ParallelWorkExecutor;
class ParallelWorkActivity;

namespace detail {

class ParallelWorkActivityScope;

inline thread_local ParallelWorkExecutor* current_analysis_executor = nullptr;
inline thread_local std::size_t current_analysis_executor_task_depth = 0u;
inline thread_local ParallelWorkActivity* current_parallel_work_activity =
    nullptr;
inline thread_local std::size_t current_parallel_work_activity_depth = 0u;

} // namespace detail

// Per-analysis activity domain. Unlike ParallelWorkExecutor::active_worker_count,
// this excludes unrelated groups sharing the process-wide executor. Nested
// help for the same domain counts the OS worker once.
class ParallelWorkActivity final {
  public:
    [[nodiscard]] std::size_t active_worker_count() const noexcept {
        return active_workers_.load(std::memory_order_acquire);
    }

  private:
    std::atomic_size_t active_workers_ = 0u;

    friend class detail::ParallelWorkActivityScope;
    friend class ParallelWorkExecutor;
};

namespace detail {

class ParallelWorkActivityScope final {
  public:
    explicit ParallelWorkActivityScope(
        ParallelWorkActivity* const activity) noexcept
        : activity_(activity),
          previous_activity_(current_parallel_work_activity),
          previous_depth_(current_parallel_work_activity_depth),
          changed_activity_(previous_activity_ != activity_) {
        if (!changed_activity_) {
            if (activity_ == nullptr) return;
            activated_current_ =
                current_parallel_work_activity_depth++ == 0u;
            if (activated_current_)
                activity_->active_workers_.fetch_add(
                    1u, std::memory_order_release);
            return;
        }

        // A worker helping another activity is no longer executing the
        // suspended domain while that nested task runs. Count it in exactly
        // one domain, including an explicitly unobserved/null domain.
        if (previous_activity_ != nullptr && previous_depth_ != 0u) {
            previous_activity_->active_workers_.fetch_sub(
                1u, std::memory_order_release);
            suspended_previous_ = true;
        }
        current_parallel_work_activity = activity_;
        current_parallel_work_activity_depth = 0u;
        if (activity_ != nullptr) {
            current_parallel_work_activity_depth = 1u;
            activity_->active_workers_.fetch_add(
                1u, std::memory_order_release);
            activated_current_ = true;
        }
    }

    ~ParallelWorkActivityScope() {
        if (!changed_activity_) {
            if (activity_ == nullptr) return;
            if (current_parallel_work_activity_depth != 0u)
                --current_parallel_work_activity_depth;
            if (activated_current_)
                activity_->active_workers_.fetch_sub(
                    1u, std::memory_order_release);
            return;
        }

        if (activated_current_)
            activity_->active_workers_.fetch_sub(
                1u, std::memory_order_release);
        current_parallel_work_activity = previous_activity_;
        current_parallel_work_activity_depth = previous_depth_;
        if (suspended_previous_)
            previous_activity_->active_workers_.fetch_add(
                1u, std::memory_order_release);
    }

  private:
    ParallelWorkActivity* activity_ = nullptr;
    ParallelWorkActivity* previous_activity_ = nullptr;
    std::size_t previous_depth_ = 0u;
    bool changed_activity_ = false;
    bool activated_current_ = false;
    bool suspended_previous_ = false;
};

} // namespace detail

// Fixed process-wide worker pool shared by every expensive analysis phase.
// Nested batches help the same queue while waiting, so even a single-worker
// executor cannot deadlock and no call creates an additional thread pool.
class ParallelWorkExecutor final {
  public:
    using Task = std::function<void()>;
    using ContinuationTask =
        std::function<AnalysisWorkDisposition()>;
    using CompletionTask =
        std::function<void(std::exception_ptr)>;
    using MemoryReclaimer = std::function<void(std::size_t)>;

    explicit ParallelWorkExecutor(const std::size_t worker_count)
        : ParallelWorkExecutor(worker_count,
                               global_analysis_memory_budget()) {}

    ParallelWorkExecutor(const std::size_t worker_count,
                         AnalysisMemoryBudget& memory_budget)
        : worker_count_(worker_count), memory_budget_(&memory_budget) {
        if (worker_count_ == 0u)
            throw std::invalid_argument(
                "Analysis-Executor braucht mindestens einen Worker.");
        workers_.reserve(worker_count_);
        try {
            for (std::size_t index = 0u; index < worker_count_; ++index)
                workers_.emplace_back([this] { worker_loop(); });
        } catch (...) {
            {
                std::lock_guard lock(queue_mutex_);
                stopping_ = true;
            }
            task_available_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
    }

    ParallelWorkExecutor(const ParallelWorkExecutor&) = delete;
    ParallelWorkExecutor& operator=(const ParallelWorkExecutor&) = delete;

    ~ParallelWorkExecutor() {
        std::deque<QueuedTask> cancelled;
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
            cancelled.swap(tasks_);
        }
        task_available_.notify_all();
        const auto cancellation = std::make_exception_ptr(
            std::runtime_error(
                "Analysis-Executor wurde vor Taskabschluss beendet."));
        for (auto& task : cancelled) {
            if (!task.completion) continue;
            try {
                task.completion(cancellation);
            } catch (...) {
            }
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    [[nodiscard]] std::size_t maximum_jobs() const noexcept {
        return worker_count_;
    }

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return worker_count_;
    }

    // Exact number of executor threads currently running a task. Nested
    // help-while-waiting work on the same OS thread is counted once, so this
    // remains a worker-utilization signal rather than a recursion counter.
    [[nodiscard]] std::size_t active_worker_count() const noexcept {
        return active_workers_.load(std::memory_order_acquire);
    }

    [[nodiscard]] ParallelWorkExecutorSnapshot snapshot() const noexcept {
        ParallelWorkExecutorSnapshot result;
        const std::lock_guard lock(queue_mutex_);
        result.running = active_workers_.load(std::memory_order_acquire);
        result.waiting = waiting_workers_.load(std::memory_order_acquire);
        result.queued = tasks_.size();
        const auto available = memory_budget_->available();
        for (const auto& task : tasks_) {
            if (task.descriptor.transient_bytes > available)
                ++result.memory_blocked;
            if (task.continuation) ++result.continuations;
        }
        result.idle = tasks_.empty() ? result.waiting : 0u;
        result.memory_capacity = memory_budget_->capacity();
        result.memory_used = memory_budget_->used();
        result.memory_peak = memory_budget_->peak();
        return result;
    }

    [[nodiscard]] AnalysisMemoryBudget& memory_budget() noexcept {
        return *memory_budget_;
    }

    [[nodiscard]] const AnalysisMemoryBudget& memory_budget() const noexcept {
        return *memory_budget_;
    }

    void set_memory_reclaimer(MemoryReclaimer reclaimer) {
        {
            const std::lock_guard lock(reclaimer_mutex_);
            memory_reclaimer_ = std::move(reclaimer);
        }
        task_available_.notify_all();
    }

    void submit(Task task) {
        submit(std::move(task), {});
    }

    // Completion runs after the executor has left the task's active-worker
    // scope. Group waiters therefore cannot observe a logically completed
    // batch while its worker is still counted as busy.
    void submit(Task task, Task after_activity) {
        if (!task)
            throw std::invalid_argument(
                "Analysis-Executor akzeptiert keinen leeren Task.");
        submit(
            AnalysisWorkDescriptor{},
            [task = std::move(task)]() mutable {
                task();
                return AnalysisWorkDisposition::Complete;
            },
            [after_activity = std::move(after_activity)](
                const std::exception_ptr error) mutable {
                std::exception_ptr completion_error;
                if (after_activity) {
                    try {
                        after_activity();
                    } catch (...) {
                        completion_error = std::current_exception();
                    }
                }
                if (error) std::rethrow_exception(error);
                if (completion_error)
                    std::rethrow_exception(completion_error);
            });
    }

    void submit_once(AnalysisWorkDescriptor descriptor,
                     Task task,
                     Task after_activity = {}) {
        if (!task)
            throw std::invalid_argument(
                "Analysis-Executor akzeptiert keinen leeren Task.");
        submit(
            std::move(descriptor),
            [task = std::move(task)]() mutable {
                task();
                return AnalysisWorkDisposition::Complete;
            },
            [after_activity = std::move(after_activity)](
                const std::exception_ptr error) mutable {
                std::exception_ptr completion_error;
                if (after_activity) {
                    try {
                        after_activity();
                    } catch (...) {
                        completion_error = std::current_exception();
                    }
                }
                if (error) std::rethrow_exception(error);
                if (completion_error)
                    std::rethrow_exception(completion_error);
            });
    }

    void submit(AnalysisWorkDescriptor descriptor,
                ContinuationTask task,
                CompletionTask completion = {}) {
        if (!task)
            throw std::invalid_argument(
                "Analysis-Executor akzeptiert keinen leeren Task.");
        validate_descriptor(descriptor);
        if (descriptor.transient_bytes > memory_budget_->capacity())
            throw AnalysisMemoryBudgetExceeded(
                descriptor.transient_bytes,
                memory_budget_->capacity());
        QueuedTask queued;
        queued.descriptor = std::move(descriptor);
        queued.work = std::move(task);
        queued.completion = std::move(completion);
        enqueue(std::move(queued), false);
    }

    [[nodiscard]] bool current_thread_is_worker() const noexcept {
        return detail::current_analysis_executor == this;
    }

    template <typename Done>
    void help_until(Done&& done) {
        while (!done()) {
            std::optional<ScheduledTask> scheduled;
            std::size_t reclaim_shortfall = 0u;
            {
                std::unique_lock lock(queue_mutex_);
                if (done()) return;
                scheduled = take_runnable_locked();
                if (!scheduled)
                    reclaim_shortfall =
                        smallest_memory_shortfall_locked();
            }
            if (scheduled) {
                try {
                    execute_task(std::move(*scheduled));
                } catch (...) {
                    // A typed completion owns task failure propagation. An
                    // unrelated nested waiter must never receive that error.
                }
                continue;
            }
            if (reclaim_shortfall != 0u &&
                run_memory_reclaimer(reclaim_shortfall))
                continue;
            std::unique_lock lock(queue_mutex_);
            if (done()) return;
            WaitingWorkerScope waiting{*this};
            task_available_.wait(
                lock,
                [&] {
                    return done() || has_runnable_locked() || stopping_;
                });
            if (done()) return;
            if (stopping_ && tasks_.empty()) return;
        }
    }

    void notify_waiters() noexcept {
        {
            std::lock_guard lock(queue_mutex_);
        }
        task_available_.notify_all();
    }

  private:
    struct QueuedTask final {
        AnalysisWorkDescriptor descriptor;
        ContinuationTask work;
        CompletionTask completion;
        std::uint64_t ticket = 0u;
        std::uint64_t enqueued_dispatch = 0u;
        bool continuation = false;
    };

    struct ScheduledTask final {
        QueuedTask task;
        AnalysisMemoryBudget::Lease lease;
    };

    class WaitingWorkerScope final {
      public:
        explicit WaitingWorkerScope(ParallelWorkExecutor& executor) noexcept
            : executor_(executor) {
            executor_.waiting_workers_.fetch_add(
                1u, std::memory_order_release);
            if (detail::current_analysis_executor == &executor_ &&
                detail::current_analysis_executor_task_depth != 0u) {
                executor_.active_workers_.fetch_sub(
                    1u, std::memory_order_release);
                suspended_executor_activity_ = true;
            }
        }

        ~WaitingWorkerScope() {
            if (suspended_executor_activity_)
                executor_.active_workers_.fetch_add(
                    1u, std::memory_order_release);
            executor_.waiting_workers_.fetch_sub(
                1u, std::memory_order_release);
        }

      private:
        ParallelWorkExecutor& executor_;
        bool suspended_executor_activity_ = false;
    };

    [[nodiscard]] static constexpr std::size_t priority_rank(
        const AnalysisWorkPriorityKind priority) noexcept {
        return static_cast<std::size_t>(priority);
    }

    [[nodiscard]] std::size_t aged_priority_rank(
        const QueuedTask& task) const noexcept {
        constexpr std::uint64_t fairness_dispatch_quantum = 32u;
        const auto base = priority_rank(task.descriptor.priority);
        const auto waited = dispatch_clock_ - task.enqueued_dispatch;
        const auto boost = static_cast<std::size_t>(std::min<std::uint64_t>(
            base, waited / fairness_dispatch_quantum));
        return base - boost;
    }

    [[nodiscard]] bool higher_priority(const QueuedTask& left,
                                       const QueuedTask& right) const noexcept {
        const auto left_aged = aged_priority_rank(left);
        const auto right_aged = aged_priority_rank(right);
        if (left_aged != right_aged) return left_aged < right_aged;
        const auto left_base = priority_rank(left.descriptor.priority);
        const auto right_base = priority_rank(right.descriptor.priority);
        const bool left_promoted = left_aged < left_base;
        const bool right_promoted = right_aged < right_base;
        if (left_promoted != right_promoted) return left_promoted;
        if (left_promoted && right_promoted)
            return left.ticket < right.ticket;
        if (left_base != right_base) return left_base < right_base;
        if (left.descriptor.priority ==
            AnalysisWorkPriorityKind::CriticalPrefix) {
            const auto left_prefix = left.descriptor.critical_prefix.value_or(
                std::numeric_limits<std::uint64_t>::max());
            const auto right_prefix = right.descriptor.critical_prefix.value_or(
                std::numeric_limits<std::uint64_t>::max());
            if (left_prefix != right_prefix)
                return left_prefix < right_prefix;
        }
        if (left.descriptor.priority ==
            AnalysisWorkPriorityKind::Unblocking) {
            if (left.descriptor.estimated_cost !=
                right.descriptor.estimated_cost)
                return left.descriptor.estimated_cost <
                       right.descriptor.estimated_cost;
        }
        if (left.descriptor.priority !=
                AnalysisWorkPriorityKind::Throughput &&
            left.descriptor.fanout != right.descriptor.fanout)
            return left.descriptor.fanout > right.descriptor.fanout;
        if (left.descriptor.priority !=
                AnalysisWorkPriorityKind::Throughput &&
            left.descriptor.estimated_cost !=
                right.descriptor.estimated_cost)
            return left.descriptor.estimated_cost <
                   right.descriptor.estimated_cost;
        return left.ticket < right.ticket;
    }

    static void validate_descriptor(
        const AnalysisWorkDescriptor& descriptor) {
        if (descriptor.estimated_cost == 0u)
            throw std::invalid_argument(
                "Analysis-Workitem braucht eine positive Kostenschaetzung.");
        if (descriptor.quantum == 0u)
            throw std::invalid_argument(
                "Analysis-Workitem braucht ein positives Quantum.");
        if (descriptor.quantum > maximum_analysis_work_quantum)
            throw std::invalid_argument(
                "Analysis-Workitem-Quantum ueberschreitet die sichere Grenze.");
        if (priority_rank(descriptor.priority) >
            priority_rank(AnalysisWorkPriorityKind::Throughput))
            throw std::invalid_argument(
                "Analysis-Workitem besitzt eine ungueltige Prioritaet.");
    }

    [[nodiscard]] std::uint64_t next_ticket_locked() {
        if (next_ticket_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error(
                "Analysis-Executor-Ticketzaehler ist erschoepft.");
        return next_ticket_++;
    }

    void enqueue(QueuedTask task, const bool continuation) {
        {
            std::lock_guard lock(queue_mutex_);
            if (stopping_)
                throw std::runtime_error(
                    "Analysis-Executor wird bereits beendet.");
            task.ticket = next_ticket_locked();
            task.enqueued_dispatch = dispatch_clock_;
            task.continuation = continuation;
            tasks_.push_back(std::move(task));
        }
        task_available_.notify_one();
    }

    [[nodiscard]] bool has_runnable_locked() const noexcept {
        const auto available = memory_budget_->available();
        return std::any_of(
            tasks_.begin(), tasks_.end(), [&](const auto& task) {
                return task.descriptor.transient_bytes <= available;
            });
    }

    [[nodiscard]] std::size_t
    smallest_memory_shortfall_locked() const noexcept {
        if (tasks_.empty()) return 0u;
        const auto available = memory_budget_->available();
        auto shortfall = std::numeric_limits<std::size_t>::max();
        for (const auto& task : tasks_) {
            if (task.descriptor.transient_bytes <= available) continue;
            shortfall = std::min(
                shortfall,
                task.descriptor.transient_bytes - available);
        }
        return shortfall == std::numeric_limits<std::size_t>::max()
                   ? 0u
                   : shortfall;
    }

    [[nodiscard]] std::optional<ScheduledTask>
    take_runnable_locked() {
        for (;;) {
            const auto available = memory_budget_->available();
            std::optional<std::size_t> best;
            for (std::size_t index = 0u; index < tasks_.size(); ++index) {
                if (tasks_[index].descriptor.transient_bytes > available)
                    continue;
                if (!best || higher_priority(tasks_[index], tasks_[*best]))
                    best = index;
            }
            if (!best) return std::nullopt;
            auto lease = memory_budget_->try_acquire(
                tasks_[*best].descriptor.transient_bytes);
            if (!lease) continue;
            ScheduledTask result{
                std::move(tasks_[*best]), std::move(*lease)};
            tasks_.erase(tasks_.begin() +
                         static_cast<std::ptrdiff_t>(*best));
            ++dispatch_clock_;
            return result;
        }
    }

    [[nodiscard]] bool run_memory_reclaimer(
        const std::size_t shortfall) noexcept {
        bool expected = false;
        if (!reclaiming_memory_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return false;
        MemoryReclaimer reclaimer;
        try {
            const std::lock_guard lock(reclaimer_mutex_);
            reclaimer = memory_reclaimer_;
        } catch (...) {
            reclaiming_memory_.store(false, std::memory_order_release);
            return false;
        }
        if (!reclaimer) {
            reclaiming_memory_.store(false, std::memory_order_release);
            return false;
        }
        const auto before = memory_budget_->available();
        try {
            // Deliberately outside queue_mutex_: reclaimers may query executor
            // state, release retained leases, or acquire cache-local locks.
            reclaimer(shortfall);
        } catch (...) {
        }
        const auto after = memory_budget_->available();
        reclaiming_memory_.store(false, std::memory_order_release);
        task_available_.notify_all();
        return after > before;
    }

    void execute_task(ScheduledTask scheduled) {
        auto task = std::move(scheduled.task);
        std::exception_ptr work_error;
        auto disposition = AnalysisWorkDisposition::Complete;
        struct ActiveTaskScope final {
            ParallelWorkExecutor& executor;
            ParallelWorkExecutor* previous_executor = nullptr;
            std::size_t previous_depth = 0u;
            bool outermost = false;

            explicit ActiveTaskScope(
                ParallelWorkExecutor& owner) noexcept
                : executor(owner),
                  previous_executor(
                      detail::current_analysis_executor),
                  previous_depth(
                      detail::current_analysis_executor_task_depth) {
                if (previous_executor != &executor) {
                    detail::current_analysis_executor = &executor;
                    detail::current_analysis_executor_task_depth = 0u;
                }
                outermost =
                    detail::current_analysis_executor_task_depth++ == 0u;
                if (outermost)
                    executor.active_workers_.fetch_add(
                        1u, std::memory_order_relaxed);
            }

            ~ActiveTaskScope() {
                if (detail::current_analysis_executor_task_depth != 0u)
                    --detail::current_analysis_executor_task_depth;
                if (outermost)
                    executor.active_workers_.fetch_sub(
                        1u, std::memory_order_relaxed);
                if (previous_executor != &executor) {
                    detail::current_analysis_executor =
                        previous_executor;
                    detail::current_analysis_executor_task_depth =
                        previous_depth;
                }
            }
        };
        {
            ActiveTaskScope active{*this};
            try {
                disposition = task.work();
                if (disposition != AnalysisWorkDisposition::Complete &&
                    disposition != AnalysisWorkDisposition::Yield)
                    throw std::logic_error(
                        "Analysis-Workitem lieferte einen ungueltigen Fortsetzungszustand.");
            } catch (...) {
                work_error = std::current_exception();
            }
        }
        scheduled.lease.release();
        task_available_.notify_all();
        if (!work_error && disposition == AnalysisWorkDisposition::Yield) {
            CompletionTask failure_completion;
            try {
                failure_completion = task.completion;
                enqueue(std::move(task), true);
                return;
            } catch (...) {
                work_error = std::current_exception();
                if (!task.completion)
                    task.completion = std::move(failure_completion);
            }
        }
        std::exception_ptr completion_error;
        if (task.completion) {
            try {
                task.completion(work_error);
            } catch (...) {
                completion_error = std::current_exception();
            }
        }
        if (work_error) std::rethrow_exception(work_error);
        if (completion_error)
            std::rethrow_exception(completion_error);
    }

    void worker_loop() noexcept {
        auto* const previous =
            std::exchange(detail::current_analysis_executor, this);
        for (;;) {
            std::optional<ScheduledTask> scheduled;
            std::size_t reclaim_shortfall = 0u;
            {
                std::unique_lock lock(queue_mutex_);
                scheduled = take_runnable_locked();
                if (!scheduled) {
                    if (stopping_ && tasks_.empty()) break;
                    reclaim_shortfall =
                        smallest_memory_shortfall_locked();
                }
            }
            if (scheduled) {
                try {
                    execute_task(std::move(*scheduled));
                } catch (...) {
                    // The completion owns failure propagation. Never let a
                    // defensive last resort terminate the worker pool.
                }
                continue;
            }
            if (reclaim_shortfall != 0u &&
                run_memory_reclaimer(reclaim_shortfall))
                continue;
            std::unique_lock lock(queue_mutex_);
            if (stopping_ && tasks_.empty()) break;
            WaitingWorkerScope waiting{*this};
            task_available_.wait(
                lock,
                [this] { return stopping_ || has_runnable_locked(); });
            if (stopping_ && tasks_.empty()) break;
        }
        detail::current_analysis_executor = previous;
    }

    std::size_t worker_count_ = 1u;
    AnalysisMemoryBudget* memory_budget_ = nullptr;
    std::atomic_size_t active_workers_ = 0u;
    std::atomic_size_t waiting_workers_ = 0u;
    mutable std::mutex queue_mutex_;
    std::condition_variable task_available_;
    std::deque<QueuedTask> tasks_;
    bool stopping_ = false;
    std::uint64_t next_ticket_ = 0u;
    std::uint64_t dispatch_clock_ = 0u;
    mutable std::mutex reclaimer_mutex_;
    MemoryReclaimer memory_reclaimer_;
    std::atomic_bool reclaiming_memory_ = false;
    std::vector<std::thread> workers_;
};

namespace detail {

struct ParallelWorkGroupExecutionFrame final {
    const void* group = nullptr;
    const ParallelWorkGroupExecutionFrame* previous = nullptr;
};

inline thread_local const ParallelWorkGroupExecutionFrame*
    current_parallel_work_group_execution = nullptr;

[[nodiscard]] inline bool parallel_work_group_is_active(
    const void* const group) noexcept {
    for (auto* frame = current_parallel_work_group_execution;
         frame != nullptr;
         frame = frame->previous) {
        if (frame->group == group) return true;
    }
    return false;
}

class ParallelWorkGroupExecutionScope final {
  public:
    explicit ParallelWorkGroupExecutionScope(
        const void* const group) noexcept
        : frame_{group, current_parallel_work_group_execution} {
        current_parallel_work_group_execution = &frame_;
    }

    ~ParallelWorkGroupExecutionScope() {
        current_parallel_work_group_execution = frame_.previous;
    }

    ParallelWorkGroupExecutionScope(
        const ParallelWorkGroupExecutionScope&) = delete;
    ParallelWorkGroupExecutionScope& operator=(
        const ParallelWorkGroupExecutionScope&) = delete;

  private:
    ParallelWorkGroupExecutionFrame frame_;
};

template <typename Work>
class ParallelWorkGroup final {
  public:
    ParallelWorkGroup(ParallelWorkExecutor& executor,
                      const std::size_t item_count,
                      const std::size_t quantum,
                      Work work,
                      ParallelWorkActivity* const activity)
        : executor_(executor), item_count_(item_count),
          quantum_(quantum), work_(std::move(work)), errors_(item_count),
          activity_(activity) {}

    void add_drain() noexcept {
        remaining_drains_.fetch_add(1u, std::memory_order_relaxed);
    }

    void cancel_drain() noexcept { complete_drain({}); }

    [[nodiscard]] AnalysisWorkDisposition drain_quantum() noexcept {
        // A worker waiting for a nested batch helps the shared executor.  It
        // must not re-enter another continuation of a work group already on
        // that worker's stack: a wide outer batch whose items launch nested
        // analysis would otherwise recursively enter itself once per lane and
        // can exhaust the finite host stack before any item returns.  Yielding
        // this continuation leaves it queued for another worker (or for this
        // worker after the active item unwinds) while still allowing unrelated
        // and genuinely nested groups to make progress.
        if (parallel_work_group_is_active(this))
            return AnalysisWorkDisposition::Yield;
        const ParallelWorkGroupExecutionScope execution_scope{this};
        const ParallelWorkActivityScope activity_scope{activity_};
        for (std::size_t consumed = 0u; consumed < quantum_; ++consumed) {
            const auto index =
                next_item_.fetch_add(1u, std::memory_order_relaxed);
            if (index >= item_count_)
                return AnalysisWorkDisposition::Complete;
            try {
                std::invoke(work_, index);
            } catch (...) {
                errors_[index] = std::current_exception();
            }
        }
        return next_item_.load(std::memory_order_acquire) < item_count_
                   ? AnalysisWorkDisposition::Yield
                   : AnalysisWorkDisposition::Complete;
    }

    [[nodiscard]] bool done() const noexcept {
        return remaining_drains_.load(std::memory_order_acquire) == 0u;
    }

    void wait() {
        if (executor_.current_thread_is_worker()) {
            executor_.help_until([this] { return done(); });
            return;
        }
        std::unique_lock lock(completion_mutex_);
        completion_.wait(lock, [this] { return done(); });
    }

    void rethrow_first_error() const {
        for (const auto& error : errors_) {
            if (error) std::rethrow_exception(error);
        }
        const std::lock_guard lock(completion_mutex_);
        if (scheduler_error_)
            std::rethrow_exception(scheduler_error_);
    }

    void complete_drain(const std::exception_ptr scheduler_error) noexcept {
        bool completed = false;
        {
            std::lock_guard lock(completion_mutex_);
            if (scheduler_error && !scheduler_error_)
                scheduler_error_ = scheduler_error;
            const auto previous =
                remaining_drains_.fetch_sub(
                    1u, std::memory_order_acq_rel);
            completed = previous == 1u;
        }
        if (!completed) return;
        completion_.notify_all();
        executor_.notify_waiters();
    }

  private:
    ParallelWorkExecutor& executor_;
    std::size_t item_count_ = 0u;
    std::size_t quantum_ = 1u;
    Work work_;
    std::atomic_size_t next_item_ = 0u;
    std::atomic_size_t remaining_drains_ = 0u;
    std::vector<std::exception_ptr> errors_;
    ParallelWorkActivity* activity_ = nullptr;
    mutable std::mutex completion_mutex_;
    std::condition_variable completion_;
    std::exception_ptr scheduler_error_;
};

} // namespace detail

[[nodiscard]] inline std::size_t configured_analysis_parallel_jobs() {
    constexpr std::size_t maximum_jobs = 64u;
    const auto hardware_jobs =
        static_cast<std::size_t>(
            std::max(1u, std::thread::hardware_concurrency()));
    auto result = std::min(maximum_jobs, hardware_jobs);
#ifdef _WIN32
    char* configured_raw = nullptr;
    std::size_t configured_size = 0u;
    if (_dupenv_s(&configured_raw,
                  &configured_size,
                  "KATANA_ANALYSIS_JOBS") != 0)
        throw std::runtime_error(
            "KATANA_ANALYSIS_JOBS konnte nicht gelesen werden.");
    const std::unique_ptr<char, decltype(&std::free)> configured_owner(
        configured_raw, &std::free);
    const auto* const configured = configured_owner.get();
#else
    const auto* const configured = std::getenv("KATANA_ANALYSIS_JOBS");
#endif
    if (configured == nullptr || *configured == '\0') return result;
    const std::string_view value(configured);
    std::size_t parsed = 0u;
    const auto conversion =
        std::from_chars(value.data(),
                        value.data() + value.size(),
                        parsed,
                        10);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size() ||
        parsed == 0u || parsed > maximum_jobs)
        throw std::invalid_argument(
            "KATANA_ANALYSIS_JOBS ist ungueltig.");
    return parsed;
}

[[nodiscard]] inline ParallelWorkExecutor&
global_analysis_executor() {
    static ParallelWorkExecutor executor(
        configured_analysis_parallel_jobs());
    return executor;
}

template <typename Work>
void parallel_analysis_for(ParallelWorkExecutor& executor,
                           AnalysisWorkDescriptor descriptor,
                           const std::size_t item_count,
                           const std::size_t maximum_local_jobs,
                           ParallelWorkActivity* const activity,
                           Work&& work) {
    if (item_count == 0u) return;
    if (maximum_local_jobs == 0u)
        throw std::invalid_argument(
            "Lokales Analysis-Parallelbudget muss mindestens einen Job erlauben.");
    const auto local_jobs = std::min(
        {item_count, maximum_local_jobs, executor.maximum_jobs()});
    if (descriptor.estimated_cost == 1u && item_count > 1u)
        descriptor.estimated_cost =
            1u + (item_count - 1u) / local_jobs;
    if (descriptor.fanout == 0u) descriptor.fanout = item_count;
    using WorkType = std::decay_t<Work>;
    auto group =
        std::make_shared<detail::ParallelWorkGroup<WorkType>>(
            executor,
            item_count,
            descriptor.quantum,
            std::forward<Work>(work),
            activity);
    std::exception_ptr submit_error;
    for (std::size_t index = 0u; index < local_jobs; ++index) {
        auto lane_descriptor = descriptor;
        ParallelWorkExecutor::ContinuationTask task =
            [group]() noexcept { return group->drain_quantum(); };
        group->add_drain();
        try {
            executor.submit(
                std::move(lane_descriptor),
                std::move(task),
                [group](const std::exception_ptr error) noexcept {
                    group->complete_drain(error);
                });
        } catch (...) {
            group->cancel_drain();
            submit_error = std::current_exception();
            break;
        }
    }
    group->wait();
    if (submit_error) std::rethrow_exception(submit_error);
    group->rethrow_first_error();
}

template <typename Work>
void parallel_analysis_for(ParallelWorkExecutor& executor,
                           const std::size_t item_count,
                           const std::size_t maximum_local_jobs,
                           ParallelWorkActivity* const activity,
                           Work&& work) {
    parallel_analysis_for(executor,
                          AnalysisWorkDescriptor{},
                          item_count,
                          maximum_local_jobs,
                          activity,
                          std::forward<Work>(work));
}

template <typename Work>
void parallel_analysis_for(ParallelWorkExecutor& executor,
                           const std::size_t item_count,
                           const std::size_t maximum_local_jobs,
                           Work&& work) {
    parallel_analysis_for(executor,
                          item_count,
                          maximum_local_jobs,
                          nullptr,
                          std::forward<Work>(work));
}

template <typename Work>
void parallel_analysis_for(ParallelWorkExecutor& executor,
                           const std::size_t item_count,
                           Work&& work) {
    parallel_analysis_for(executor,
                          item_count,
                          executor.maximum_jobs(),
                          std::forward<Work>(work));
}

template <typename Work>
void parallel_analysis_for(const std::size_t item_count,
                           const std::size_t maximum_local_jobs,
                           Work&& work) {
    parallel_analysis_for(global_analysis_executor(),
                          item_count,
                          maximum_local_jobs,
                          std::forward<Work>(work));
}

template <typename Work>
void parallel_analysis_for(const std::size_t item_count, Work&& work) {
    auto& executor = global_analysis_executor();
    parallel_analysis_for(executor,
                          item_count,
                          executor.maximum_jobs(),
                          std::forward<Work>(work));
}

} // namespace katana::analysis
