#pragma once

#include "katana/runtime/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace katana::runtime {

class SystemReplayLog;

enum class ExecutionOrigin : std::uint8_t { Backend, Fallback };
enum class SafepointKind : std::uint8_t { BlockEnd, LoopBackedge, BeforeDelaySlot, AfterDelaySlot };

struct SafepointReport {
    SafepointKind kind = SafepointKind::BlockEnd;
    ExecutionOrigin origin = ExecutionOrigin::Backend;
    std::uint64_t requested_cycle = 0u;
    std::uint64_t delivered_cycle = 0u;
    std::uint64_t jitter = 0u;
    std::size_t processed_events = 0u;
    bool interrupt_delivered = false;
    bool budget_exhausted = false;
    bool guest_cycle_budget_exhausted = false;
};

class SchedulerSafepoints {
  public:
    SchedulerSafepoints(EventScheduler& scheduler,
                        std::size_t event_budget,
                        std::uint64_t loop_quantum,
                        std::function<bool()> interrupt_delivery = {},
                        SystemReplayLog* replay_log = nullptr);

    [[nodiscard]] SafepointReport
    consume(std::uint64_t guest_cycles, SafepointKind kind, ExecutionOrigin origin);
    [[nodiscard]] std::vector<SafepointReport> consume_loop(std::uint64_t guest_cycles,
                                                            ExecutionOrigin origin);
    [[nodiscard]] const std::vector<SafepointReport>& reports() const noexcept;
    [[nodiscard]] std::string machine_report() const;

  private:
    EventScheduler& scheduler_;
    std::size_t event_budget_;
    std::uint64_t loop_quantum_;
    std::function<bool()> interrupt_delivery_;
    SystemReplayLog* replay_log_ = nullptr;
    std::vector<SafepointReport> reports_;
};

} // namespace katana::runtime
