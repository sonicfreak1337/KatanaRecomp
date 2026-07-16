#include "katana/runtime/scheduler_safepoint.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        EventScheduler scheduler;
        std::vector<std::uint64_t> order;
        static_cast<void>(
            scheduler.schedule_at(5u, [&](auto, auto cycle) { order.push_back(cycle); }));
        bool interrupt_pending = true;
        SchedulerSafepoints safepoints(scheduler, 8u, 4u, [&] {
            const bool delivered = interrupt_pending;
            interrupt_pending = false;
            return delivered;
        });
        const auto exact =
            safepoints.consume(5u, SafepointKind::BlockEnd, ExecutionOrigin::Backend);
        require(order == std::vector<std::uint64_t>{5u} && exact.processed_events == 1u &&
                    exact.interrupt_delivered && exact.jitter == 0u,
                "Ereignis genau am Blockende oder Interruptzustellung ist nicht deterministisch.");

        static_cast<void>(
            scheduler.schedule_at(7u, [&](auto, auto cycle) { order.push_back(cycle); }));
        const auto loop = safepoints.consume_loop(10u, ExecutionOrigin::Backend);
        require(loop.size() == 3u && order.back() == 7u && scheduler.current_cycle() == 15u,
                "Lange Schleife laesst faelliges Ereignis ueber das Quantum hinaus verhungern.");

        static_cast<void>(
            scheduler.schedule_at(16u, [&](auto, auto cycle) { order.push_back(cycle); }));
        const auto before =
            safepoints.consume(0u, SafepointKind::BeforeDelaySlot, ExecutionOrigin::Fallback);
        const auto after =
            safepoints.consume(1u, SafepointKind::AfterDelaySlot, ExecutionOrigin::Fallback);
        require(before.delivered_cycle == 15u && after.delivered_cycle == 16u &&
                    order.back() == 16u,
                "Safepoints vor und nach dem Delay Slot sind nicht getrennt.");

        EventScheduler comparison_scheduler;
        SchedulerSafepoints comparison(comparison_scheduler, 8u, 4u);
        const auto backend =
            comparison.consume(3u, SafepointKind::BlockEnd, ExecutionOrigin::Backend);
        comparison_scheduler.reset();
        SchedulerSafepoints fallback(comparison_scheduler, 8u, 4u);
        const auto interpreted =
            fallback.consume(3u, SafepointKind::BlockEnd, ExecutionOrigin::Fallback);
        require(
            backend.delivered_cycle == interpreted.delivered_cycle &&
                safepoints.machine_report().find("jitter=") != std::string::npos,
            "Backend und Fallback verbrauchen inkompatible Zyklen oder berichten keinen Jitter.");

        EventScheduler pressure_scheduler;
        for (const auto cycle : {2u, 4u, 6u}) {
            static_cast<void>(pressure_scheduler.schedule_at(cycle, [](auto, auto) {}));
        }
        SchedulerSafepoints pressure_backend(pressure_scheduler, 1u, 10u);
        const auto pressured = pressure_backend.consume_loop(10u, ExecutionOrigin::Backend);
        require(pressured.size() == 3u && pressured[0].budget_exhausted &&
                    pressured[0].delivered_cycle == 2u && pressured[1].delivered_cycle == 4u &&
                    pressured.back().delivered_cycle == 10u &&
                    pressure_scheduler.current_cycle() == 10u,
                "Wiederholte Budgetstopps verlieren Gastzyklen oder erreichen den Zielzyklus nicht "
                "exakt.");

        EventScheduler fallback_pressure_scheduler;
        for (const auto cycle : {2u, 4u, 6u}) {
            static_cast<void>(fallback_pressure_scheduler.schedule_at(cycle, [](auto, auto) {}));
        }
        SchedulerSafepoints pressure_fallback(fallback_pressure_scheduler, 1u, 10u);
        const auto fallback_pressured =
            pressure_fallback.consume_loop(10u, ExecutionOrigin::Fallback);
        require(fallback_pressured.back().delivered_cycle == pressured.back().delivered_cycle,
                "Backend und Fallback laufen unter Ereignisbudgetdruck zyklisch auseinander.");

        EventScheduler stalled_scheduler;
        static_cast<void>(stalled_scheduler.schedule_at(0u, [](auto, auto) {}));
        static_cast<void>(stalled_scheduler.schedule_at(0u, [](auto, auto) {}));
        SchedulerSafepoints stalled(stalled_scheduler, 1u, 10u);
        bool no_progress_reported = false;
        try {
            static_cast<void>(stalled.consume_loop(10u, ExecutionOrigin::Backend));
        } catch (const std::runtime_error&) {
            no_progress_reported = true;
        }
        require(
            no_progress_reported && stalled_scheduler.current_cycle() == 0u,
            "Budgetstopp ohne Zyklusfortschritt fuehrt zur Endlosschleife oder bleibt unsichtbar.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
