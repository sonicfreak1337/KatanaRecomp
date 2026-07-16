#include "katana/runtime/scheduler_safepoint.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace katana::runtime;
namespace { void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }

int main() {
    try {
        EventScheduler scheduler;
        std::vector<std::uint64_t> order;
        static_cast<void>(scheduler.schedule_at(5u, [&](auto, auto cycle) { order.push_back(cycle); }));
        bool interrupt_pending = true;
        SchedulerSafepoints safepoints(scheduler, 8u, 4u, [&] {
            const bool delivered = interrupt_pending;
            interrupt_pending = false;
            return delivered;
        });
        const auto exact = safepoints.consume(5u, SafepointKind::BlockEnd, ExecutionOrigin::Backend);
        require(order == std::vector<std::uint64_t>{5u} && exact.processed_events == 1u &&
                exact.interrupt_delivered && exact.jitter == 0u,
            "Ereignis genau am Blockende oder Interruptzustellung ist nicht deterministisch.");

        static_cast<void>(scheduler.schedule_at(7u, [&](auto, auto cycle) { order.push_back(cycle); }));
        const auto loop = safepoints.consume_loop(10u, ExecutionOrigin::Backend);
        require(loop.size() == 3u && order.back() == 7u && scheduler.current_cycle() == 15u,
            "Lange Schleife laesst faelliges Ereignis ueber das Quantum hinaus verhungern.");

        static_cast<void>(scheduler.schedule_at(16u, [&](auto, auto cycle) { order.push_back(cycle); }));
        const auto before = safepoints.consume(0u, SafepointKind::BeforeDelaySlot, ExecutionOrigin::Fallback);
        const auto after = safepoints.consume(1u, SafepointKind::AfterDelaySlot, ExecutionOrigin::Fallback);
        require(before.delivered_cycle == 15u && after.delivered_cycle == 16u && order.back() == 16u,
            "Safepoints vor und nach dem Delay Slot sind nicht getrennt.");

        EventScheduler comparison_scheduler;
        SchedulerSafepoints comparison(comparison_scheduler, 8u, 4u);
        const auto backend = comparison.consume(3u, SafepointKind::BlockEnd, ExecutionOrigin::Backend);
        comparison_scheduler.reset();
        SchedulerSafepoints fallback(comparison_scheduler, 8u, 4u);
        const auto interpreted = fallback.consume(3u, SafepointKind::BlockEnd, ExecutionOrigin::Fallback);
        require(backend.delivered_cycle == interpreted.delivered_cycle &&
                safepoints.machine_report().find("jitter=") != std::string::npos,
            "Backend und Fallback verbrauchen inkompatible Zyklen oder berichten keinen Jitter.");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
