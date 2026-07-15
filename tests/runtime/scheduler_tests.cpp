#include "katana/runtime/scheduler.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    using katana::runtime::EventScheduler;
    using katana::runtime::SchedulerAdvanceStatus;

    EventScheduler scheduler;
    std::vector<std::uint64_t> order;
    const auto late = scheduler.schedule_at(20u, [&](const auto id, const auto cycle) {
        require(cycle == scheduler.current_cycle(), "Callback sieht nicht seinen Gastzyklus.");
        order.push_back(id);
    });
    const auto first = scheduler.schedule_at(10u, [&](const auto id, const auto) {
        order.push_back(id);
        static_cast<void>(scheduler.schedule_at(10u, [&](const auto nested, const auto) {
            order.push_back(nested);
        }));
    });
    const auto second = scheduler.schedule_at(10u, [&](const auto id, const auto) {
        order.push_back(id);
    });

    const auto ordered = scheduler.advance_to(20u, 4u);
    require(
        ordered.status == SchedulerAdvanceStatus::ReachedTarget &&
            ordered.processed_events == 4u && ordered.guest_cycle == 20u &&
            order.size() == 4u && order[0] == first && order[1] == second &&
            order[2] > second && order[3] == late,
        "Scheduler-Reihenfolge ist nicht nach Zyklus und Einfuege-ID stabil."
    );
    require(
        scheduler.processed_event_count() == 4u && scheduler.pending_event_count() == 0u,
        "Scheduler-Zaehler stimmen nach geordnetem Lauf nicht."
    );

    EventScheduler reentrant;
    bool nested_advance_rejected = false;
    bool nested_advance_by_rejected = false;
    bool nested_reset_rejected = false;
    bool nested_cancel_succeeded = false;
    std::size_t nested_schedule_callbacks = 0u;
    const auto cancelled_from_callback = reentrant.schedule_at(
        15u,
        [](const auto, const auto) {}
    );
    static_cast<void>(reentrant.schedule_at(10u, [&](const auto, const auto) {
        nested_advance_rejected = throws<std::logic_error>([&] {
            static_cast<void>(reentrant.advance_to(100u, 1u));
        });
        nested_advance_by_rejected = throws<std::logic_error>([&] {
            static_cast<void>(reentrant.advance_by(90u, 1u));
        });
        nested_reset_rejected = throws<std::logic_error>([&] { reentrant.reset(); });
        nested_cancel_succeeded = reentrant.cancel(cancelled_from_callback);
        static_cast<void>(reentrant.schedule_at(12u, [&](const auto, const auto) {
            ++nested_schedule_callbacks;
        }));
    }));
    const auto guarded = reentrant.advance_to(20u, 2u);
    require(
        nested_advance_rejected && nested_advance_by_rejected && nested_reset_rejected &&
            nested_cancel_succeeded && nested_schedule_callbacks == 1u &&
            guarded.status == SchedulerAdvanceStatus::ReachedTarget &&
            guarded.processed_events == 2u && guarded.guest_cycle == 20u &&
            reentrant.current_cycle() == 20u && reentrant.processed_event_count() == 2u,
        "Scheduler erlaubt rekursive Zeitmutation oder blockiert sichere Callback-Operationen."
    );

    const auto cancelled = scheduler.schedule_after(5u, [](const auto, const auto) {});
    require(
        scheduler.cancel(cancelled) && !scheduler.cancel(cancelled) &&
            !scheduler.next_event_cycle().has_value(),
        "Scheduler-Cancellation ist nicht idempotent sichtbar."
    );

    std::size_t budget_callbacks = 0u;
    static_cast<void>(scheduler.schedule_at(25u, [&](const auto, const auto) {
        ++budget_callbacks;
    }));
    static_cast<void>(scheduler.schedule_at(26u, [&](const auto, const auto) {
        ++budget_callbacks;
    }));
    const auto limited = scheduler.advance_to(30u, 1u);
    require(
        limited.status == SchedulerAdvanceStatus::EventBudgetExhausted &&
            limited.processed_events == 1u && limited.guest_cycle == 25u &&
            scheduler.current_cycle() == 25u && scheduler.pending_event_count() == 1u,
        "Ereignisbudget stoppt nicht deterministisch am letzten Callback."
    );
    const auto resumed = scheduler.advance_to(30u, 1u);
    require(
        resumed.status == SchedulerAdvanceStatus::ReachedTarget &&
            resumed.processed_events == 1u && budget_callbacks == 2u,
        "Scheduler kann nach Budgetstopp nicht deterministisch fortsetzen."
    );

    require(
        throws<std::invalid_argument>([&] {
            static_cast<void>(scheduler.schedule_at(29u, [](const auto, const auto) {}));
        }) &&
            throws<std::invalid_argument>([&] {
                static_cast<void>(scheduler.advance_to(29u, 1u));
            }) &&
            throws<std::invalid_argument>([&] {
                static_cast<void>(scheduler.schedule_at(30u, {}));
            }),
        "Rueckwaertszeit oder leerer Callback wird nicht abgewiesen."
    );

    scheduler.reset();
    static_cast<void>(scheduler.advance_to(std::numeric_limits<std::uint64_t>::max(), 0u));
    require(
        throws<std::overflow_error>([&] {
            static_cast<void>(scheduler.schedule_after(1u, [](const auto, const auto) {}));
        }) &&
            throws<std::overflow_error>([&] {
                static_cast<void>(scheduler.advance_by(1u, 0u));
            }),
        "Scheduler-Zielzyklusueberlauf wird nicht sichtbar abgewiesen."
    );

    scheduler.reset();
    static_cast<void>(scheduler.schedule_at(3u, [](const auto, const auto) {
        throw std::runtime_error("sichtbarer Callbackfehler");
    }));
    require(
        throws<std::runtime_error>([&] {
            static_cast<void>(scheduler.advance_to(5u, 1u));
        }) &&
            scheduler.current_cycle() == 3u && scheduler.pending_event_count() == 0u &&
            scheduler.processed_event_count() == 1u,
        "Callbackfehler wird verschluckt oder hinterlaesst unklaren Schedulerzustand."
    );

    require(
        scheduler.advance_to(5u, 0u).guest_cycle == 5u,
        "Callbackfehler laesst den Scheduler faelschlich im Advance-Zustand."
    );

    scheduler.reset();
    require(
        scheduler.current_cycle() == 0u && scheduler.processed_event_count() == 0u &&
            scheduler.pending_event_count() == 0u,
        "Scheduler-Reset stellt keinen deterministischen Grundzustand her."
    );

    std::cout << "KR-3101 Event-Scheduler erfolgreich.\n";
    return 0;
}
