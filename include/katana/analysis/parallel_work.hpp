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

namespace detail {

inline thread_local ParallelWorkExecutor* current_analysis_executor = nullptr;

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

    void submit(Task task) {
        if (!task)
            throw std::invalid_argument(
                "Analysis-Executor akzeptiert keinen leeren Task.");
        {
            std::lock_guard lock(queue_mutex_);
            if (stopping_)
                throw std::runtime_error(
                    "Analysis-Executor wird bereits beendet.");
            tasks_.push_back(std::move(task));
        }
        task_available_.notify_one();
    }

    [[nodiscard]] bool current_thread_is_worker() const noexcept {
        return detail::current_analysis_executor == this;
    }

    template <typename Done>
    void help_until(Done&& done) {
        while (!done()) {
            Task task;
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
            task();
        }
    }

    void notify_waiters() noexcept {
        {
            std::lock_guard lock(queue_mutex_);
        }
        task_available_.notify_all();
    }

  private:
    void worker_loop() noexcept {
        auto* const previous =
            std::exchange(detail::current_analysis_executor, this);
        for (;;) {
            Task task;
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
                task();
            } catch (...) {
                // Submitted wrappers are required to contain task failures.
                // Never let a defensive last resort terminate the worker pool.
            }
        }
        detail::current_analysis_executor = previous;
    }

    std::size_t worker_count_ = 1u;
    std::mutex queue_mutex_;
    std::condition_variable task_available_;
    std::deque<Task> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

namespace detail {

template <typename Work>
class ParallelWorkGroup final {
  public:
    ParallelWorkGroup(ParallelWorkExecutor& executor,
                      const std::size_t item_count,
                      Work work)
        : executor_(executor), item_count_(item_count),
          work_(std::move(work)), errors_(item_count) {}

    void add_drain() noexcept {
        remaining_drains_.fetch_add(1u, std::memory_order_relaxed);
    }

    void cancel_drain() noexcept { complete_drain(); }

    void drain() noexcept {
        struct Completion final {
            ParallelWorkGroup* group;
            ~Completion() { group->complete_drain(); }
        } completion{this};
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

  private:
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

    ParallelWorkExecutor& executor_;
    std::size_t item_count_ = 0u;
    Work work_;
    std::atomic_size_t next_item_ = 0u;
    std::atomic_size_t remaining_drains_ = 0u;
    std::vector<std::exception_ptr> errors_;
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
            std::forward<Work>(work));
    std::exception_ptr submit_error;
    for (std::size_t index = 0u; index < local_jobs; ++index) {
        ParallelWorkExecutor::Task task =
            [group]() noexcept { group->drain(); };
        group->add_drain();
        try {
            executor.submit(std::move(task));
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
