#include "katana/runtime/dreamcast_memory.hpp"

#include <cstddef>
#include <cstdint>
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

template <typename Exception, typename Function>
bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

std::uint32_t direct_alias(
    const std::uint32_t segment_base,
    const std::uint32_t physical_base
) {
    return segment_base + physical_base;
}

} // namespace

int main() {
    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::Memory;
    using katana::runtime::dreamcast_aica_ram_alias_count;
    using katana::runtime::dreamcast_aica_ram_physical_bases;
    using katana::runtime::dreamcast_aica_ram_size;
    using katana::runtime::dreamcast_direct_segment_bases;
    using katana::runtime::dreamcast_main_ram_alias_count;
    using katana::runtime::dreamcast_vram_32bit_alias_count;
    using katana::runtime::dreamcast_vram_32bit_physical_bases;
    using katana::runtime::dreamcast_vram_64bit_alias_count;
    using katana::runtime::dreamcast_vram_64bit_physical_bases;
    using katana::runtime::dreamcast_vram_alias_count;
    using katana::runtime::dreamcast_vram_size;
    using katana::runtime::map_dreamcast_aica_ram;
    using katana::runtime::map_dreamcast_main_ram;
    using katana::runtime::map_dreamcast_vram;

    Memory bus(0u);
    const auto vram = map_dreamcast_vram(bus);
    const auto aica_ram = map_dreamcast_aica_ram(bus);

    require(
        vram->size() == dreamcast_vram_size,
        "Das Dreamcast-VRAM besitzt nicht genau 8 MiB."
    );
    require(
        aica_ram->size() == dreamcast_aica_ram_size,
        "Das Dreamcast-AICA-RAM besitzt nicht genau 2 MiB."
    );
    require(
        bus.region_count() ==
            dreamcast_vram_alias_count + dreamcast_aica_ram_alias_count,
        "Nicht alle VRAM- und AICA-RAM-Aliasfenster wurden registriert."
    );
    require(
        dreamcast_vram_64bit_alias_count == 28u &&
        dreamcast_vram_32bit_alias_count == 28u &&
        dreamcast_aica_ram_alias_count == 28u,
        "Die erwarteten direkten Dreamcast-Aliaszahlen stimmen nicht."
    );
    require(
        vram->read_u8(0u) == 0u &&
        vram->read_u8(static_cast<std::uint32_t>(dreamcast_vram_size - 1u)) == 0u &&
        aica_ram->read_u8(0u) == 0u &&
        aica_ram->read_u8(static_cast<std::uint32_t>(dreamcast_aica_ram_size - 1u)) == 0u,
        "VRAM oder AICA-RAM ist nicht deterministisch nullinitialisiert."
    );

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_64bit_physical_bases) {
            require(
                bus.contains(direct_alias(segment_base, physical_base), dreamcast_vram_size),
                "Ein linearer 64-Bit-VRAM-Alias fehlt."
            );
        }
        for (const auto physical_base : dreamcast_vram_32bit_physical_bases) {
            require(
                bus.contains(direct_alias(segment_base, physical_base), dreamcast_vram_size),
                "Ein interleavter 32-Bit-VRAM-Alias fehlt."
            );
        }
        for (const auto physical_base : dreamcast_aica_ram_physical_bases) {
            require(
                bus.contains(direct_alias(segment_base, physical_base), dreamcast_aica_ram_size),
                "Ein AICA-RAM-Alias fehlt."
            );
        }
    }

    require(
        !bus.contains(0xE0800000u) &&
        !bus.contains(0xE4000000u) &&
        !bus.contains(0xE5000000u),
        "P4 darf weder als AICA-RAM noch als VRAM abgebildet werden."
    );

    constexpr std::uint32_t aica_offset = 0x001ABCDEu;
    bus.write_u8(0xA0E00000u + aica_offset, 0x5Au);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_aica_ram_physical_bases) {
            require(
                bus.read_u8(
                    direct_alias(segment_base, physical_base) + aica_offset
                ) == 0x5Au,
                "Ein AICA-RAM-Alias verwendet nicht dasselbe 2-MiB-Backing."
            );
        }
    }

    bus.write_u32(0x20800100u, 0x89ABCDEFu);
    require(
        bus.read_u32(0xC0E00100u) == 0x89ABCDEFu &&
        aica_ram->read_u8(0x00000100u) == 0xEFu &&
        aica_ram->read_u8(0x00000103u) == 0x89u,
        "Little-Endian-Zugriffe sind zwischen AICA-RAM-Aliasen inkonsistent."
    );

    bus.write_u8(0x00800000u, 0x11u);
    bus.write_u8(0xC0FFFFFFu, 0x22u);
    require(
        bus.read_u8(0x60E00000u) == 0x11u &&
        bus.read_u8(0x20DFFFFFu) == 0x22u,
        "Erstes oder letztes Byte des AICA-RAM-Backings wird nicht gespiegelt."
    );

    constexpr std::uint32_t vram_offset = 0x00123456u;
    bus.write_u8(0x04000000u + vram_offset, 0xA5u);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_64bit_physical_bases) {
            require(
                bus.read_u8(
                    direct_alias(segment_base, physical_base) + vram_offset
                ) == 0xA5u,
                "Ein 64-Bit-VRAM-Alias verwendet nicht dasselbe 8-MiB-Backing."
            );
        }
    }

    bus.write_u32(0x05000000u, 0x11223344u);
    bus.write_u32(0x05400000u, 0x55667788u);
    bus.write_u32(0x05000004u, 0x99AABBCCu);
    require(
        bus.read_u32(0x04000000u) == 0x11223344u &&
        bus.read_u32(0x04000004u) == 0x55667788u &&
        bus.read_u32(0x04000008u) == 0x99AABBCCu,
        "Der 32-Bit-VRAM-Pfad interleavt die beiden 32-Bit-Baenke falsch."
    );

    bus.write_u32(0x04000100u, 0x0BADF00Du);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_32bit_physical_bases) {
            require(
                bus.read_u32(
                    direct_alias(segment_base, physical_base) + 0x00000080u
                ) == 0x0BADF00Du,
                "Ein 32-Bit-VRAM-Alias verwendet nicht dieselbe interleavte Sicht."
            );
        }
    }

    vram->write_u8(
        static_cast<std::uint32_t>(dreamcast_vram_size - 1u),
        0xD3u
    );
    require(
        bus.read_u8(0xC7FFFFFFu) == 0xD3u,
        "Das letzte VRAM-Byte ist ueber den 32-Bit-Pfad nicht erreichbar."
    );

    Memory full_bus(0u);
    static_cast<void>(map_dreamcast_main_ram(full_bus));
    static_cast<void>(map_dreamcast_vram(full_bus));
    static_cast<void>(map_dreamcast_aica_ram(full_bus));
    require(
        full_bus.region_count() ==
            dreamcast_main_ram_alias_count +
            dreamcast_vram_alias_count +
            dreamcast_aica_ram_alias_count,
        "Haupt-RAM, VRAM und AICA-RAM lassen sich nicht gemeinsam abbilden."
    );

    Memory conflicting_vram_bus(0u);
    conflicting_vram_bus.map_region(
        "collision",
        0xA5000000u,
        std::make_shared<LinearMemoryDevice>(16u)
    );
    require(
        throws<std::invalid_argument>([&conflicting_vram_bus] {
            static_cast<void>(map_dreamcast_vram(conflicting_vram_bus));
        }),
        "Kollisionen mit einem VRAM-Alias werden nicht abgelehnt."
    );
    require(
        conflicting_vram_bus.region_count() == 1u &&
        !conflicting_vram_bus.contains(0x04000000u),
        "Eine fehlgeschlagene VRAM-Konfiguration hinterlaesst Teilabbildungen."
    );

    Memory conflicting_aica_bus(0u);
    conflicting_aica_bus.map_region(
        "collision",
        0x60A00000u,
        std::make_shared<LinearMemoryDevice>(16u)
    );
    require(
        throws<std::invalid_argument>([&conflicting_aica_bus] {
            static_cast<void>(map_dreamcast_aica_ram(conflicting_aica_bus));
        }),
        "Kollisionen mit einem AICA-RAM-Alias werden nicht abgelehnt."
    );
    require(
        conflicting_aica_bus.region_count() == 1u &&
        !conflicting_aica_bus.contains(0x00800000u),
        "Eine fehlgeschlagene AICA-Konfiguration hinterlaesst Teilabbildungen."
    );

    std::cout << "Dreamcast-VRAM und AICA-RAM erfolgreich.\n";
    return EXIT_SUCCESS;
}