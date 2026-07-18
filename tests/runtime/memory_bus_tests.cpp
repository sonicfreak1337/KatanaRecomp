#include "katana/runtime/memory.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
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
    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::Memory;
    using katana::runtime::MemoryLookupMode;
    using katana::runtime::MemoryRegionAccess;

    Memory bus(0u);
    const auto work_ram = std::make_shared<LinearMemoryDevice>(16u);
    const auto adjacent_ram = std::make_shared<LinearMemoryDevice>(16u);
    const auto boot_rom = std::make_shared<LinearMemoryDevice>(8u);

    boot_rom->write_u8(0u, 0xA5u);

    bus.map_region("work-ram", 0x00001000u, work_ram);
    bus.map_region("adjacent-ram", 0x00001010u, adjacent_ram);
    bus.map_region("boot-rom", 0x00002000u, boot_rom, MemoryRegionAccess::ReadOnly);

    require(bus.region_count() == 3u && bus.region(0u).name == "work-ram" &&
                bus.region(1u).name == "adjacent-ram" && bus.region(2u).name == "boot-rom",
            "Speicherregionen werden nicht deterministisch nach Basisadresse sortiert.");
    require(bus.size() == 40u && bus.contains(0x00001000u, 16u) && !bus.contains(0x0000100Fu, 2u) &&
                !bus.contains(0x00003000u),
            "Regionsgroesse oder Adressdekodierung ist falsch.");

    bus.write_u32(0x00001004u, 0x89ABCDEFu);
    require(bus.read_u8(0x00001004u) == 0xEFu && bus.read_u16(0x00001004u) == 0xCDEFu &&
                bus.read_u32(0x00001004u) == 0x89ABCDEFu,
            "Der Speicherbus garantiert Little Endian nicht zentral.");
    require(work_ram->read_u8(4u) == 0xEFu && work_ram->read_u8(7u) == 0x89u,
            "Busadressen werden nicht korrekt in Geraeteoffsets uebersetzt.");
    require(bus.read_u8(0x00002000u) == 0xA5u,
            "Lesen aus einer Read-only-Region ist fehlgeschlagen.");

    require(throws<std::runtime_error>([&bus] { bus.write_u8(0x00002000u, 0x5Au); }),
            "Schreibzugriffe auf Read-only-Regionen werden nicht abgelehnt.");
    require(throws<katana::runtime::MemoryAccessError>(
                [&bus] { static_cast<void>(bus.read_u16(0x0000100Fu)); }),
            "Ein fehlplatzierter Mehrbytezugriff wird nicht sichtbar abgelehnt.");
    require(throws<katana::runtime::MemoryAccessError>(
                [&bus] { static_cast<void>(bus.read_u8(0x00003000u)); }),
            "Nicht zugeordnete Adressen werden nicht sichtbar abgelehnt.");
    require(throws<std::invalid_argument>([&bus] {
                bus.map_region("overlap", 0x00001008u, std::make_shared<LinearMemoryDevice>(8u));
            }),
            "Ueberlappende Speicherregionen werden akzeptiert.");
    require(throws<std::invalid_argument>([&bus] {
                bus.map_region("overflow", 0xFFFFFFFFu, std::make_shared<LinearMemoryDevice>(2u));
            }),
            "Regionen duerfen den 32-Bit-Adressraum nicht ueberschreiten.");

    Memory indexed(0u);
    auto indexed_ram = std::make_shared<LinearMemoryDevice>(0x20000u);
    indexed.map_region("indexed-ram", 0x00100000u, indexed_ram);
    indexed.write_u32(0x00110000u, 0xA1B2C3D4u);
    const auto indexed_value = indexed.read_u32(0x00110000u);
    require(indexed_value == 0xA1B2C3D4u &&
                indexed.performance_counters().indexed_region_hits >= 2u &&
                indexed.performance_counters().unobserved_accesses == 2u,
            "Regionsindex, nativer Linearzugriff oder Nullbeobachterpfad wurde nicht verwendet.");
    indexed.set_lookup_mode(MemoryLookupMode::Reference);
    indexed.reset_performance_counters();
    require(indexed.read_u32(0x00110000u) == indexed_value &&
                indexed.performance_counters().reference_region_probes != 0u,
            "Deaktivierter Speicherfastpath liefert nicht dieselben Gastbytes.");
    const auto watchpoint = indexed.add_watchpoint(
        0x00110000u,
        4u,
        katana::runtime::MemoryWatchpointAccess::Read,
        [](const auto&) {});
    static_cast<void>(indexed.read_u32(0x00110000u));
    require(indexed.performance_counters().observed_accesses == 1u,
            "Watchpoint umgeht den beobachteten Speicherpfad.");
    static_cast<void>(indexed.remove_watchpoint(watchpoint));

    std::cout << "Regionbasierter Speicherbus erfolgreich.\n";
    return EXIT_SUCCESS;
}
