#include "katana/runtime/disc.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
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
    std::vector<std::uint8_t> image(3u * 2048u);
    std::fill(image.begin() + 2048, image.begin() + 4096, static_cast<std::uint8_t>(0x5Au));
    auto source = std::make_shared<MemoryDiscSource>(image, "synthetic:gdrom");
    GdRomDrive drive(source);
    require(drive.execute({GdRomCommand::TestUnitReady}).status == GdRomStatus::Good,
        "GD-ROM meldet eingelegte synthetische Disc nicht als bereit.");
    const auto status = drive.execute({GdRomCommand::GetStatus});
    require(status.status == GdRomStatus::Good && status.data.size() == 4u && status.data[0] == 0u,
        "GD-ROM-Statusantwort ist nicht deterministisch.");
    const auto capacity = drive.execute({GdRomCommand::GetCapacity});
    require(capacity.status == GdRomStatus::Good && capacity.data ==
        std::vector<std::uint8_t>({0u, 0u, 0u, 2u, 0u, 0u, 8u, 0u}),
        "GD-ROM-Kapazitaet ist nicht als Big-Endian Last-LBA und Blockgroesse kodiert.");
    const auto read = drive.execute({GdRomCommand::ReadSectors, 1u, 1u});
    require(read.status == GdRomStatus::Good && read.transferred_sectors == 1u &&
        read.data.size() == 2048u && read.data.front() == 0x5Au && read.data.back() == 0x5Au,
        "GD-ROM-Sektorread liefert falsche Daten oder Transferzahl.");
    require(drive.execute({GdRomCommand::ReadSectors, 2u, 2u}).status == GdRomStatus::OutOfRange,
        "GD-ROM akzeptiert einen Read ueber das Discende.");
    require(drive.execute({GdRomCommand::ReadSectors, 0u, 0u}).status == GdRomStatus::InvalidField,
        "GD-ROM akzeptiert einen Nullsektor-Read.");
    require(drive.execute({static_cast<GdRomCommand>(0xFFu)}).status == GdRomStatus::InvalidCommand,
        "GD-ROM akzeptiert ein unbekanntes Kommando.");
    auto empty = std::make_shared<MemoryDiscSource>(std::span<const std::uint8_t>{}, "synthetic:empty");
    require(GdRomDrive(empty).execute({GdRomCommand::TestUnitReady}).status == GdRomStatus::NoMedia,
        "Leere Discquelle wird als eingelegtes Medium gemeldet.");
    require(throws<std::invalid_argument>([] { GdRomDrive invalid(nullptr); }),
        "GD-ROM-Laufwerk akzeptiert eine fehlende Quelle.");

    std::cout << "KR-3002 GD-ROM-Kommandos erfolgreich.\n";
}
