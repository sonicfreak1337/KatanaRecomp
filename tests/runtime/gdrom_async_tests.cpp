#include "katana/runtime/disc.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}

class ThrowingDiscSource final : public katana::runtime::DiscSource {
  public:
    [[nodiscard]] std::uint64_t size() const noexcept override { return 2048u; }
    [[nodiscard]] const std::string& identity() const noexcept override {
        return identity_;
    }
    void read(const std::uint64_t,
              const std::span<std::uint8_t>) const override {
        throw std::runtime_error("synthetischer GD-ROM-Lesefehler");
    }

  private:
    std::string identity_ = "synthetic:throwing-gdrom";
};
} // namespace

int main() {
    using namespace katana::runtime;
    std::vector<std::uint8_t> image(4u * 2048u, 0xA5u);
    auto source = std::make_shared<MemoryDiscSource>(image, "synthetic:async-gdrom");
    EventScheduler scheduler;
    std::vector<std::uint64_t> completion_cycles;
    GdRomAsyncReader reader(scheduler, GdRomDrive(source), {10u, 5u}, [&](const auto cycle) {
        completion_cycles.push_back(cycle);
    });
    const auto read_id = reader.submit({GdRomCommand::ReadSectors, 1u, 2u});
    const auto status_id = reader.submit({GdRomCommand::GetStatus});
    require(reader.pending_count() == 2u && read_id == 1u && status_id == 2u,
            "Asynchrone GD-ROM-Requests erhalten keine stabilen IDs.");
    static_cast<void>(scheduler.advance_to(9u, 0u));
    require(!reader.take_completed().has_value(),
            "GD-ROM-Request wird vor seinem Zielzyklus fertig.");
    static_cast<void>(scheduler.advance_to(10u, 1u));
    const auto status = reader.take_completed();
    require(status && status->request_id == status_id && status->ready_cycle == 10u &&
                status->response.status == GdRomStatus::Good,
            "Kurzer GD-ROM-Request wird nicht nach Fertigstellungszyklus geordnet.");
    static_cast<void>(scheduler.advance_to(20u, 1u));
    const auto read = reader.take_completed();
    require(read && read->request_id == read_id && read->ready_cycle == 20u &&
                read->response.data.size() == 4096u && reader.pending_count() == 0u &&
                completion_cycles == std::vector<std::uint64_t>({10u, 20u}) &&
                reader.current_cycle() == scheduler.current_cycle(),
            "Asynchroner Sektorread verliert Daten, Timing oder Pending-Zustand.");
    const auto cancelled_request = reader.submit({GdRomCommand::GetStatus});
    require(reader.cancel(cancelled_request) && !reader.cancel(cancelled_request) &&
                reader.pending_count() == 0u,
            "GD-ROM-Abbruch entfernt den Schedulerauftrag nicht eindeutig.");
    static_cast<void>(scheduler.advance_to(30u, 1u));
    require(!reader.take_completed().has_value() &&
                completion_cycles == std::vector<std::uint64_t>({10u, 20u}),
            "Abgebrochener GD-ROM-Request erzeugt spaeter eine Scheinkompletion.");
    static_cast<void>(reader.submit({GdRomCommand::GetStatus}));
    reader.reset();
    static_cast<void>(scheduler.advance_to(40u, 1u));
    require(reader.pending_count() == 0u && !reader.take_completed().has_value(),
            "GD-ROM-Readerreset laesst einen alten Schedulerauftrag aktiv.");
    static_cast<void>(reader.submit({GdRomCommand::GetStatus}));
    scheduler.reset();
    require(reader.pending_count() == 0u && !reader.take_completed().has_value(),
            "Schedulerreset laesst einen alten GD-ROM-Request sichtbar.");
    const auto reset_request = reader.submit({GdRomCommand::GetStatus});
    static_cast<void>(scheduler.advance_to(10u, 1u));
    const auto reset_completion = reader.take_completed();
    require(reset_request == 1u && reset_completion &&
                reset_completion->request_id == reset_request &&
                reset_completion->ready_cycle == 10u,
            "GD-ROM reaktiviert nach Schedulerreset ID oder Frist nicht deterministisch.");
    EventScheduler overflow_scheduler;
    static_cast<void>(overflow_scheduler.advance_to(1u, 0u));
    GdRomAsyncReader overflow(
        overflow_scheduler, GdRomDrive(source), {std::numeric_limits<std::uint64_t>::max(), 1u});
    require(throws<std::out_of_range>(
                [&] { static_cast<void>(overflow.submit({GdRomCommand::GetStatus})); }),
            "Ueberlaufender GD-ROM-Fertigstellungszyklus wird akzeptiert.");
    require(overflow.pending_count() == 0u &&
                overflow_scheduler.pending_event_count() == 0u,
            "Abgelehnte GD-ROM-Submission hinterlaesst Queue- oder Schedulerzustand.");

    EventScheduler failure_scheduler;
    std::uint64_t throwing_observer_calls = 0u;
    GdRomAsyncReader failure_reader(
        failure_scheduler,
        GdRomDrive(std::make_shared<ThrowingDiscSource>()),
        {1u, 1u},
        [&](const auto) {
            ++throwing_observer_calls;
            throw std::runtime_error("synthetischer Completionbeobachterfehler");
        });
    const auto failed_read =
        failure_reader.submit({GdRomCommand::ReadSectors, 0u, 1u});
    require(!throws<std::exception>(
                [&] { static_cast<void>(failure_scheduler.advance_to(2u, 1u)); }),
            "Erwartbarer GD-ROM-Lese- oder Beobachterfehler tritt aus dem Scheduler aus.");
    const auto failed_completion = failure_reader.take_completed();
    require(failed_completion && failed_completion->request_id == failed_read &&
                failed_completion->response.status == GdRomStatus::Aborted &&
                failed_completion->response.data.empty() &&
                failure_reader.pending_count() == 0u &&
                throwing_observer_calls == 1u,
            "Fehlgeschlagene GD-ROM-Completion wird nicht atomar als ABORTED publiziert.");
    const auto recovery_request =
        failure_reader.submit({GdRomCommand::GetStatus});
    static_cast<void>(failure_scheduler.advance_to(3u, 1u));
    const auto recovery_completion = failure_reader.take_completed();
    require(recovery_completion &&
                recovery_completion->request_id == recovery_request &&
                recovery_completion->response.status == GdRomStatus::Good &&
                throwing_observer_calls == 2u,
            "GD-ROM-Reader ist nach einer fehlgeschlagenen Completion nicht wiederverwendbar.");

    std::cout << "KR-3004 Asynchrone Reads und Timing erfolgreich.\n";
}
