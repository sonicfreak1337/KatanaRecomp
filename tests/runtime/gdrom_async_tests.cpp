#include "katana/runtime/disc.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

int main() {
    using namespace katana::runtime;
    std::vector<std::uint8_t> image(4u * 2048u, 0xA5u);
    auto source = std::make_shared<MemoryDiscSource>(image, "synthetic:async-gdrom");
    GdRomAsyncReader reader(GdRomDrive(source), {10u, 5u});
    const auto read_id = reader.submit({GdRomCommand::ReadSectors, 1u, 2u});
    const auto status_id = reader.submit({GdRomCommand::GetStatus});
    require(reader.pending_count() == 2u && read_id == 1u && status_id == 2u,
        "Asynchrone GD-ROM-Requests erhalten keine stabilen IDs.");
    reader.advance_to(9u);
    require(!reader.take_completed().has_value(), "GD-ROM-Request wird vor seinem Zielzyklus fertig.");
    reader.advance_to(10u);
    const auto status = reader.take_completed();
    require(status && status->request_id == status_id && status->ready_cycle == 10u &&
        status->response.status == GdRomStatus::Good,
        "Kurzer GD-ROM-Request wird nicht nach Fertigstellungszyklus geordnet.");
    reader.advance_to(20u);
    const auto read = reader.take_completed();
    require(read && read->request_id == read_id && read->ready_cycle == 20u &&
        read->response.data.size() == 4096u && reader.pending_count() == 0u,
        "Asynchroner Sektorread verliert Daten, Timing oder Pending-Zustand.");
    require(throws<std::invalid_argument>([&] { reader.advance_to(19u); }),
        "GD-ROM-Zyklusuhr akzeptiert einen Ruecksprung.");

    GdRomAsyncReader overflow(GdRomDrive(source), {std::numeric_limits<std::uint64_t>::max(), 1u});
    overflow.advance_to(1u);
    require(throws<std::out_of_range>([&] { static_cast<void>(overflow.submit({GdRomCommand::GetStatus})); }),
        "Ueberlaufender GD-ROM-Fertigstellungszyklus wird akzeptiert.");

    std::cout << "KR-3004 Asynchrone Reads und Timing erfolgreich.\n";
}
