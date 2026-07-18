#include "katana/runtime/scheduler.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function> bool throws(Function&& function) {
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
        static_cast<void>(scheduler.schedule_at(
            10u, [&](const auto nested, const auto) { order.push_back(nested); }));
    });
    const auto second =
        scheduler.schedule_at(10u, [&](const auto id, const auto) { order.push_back(id); });

    const auto ordered = scheduler.advance_to(20u, 4u);
    require(ordered.status == SchedulerAdvanceStatus::ReachedTarget &&
                ordered.processed_events == 4u && ordered.guest_cycle == 20u &&
                order.size() == 4u && order[0] == first && order[1] == second &&
                order[2] > second && order[3] == late,
            "Scheduler-Reihenfolge ist nicht nach Zyklus und Einfuege-ID stabil.");
    require(scheduler.processed_event_count() == 4u && scheduler.pending_event_count() == 0u,
            "Scheduler-Zaehler stimmen nach geordnetem Lauf nicht.");

    EventScheduler reentrant;
    bool nested_advance_rejected = false;
    bool nested_advance_by_rejected = false;
    bool nested_reset_rejected = false;
    bool nested_cancel_succeeded = false;
    std::size_t nested_schedule_callbacks = 0u;
    const auto cancelled_from_callback = reentrant.schedule_at(15u, [](const auto, const auto) {});
    static_cast<void>(reentrant.schedule_at(10u, [&](const auto, const auto) {
        nested_advance_rejected =
            throws<std::logic_error>([&] { static_cast<void>(reentrant.advance_to(100u, 1u)); });
        nested_advance_by_rejected =
            throws<std::logic_error>([&] { static_cast<void>(reentrant.advance_by(90u, 1u)); });
        nested_reset_rejected = throws<std::logic_error>([&] { reentrant.reset(); });
        nested_cancel_succeeded = reentrant.cancel(cancelled_from_callback);
        static_cast<void>(reentrant.schedule_at(
            12u, [&](const auto, const auto) { ++nested_schedule_callbacks; }));
    }));
    const auto guarded = reentrant.advance_to(20u, 2u);
    require(
        nested_advance_rejected && nested_advance_by_rejected && nested_reset_rejected &&
            nested_cancel_succeeded && nested_schedule_callbacks == 1u &&
            guarded.status == SchedulerAdvanceStatus::ReachedTarget &&
            guarded.processed_events == 2u && guarded.guest_cycle == 20u &&
            reentrant.current_cycle() == 20u && reentrant.processed_event_count() == 2u,
        "Scheduler erlaubt rekursive Zeitmutation oder blockiert sichere Callback-Operationen.");

    const auto cancelled = scheduler.schedule_after(5u, [](const auto, const auto) {});
    require(scheduler.cancel(cancelled) && !scheduler.cancel(cancelled) &&
                !scheduler.next_event_cycle().has_value(),
            "Scheduler-Cancellation ist nicht idempotent sichtbar.");

    std::size_t budget_callbacks = 0u;
    static_cast<void>(
        scheduler.schedule_at(25u, [&](const auto, const auto) { ++budget_callbacks; }));
    static_cast<void>(
        scheduler.schedule_at(26u, [&](const auto, const auto) { ++budget_callbacks; }));
    const auto limited = scheduler.advance_to(30u, 1u);
    require(limited.status == SchedulerAdvanceStatus::EventBudgetExhausted &&
                limited.processed_events == 1u && limited.guest_cycle == 25u &&
                scheduler.current_cycle() == 25u && scheduler.pending_event_count() == 1u,
            "Ereignisbudget stoppt nicht deterministisch am letzten Callback.");
    const auto resumed = scheduler.advance_to(30u, 1u);
    require(resumed.status == SchedulerAdvanceStatus::ReachedTarget &&
                resumed.processed_events == 1u && budget_callbacks == 2u,
            "Scheduler kann nach Budgetstopp nicht deterministisch fortsetzen.");

    require(throws<std::invalid_argument>([&] {
                static_cast<void>(scheduler.schedule_at(29u, [](const auto, const auto) {}));
            }) &&
                throws<std::invalid_argument>(
                    [&] { static_cast<void>(scheduler.advance_to(29u, 1u)); }) &&
                throws<std::invalid_argument>(
                    [&] { static_cast<void>(scheduler.schedule_at(30u, {})); }),
            "Rueckwaertszeit oder leerer Callback wird nicht abgewiesen.");

    scheduler.reset();
    static_cast<void>(scheduler.advance_to(std::numeric_limits<std::uint64_t>::max(), 0u));
    require(
        throws<std::overflow_error>([&] {
            static_cast<void>(scheduler.schedule_after(1u, [](const auto, const auto) {}));
        }) &&
            throws<std::overflow_error>([&] { static_cast<void>(scheduler.advance_by(1u, 0u)); }),
        "Scheduler-Zielzyklusueberlauf wird nicht sichtbar abgewiesen.");

    scheduler.reset();
    static_cast<void>(scheduler.schedule_at(
        3u, [](const auto, const auto) { throw std::runtime_error("sichtbarer Callbackfehler"); }));
    require(throws<std::runtime_error>([&] { static_cast<void>(scheduler.advance_to(5u, 1u)); }) &&
                scheduler.current_cycle() == 3u && scheduler.pending_event_count() == 0u &&
                scheduler.processed_event_count() == 1u,
            "Callbackfehler wird verschluckt oder hinterlaesst unklaren Schedulerzustand.");

    require(scheduler.advance_to(5u, 0u).guest_cycle == 5u,
            "Callbackfehler laesst den Scheduler faelschlich im Advance-Zustand.");

    std::uint64_t late_delivery = 0u;
    static_cast<void>(scheduler.schedule_at_or_now(
        4u, [&](const auto, const auto delivered_cycle) { late_delivery = delivered_cycle - 4u; }));
    static_cast<void>(scheduler.advance_to(5u, 1u));
    require(late_delivery == 1u,
            "Explizit verspaetet eingestelltes Ereignis misst nicht die echte Gastzykluslatenz.");

    scheduler.reset();
    require(scheduler.current_cycle() == 0u && scheduler.processed_event_count() == 0u &&
                scheduler.pending_event_count() == 0u,
            "Scheduler-Reset stellt keinen deterministischen Grundzustand her.");

    EventScheduler reset_ids;
    std::size_t reset_notifications = 0u;
    const auto observer = reset_ids.add_reset_observer([&] { ++reset_notifications; });
    const auto stale = reset_ids.schedule_at(10u, [](const auto, const auto) {});
    reset_ids.reset();
    const auto fresh = reset_ids.schedule_at(10u, [](const auto, const auto) {});
    require(fresh > stale && !reset_ids.cancel(stale) && reset_ids.pending_event_count() == 1u &&
                reset_notifications == 1u && reset_ids.reset_generation() == 1u &&
                reset_ids.remove_reset_observer(observer),
            "Scheduler-Reset recycelt Ereignis-IDs oder benachrichtigt Zeitgeber nicht sicher.");

    require(katana::runtime::parse_guest_cycle_budget("18446744073709551615") ==
                    std::numeric_limits<std::uint64_t>::max() &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("0")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("-1")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("+1")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget(" 10")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("10 ")); }) &&
                throws<std::invalid_argument>(
                    [] { static_cast<void>(katana::runtime::parse_guest_cycle_budget("text")); }) &&
                throws<std::invalid_argument>([] {
                    static_cast<void>(
                        katana::runtime::parse_guest_cycle_budget("18446744073709551616"));
                }),
            "Gastzyklusbudgetparser akzeptiert Null, Text oder 64-Bit-Ueberlauf.");

    const auto budget_case = [](const std::uint64_t budget) {
        EventScheduler test;
        test.set_guest_cycle_budget(budget);
        std::size_t callbacks = 0u;
        static_cast<void>(
            test.schedule_at(10u, [&](const auto, const auto) { ++callbacks; }));
        return std::pair{test.advance_to(20u, 1u), callbacks};
    };
    const auto [below_deadline, below_callbacks] = budget_case(9u);
    const auto [at_deadline, at_callbacks] = budget_case(10u);
    const auto [above_deadline, above_callbacks] = budget_case(11u);
    require(below_deadline.status == SchedulerAdvanceStatus::GuestCycleBudgetExhausted &&
                below_deadline.guest_cycle == 9u && below_callbacks == 0u &&
                at_deadline.status == SchedulerAdvanceStatus::GuestCycleBudgetExhausted &&
                at_deadline.guest_cycle == 10u && at_callbacks == 1u &&
                above_deadline.status == SchedulerAdvanceStatus::GuestCycleBudgetExhausted &&
                above_deadline.guest_cycle == 11u && above_callbacks == 1u,
            "Gastbudget unter, auf oder ueber einer Ereignisfrist kappt falsch.");

    EventScheduler bounded;
    bounded.set_guest_cycle_budget(10u);
    std::uint64_t bounded_event_cycle = 0u;
    static_cast<void>(bounded.schedule_at(
        10u, [&](const auto, const auto cycle) { bounded_event_cycle = cycle; }));
    const auto bounded_result = bounded.advance_to(20u, 1u);
    require(bounded_result.status == SchedulerAdvanceStatus::GuestCycleBudgetExhausted &&
                bounded_result.guest_cycle == 10u && bounded_event_cycle == 10u &&
                bounded.remaining_guest_cycles() == 0u &&
                bounded.advance_by(1u, 0u).status ==
                    SchedulerAdvanceStatus::GuestCycleBudgetExhausted &&
                throws<std::logic_error>([&] { bounded.set_guest_cycle_budget(11u); }),
            "Gastzyklusbudget kappt nicht exakt oder kann waehrend des Laufs mutieren.");

    std::cout << "KR-3101 Event-Scheduler erfolgreich.\n";
    return 0;
}
