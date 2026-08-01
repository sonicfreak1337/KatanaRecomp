#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace katana::analysis {

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

    explicit ParallelWorkExecutor(const std::size_t worker_count)
        : worker_count_(worker_count) {
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
        {
            std::lock_guard lock(queue_mutex_);
            stopping_ = true;
        }
        task_available_.notify_all();
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
        return active_workers_.load(std::memory_order_relaxed);
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
        {
            std::lock_guard lock(queue_mutex_);
            if (stopping_)
                throw std::runtime_error(
                    "Analysis-Executor wird bereits beendet.");
            tasks_.push_back(
                {std::move(task), std::move(after_activity)});
        }
        task_available_.notify_one();
    }

    [[nodiscard]] bool current_thread_is_worker() const noexcept {
        return detail::current_analysis_executor == this;
    }

    template <typename Done>
    void help_until(Done&& done) {
        while (!done()) {
            QueuedTask task;
            {
                std::unique_lock lock(queue_mutex_);
                task_available_.wait(
                    lock,
                    [&] {
                        return done() || !tasks_.empty() || stopping_;
                    });
                if (done()) return;
                if (tasks_.empty()) continue;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            execute_task(task);
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
        Task work;
        Task after_activity;
    };

    void execute_task(QueuedTask& task) {
        std::exception_ptr work_error;
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
                task.work();
            } catch (...) {
                work_error = std::current_exception();
            }
        }
        std::exception_ptr completion_error;
        if (task.after_activity) {
            try {
                task.after_activity();
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
            QueuedTask task;
            {
                std::unique_lock lock(queue_mutex_);
                task_available_.wait(
                    lock,
                    [&] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    if (stopping_) break;
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                execute_task(task);
            } catch (...) {
                // Submitted wrappers are required to contain task failures.
                // Never let a defensive last resort terminate the worker pool.
            }
        }
        detail::current_analysis_executor = previous;
    }

    std::size_t worker_count_ = 1u;
    std::atomic_size_t active_workers_ = 0u;
    std::mutex queue_mutex_;
    std::condition_variable task_available_;
    std::deque<QueuedTask> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

namespace detail {

template <typename Work>
class ParallelWorkGroup final {
  public:
    ParallelWorkGroup(ParallelWorkExecutor& executor,
                      const std::size_t item_count,
                      Work work,
                      ParallelWorkActivity* const activity)
        : executor_(executor), item_count_(item_count),
          work_(std::move(work)), errors_(item_count),
          activity_(activity) {}

    void add_drain() noexcept {
        remaining_drains_.fetch_add(1u, std::memory_order_relaxed);
    }

    void cancel_drain() noexcept { complete_drain(); }

    void drain() noexcept {
        const ParallelWorkActivityScope activity_scope{activity_};
        for (;;) {
            const auto index =
                next_item_.fetch_add(1u, std::memory_order_relaxed);
            if (index >= item_count_) return;
            try {
                std::invoke(work_, index);
            } catch (...) {
                errors_[index] = std::current_exception();
            }
        }
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
    }

    void complete_drain() noexcept {
        bool completed = false;
        {
            std::lock_guard lock(completion_mutex_);
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
    Work work_;
    std::atomic_size_t next_item_ = 0u;
    std::atomic_size_t remaining_drains_ = 0u;
    std::vector<std::exception_ptr> errors_;
    ParallelWorkActivity* activity_ = nullptr;
    mutable std::mutex completion_mutex_;
    std::condition_variable completion_;
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
    using WorkType = std::decay_t<Work>;
    auto group =
        std::make_shared<detail::ParallelWorkGroup<WorkType>>(
            executor,
            item_count,
            std::forward<Work>(work),
            activity);
    std::exception_ptr submit_error;
    for (std::size_t index = 0u; index < local_jobs; ++index) {
        ParallelWorkExecutor::Task task =
            [group]() noexcept { group->drain(); };
        group->add_drain();
        try {
            executor.submit(
                std::move(task),
                [group]() noexcept {
                    group->complete_drain();
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
