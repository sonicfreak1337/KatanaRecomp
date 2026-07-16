#include "katana/runtime/scheduler_safepoint.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::runtime {

SchedulerSafepoints::SchedulerSafepoints(
    EventScheduler& scheduler,
    const std::size_t event_budget,
    const std::uint64_t loop_quantum,
    std::function<bool()> interrupt_delivery
) : scheduler_(scheduler), event_budget_(event_budget), loop_quantum_(loop_quantum),
    interrupt_delivery_(std::move(interrupt_delivery)) {
    if (event_budget == 0u || loop_quantum == 0u) {
        throw std::invalid_argument("Scheduler-Safepoints brauchen Ereignisbudget und Schleifenquantum.");
    }
}

SafepointReport SchedulerSafepoints::consume(
    const std::uint64_t guest_cycles,
    const SafepointKind kind,
    const ExecutionOrigin origin
) {
    if (guest_cycles > std::numeric_limits<std::uint64_t>::max() - scheduler_.current_cycle()) {
        throw std::overflow_error("Gastzyklusbudget laeuft ueber.");
    }
    const auto target = scheduler_.current_cycle() + guest_cycles;
    const auto advanced = scheduler_.advance_to(target, event_budget_);
    const bool interrupt = interrupt_delivery_ ? interrupt_delivery_() : false;
    const auto delivered = scheduler_.current_cycle();
    const auto jitter = delivered > target ? delivered - target : target - delivered;
    SafepointReport report{
        kind, origin, target, delivered, jitter, advanced.processed_events, interrupt,
        advanced.status == SchedulerAdvanceStatus::EventBudgetExhausted
    };
    reports_.push_back(report);
    return report;
}

std::vector<SafepointReport> SchedulerSafepoints::consume_loop(
    std::uint64_t guest_cycles,
    const ExecutionOrigin origin
) {
    std::vector<SafepointReport> result;
    while (guest_cycles != 0u) {
        const auto chunk = std::min(guest_cycles, loop_quantum_);
        const auto before = scheduler_.current_cycle();
        result.push_back(consume(chunk, SafepointKind::LoopBackedge, origin));
        const auto consumed = result.back().delivered_cycle - before;
        if (consumed == 0u && result.back().budget_exhausted) {
            throw std::runtime_error(
                "Scheduler-Ereignisbudget ist ohne Gastzyklusfortschritt erschoepft."
            );
        }
        guest_cycles -= consumed;
    }
    return result;
}

const std::vector<SafepointReport>& SchedulerSafepoints::reports() const noexcept { return reports_; }

std::string SchedulerSafepoints::machine_report() const {
    std::ostringstream out;
    for (const auto& report : reports_) {
        out << "kind=" << static_cast<unsigned>(report.kind)
            << ";origin=" << static_cast<unsigned>(report.origin)
            << ";requested=" << report.requested_cycle
            << ";delivered=" << report.delivered_cycle
            << ";jitter=" << report.jitter
            << ";events=" << report.processed_events
            << ";interrupt=" << (report.interrupt_delivered ? 1 : 0)
            << ";budget=" << (report.budget_exhausted ? 1 : 0) << '\n';
    }
    return out.str();
}

} // namespace katana::runtime
