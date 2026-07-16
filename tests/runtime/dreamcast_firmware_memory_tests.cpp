#include "katana/runtime/dreamcast_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

std::uint32_t direct_alias(const std::uint32_t segment_base, const std::uint32_t physical_base) {
    return segment_base + physical_base;
}

} // namespace

int main() {
    using katana::runtime::dreamcast_aica_ram_alias_count;
    using katana::runtime::dreamcast_bios_alias_count;
    using katana::runtime::dreamcast_bios_physical_base;
    using katana::runtime::dreamcast_bios_size;
    using katana::runtime::dreamcast_direct_segment_bases;
    using katana::runtime::dreamcast_flash_alias_count;
    using katana::runtime::dreamcast_flash_physical_base;
    using katana::runtime::dreamcast_flash_size;
    using katana::runtime::dreamcast_main_ram_alias_count;
    using katana::runtime::dreamcast_vram_alias_count;
    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::map_dreamcast_aica_ram;
    using katana::runtime::map_dreamcast_bios;
    using katana::runtime::map_dreamcast_flash;
    using katana::runtime::map_dreamcast_main_ram;
    using katana::runtime::map_dreamcast_vram;
    using katana::runtime::Memory;
    using katana::runtime::MemoryRegionAccess;

    std::vector<std::uint8_t> bios_image(dreamcast_bios_size, 0xFFu);
    bios_image[0u] = 0x12u;
    bios_image[0x00012345u] = 0x34u;
    bios_image[dreamcast_bios_size - 1u] = 0x56u;

    Memory bus(0u);
    const auto bios = map_dreamcast_bios(bus, bios_image);
    const auto flash = map_dreamcast_flash(bus);

    require(bios->size() == dreamcast_bios_size && flash->size() == dreamcast_flash_size,
            "BIOS oder Flash besitzt nicht die erwartete Groesse.");
    static_assert(dreamcast_bios_alias_count == 7u);
    static_assert(dreamcast_flash_alias_count == 7u);
    require(bus.region_count() == 14u, "BIOS- oder Flash-Aliaszahl stimmt nicht.");

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        const auto bios_base = direct_alias(segment_base, dreamcast_bios_physical_base);
        const auto flash_base = direct_alias(segment_base, dreamcast_flash_physical_base);

        require(bus.contains(bios_base, dreamcast_bios_size), "Ein direkter BIOS-Alias fehlt.");
        require(bus.contains(flash_base, dreamcast_flash_size), "Ein direkter Flash-Alias fehlt.");
        require(bus.read_u8(bios_base) == 0x12u && bus.read_u8(bios_base + 0x00012345u) == 0x34u &&
                    bus.read_u8(bios_base + static_cast<std::uint32_t>(dreamcast_bios_size - 1u)) ==
                        0x56u,
                "Ein BIOS-Alias verwendet nicht dasselbe geladene Abbild.");
    }

    require(!bus.contains(0xE0000000u) && !bus.contains(0xE0200000u),
            "P4 darf weder BIOS noch Flash enthalten.");
    require(throws<std::runtime_error>([&bus] { bus.write_u8(0x00000000u, 0xA5u); }),
            "BIOS-Schreibzugriffe werden nicht sichtbar abgelehnt.");
    require(bus.read_u8(0x80000000u) == 0x12u,
            "Ein abgelehnter BIOS-Schreibzugriff hat das Abbild veraendert.");

    require(flash->read_u8(0u) == 0xFFu &&
                flash->read_u8(static_cast<std::uint32_t>(dreamcast_flash_size - 1u)) == 0xFFu,
            "Leeres Flash ist nicht deterministisch auf 0xFF initialisiert.");

    bus.write_u32(0xA0200100u, 0x89ABCDEFu);
    require(bus.read_u32(0x60200100u) == 0x89ABCDEFu && flash->read_u8(0x00000100u) == 0xEFu &&
                flash->read_u8(0x00000103u) == 0x89u,
            "Flash-Aliase oder Little-Endian-Zugriffe sind inkonsistent.");

    bus.write_u8(0x00200000u, 0x11u);
    bus.write_u8(0xC0200000u + static_cast<std::uint32_t>(dreamcast_flash_size - 1u), 0x22u);
    require(bus.read_u8(0x80200000u) == 0x11u &&
                bus.read_u8(0x20200000u + static_cast<std::uint32_t>(dreamcast_flash_size - 1u)) ==
                    0x22u,
            "Erstes oder letztes Flash-Byte wird nicht gespiegelt.");

    bool saw_bios = false;
    bool saw_flash = false;
    for (std::size_t index = 0u; index < bus.region_count(); ++index) {
        const auto& region = bus.region(index);
        if (region.name.rfind("dreamcast-bios-", 0u) == 0u) {
            saw_bios = true;
            require(region.access == MemoryRegionAccess::ReadOnly,
                    "Ein BIOS-Alias ist nicht als read-only markiert.");
        }
        if (region.name.rfind("dreamcast-flash-", 0u) == 0u) {
            saw_flash = true;
            require(region.access == MemoryRegionAccess::ReadWrite,
                    "Ein Flash-Alias ist nicht beschreibbar.");
        }
    }
    require(saw_bios && saw_flash, "BIOS- oder Flash-Metadaten fehlen.");

    std::vector<std::uint8_t> flash_image(dreamcast_flash_size, 0xFFu);
    flash_image[0u] = 0xA1u;
    flash_image[dreamcast_flash_size - 1u] = 0xB2u;
    Memory loaded_flash_bus(0u);
    static_cast<void>(map_dreamcast_flash(loaded_flash_bus, flash_image));
    require(loaded_flash_bus.read_u8(0x00200000u) == 0xA1u &&
                loaded_flash_bus.read_u8(
                    0x00200000u + static_cast<std::uint32_t>(dreamcast_flash_size - 1u)) == 0xB2u,
            "Ein bereitgestelltes Flash-Abbild wird nicht vollstaendig kopiert.");

    Memory invalid_bios_bus(0u);
    const std::vector<std::uint8_t> short_bios(16u, 0u);
    require(throws<std::invalid_argument>([&invalid_bios_bus, &short_bios] {
                static_cast<void>(map_dreamcast_bios(invalid_bios_bus, short_bios));
            }) &&
                invalid_bios_bus.region_count() == 0u,
            "Eine falsche BIOS-Groesse wird nicht atomar abgelehnt.");

    Memory invalid_flash_bus(0u);
    const std::vector<std::uint8_t> short_flash(16u, 0u);
    require(throws<std::invalid_argument>([&invalid_flash_bus, &short_flash] {
                static_cast<void>(map_dreamcast_flash(invalid_flash_bus, short_flash));
            }) &&
                invalid_flash_bus.region_count() == 0u,
            "Eine falsche Flash-Groesse wird nicht atomar abgelehnt.");

    Memory conflicting_bios_bus(0u);
    conflicting_bios_bus.map_region(
        "collision", 0x80000000u, std::make_shared<LinearMemoryDevice>(16u));
    require(throws<std::invalid_argument>([&conflicting_bios_bus] {
                static_cast<void>(map_dreamcast_bios(conflicting_bios_bus));
            }) &&
                conflicting_bios_bus.region_count() == 1u &&
                !conflicting_bios_bus.contains(0x00000000u),
            "Eine BIOS-Kollision hinterlaesst Teilabbildungen.");

    Memory conflicting_flash_bus(0u);
    conflicting_flash_bus.map_region(
        "collision", 0x60200000u, std::make_shared<LinearMemoryDevice>(16u));
    require(throws<std::invalid_argument>([&conflicting_flash_bus] {
                static_cast<void>(map_dreamcast_flash(conflicting_flash_bus));
            }) &&
                conflicting_flash_bus.region_count() == 1u &&
                !conflicting_flash_bus.contains(0x00200000u),
            "Eine Flash-Kollision hinterlaesst Teilabbildungen.");

    Memory full_bus(0u);
    static_cast<void>(map_dreamcast_main_ram(full_bus));
    static_cast<void>(map_dreamcast_vram(full_bus));
    static_cast<void>(map_dreamcast_aica_ram(full_bus));
    static_cast<void>(map_dreamcast_bios(full_bus));
    static_cast<void>(map_dreamcast_flash(full_bus));
    require(full_bus.region_count() == dreamcast_main_ram_alias_count + dreamcast_vram_alias_count +
                                           dreamcast_aica_ram_alias_count +
                                           dreamcast_bios_alias_count + dreamcast_flash_alias_count,
            "Alle Dreamcast-Speicherbackings lassen sich nicht gemeinsam abbilden.");

    std::cout << "Dreamcast-BIOS und Flash erfolgreich.\n";
    return EXIT_SUCCESS;
}
