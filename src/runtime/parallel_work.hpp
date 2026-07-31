#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace katana::runtime::detail {

inline constexpr std::size_t runtime_parallel_job_limit = 64u;

[[nodiscard]] constexpr std::size_t
bounded_runtime_parallel_jobs(const std::size_t detected_jobs,
                              const std::size_t requested_jobs) noexcept {
    const auto detected =
        std::clamp<std::size_t>(detected_jobs, 1u, runtime_parallel_job_limit);
    return std::clamp<std::size_t>(requested_jobs, 1u, detected);
}

static_assert(bounded_runtime_parallel_jobs(24u, 64u) == 24u);
static_assert(bounded_runtime_parallel_jobs(24u, 12u) == 12u);
static_assert(bounded_runtime_parallel_jobs(0u, 64u) == 1u);

[[nodiscard]] inline std::size_t configured_runtime_parallel_jobs() noexcept {
    static const auto jobs = [] {
        const auto detected =
            std::max<std::size_t>(1u, std::thread::hardware_concurrency());
        const auto result =
            bounded_runtime_parallel_jobs(detected, detected);
        const char* configured = nullptr;
#if defined(_MSC_VER)
        char* configured_copy = nullptr;
        std::size_t configured_size = 0u;
        if (_dupenv_s(&configured_copy,
                      &configured_size,
                      "KATANA_RUNTIME_JOBS") != 0)
            return result;
        configured = configured_copy;
#else
        configured = std::getenv("KATANA_RUNTIME_JOBS");
#endif
        if (configured == nullptr || *configured == '\0') {
#if defined(_MSC_VER)
            std::free(configured_copy);
#endif
            return result;
        }

        const std::string_view text(configured);
        std::size_t parsed = 0u;
        const auto conversion =
            std::from_chars(text.data(), text.data() + text.size(), parsed);
        const auto valid =
            conversion.ec == std::errc{} &&
            conversion.ptr == text.data() + text.size() &&
            parsed != 0u;
#if defined(_MSC_VER)
        std::free(configured_copy);
#endif
        if (!valid)
            return result;
        return bounded_runtime_parallel_jobs(detected, parsed);
    }();
    return jobs;
}

class RuntimeParallelExecutor final {
  public:
    using Work = std::function<void(std::size_t, std::size_t)>;

    explicit RuntimeParallelExecutor(const std::size_t job_capacity) noexcept {
        const auto requested_workers =
            job_capacity > 1u ? job_capacity - 1u : 0u;
        try {
            workers_.reserve(requested_workers);
            for (std::size_t index = 0u; index < requested_workers; ++index) {
                static_cast<void>(index);
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            stopping_.store(true, std::memory_order_release);
            ready_.release(static_cast<std::ptrdiff_t>(workers_.size()));
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            workers_.clear();
            stopping_.store(false, std::memory_order_relaxed);
        }
    }

    ~RuntimeParallelExecutor() {
        std::lock_guard run_lock(run_mutex_);
        stopping_.store(true, std::memory_order_release);
        ready_.release(static_cast<std::ptrdiff_t>(workers_.size()));
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    RuntimeParallelExecutor(const RuntimeParallelExecutor&) = delete;
    RuntimeParallelExecutor& operator=(const RuntimeParallelExecutor&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return workers_.size() + 1u;
    }

    [[nodiscard]] std::size_t run(const std::size_t work_items,
                                  const Work& work) {
        if (work_items == 0u) return 0u;
        const auto lanes = std::min(work_items, capacity());
        if (lanes == 1u || active_executor_ == this) {
            work(0u, 1u);
            return 1u;
        }

        std::lock_guard run_lock(run_mutex_);
        Batch batch{&work, std::vector<std::exception_ptr>(lanes)};
        batch_ = &batch;
        next_lane_.store(1u, std::memory_order_relaxed);
        remaining_workers_.store(lanes - 1u, std::memory_order_relaxed);
        ready_.release(static_cast<std::ptrdiff_t>(lanes - 1u));

        const auto* const prior_executor = active_executor_;
        active_executor_ = this;
        try {
            work(0u, lanes);
        } catch (...) {
            batch.errors[0u] = std::current_exception();
        }
        active_executor_ = prior_executor;

        auto remaining =
            remaining_workers_.load(std::memory_order_acquire);
        while (remaining != 0u) {
            remaining_workers_.wait(
                remaining, std::memory_order_acquire);
            remaining =
                remaining_workers_.load(
                    std::memory_order_acquire);
        }
        batch_ = nullptr;
        for (const auto& error : batch.errors) {
            if (error) std::rethrow_exception(error);
        }
        return lanes;
    }

  private:
    struct Batch {
        const Work* work = nullptr;
        std::vector<std::exception_ptr> errors;
    };

    void worker_loop() noexcept {
        for (;;) {
            ready_.acquire();
            if (stopping_.load(std::memory_order_acquire)) return;
            auto* const batch = batch_;
            const auto lane =
                next_lane_.fetch_add(1u, std::memory_order_relaxed);
            const auto lanes = batch->errors.size();

            const auto* const prior_executor = active_executor_;
            active_executor_ = this;
            try {
                (*batch->work)(lane, lanes);
            } catch (...) {
                batch->errors[lane] = std::current_exception();
            }
            active_executor_ = prior_executor;

            // Chain every lane's writes through the RMW release sequence so
            // the caller's acquire predicate observes all completed result
            // buffers, not only the final worker's exception slot.
            if (remaining_workers_.fetch_sub(
                    1u, std::memory_order_acq_rel) == 1u)
                remaining_workers_.notify_one();
        }
    }

    inline static thread_local const RuntimeParallelExecutor* active_executor_ =
        nullptr;
    mutable std::mutex run_mutex_;
    std::counting_semaphore<runtime_parallel_job_limit> ready_{0};
    std::vector<std::thread> workers_;
    Batch* batch_ = nullptr;
    std::atomic<std::size_t> next_lane_{1u};
    std::atomic<std::size_t> remaining_workers_{0u};
    std::atomic<bool> stopping_{false};
};

[[nodiscard]] inline RuntimeParallelExecutor&
runtime_parallel_executor() noexcept {
    static RuntimeParallelExecutor executor(configured_runtime_parallel_jobs());
    return executor;
}

[[nodiscard]] inline std::size_t runtime_parallel_job_capacity() noexcept {
    return runtime_parallel_executor().capacity();
}

template <typename Function>
[[nodiscard]] std::size_t
run_runtime_parallel_work(const std::size_t work_items, Function&& function) {
    const RuntimeParallelExecutor::Work work(std::forward<Function>(function));
    return runtime_parallel_executor().run(work_items, work);
}

} // namespace katana::runtime::detail
