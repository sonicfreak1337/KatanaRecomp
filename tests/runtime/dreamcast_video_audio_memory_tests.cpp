#include "katana/runtime/dreamcast_memory.hpp"

#include <array>
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

std::uint32_t expected_vram_32bit_linear_offset(const std::uint32_t offset) {
    constexpr std::uint32_t bytes_per_word = 4u;

    const auto bank = offset < katana::runtime::dreamcast_vram_bank_size ? 0u : 1u;
    const auto bank_offset = offset - bank * katana::runtime::dreamcast_vram_bank_size;
    const auto word_in_bank = bank_offset / bytes_per_word;
    const auto byte_in_word = bank_offset % bytes_per_word;

    return (word_in_bank * 2u + bank) * bytes_per_word + byte_in_word;
}

[[noreturn]] void fail_vram_offset(const std::string& message,
                                   const std::uint32_t offset,
                                   const std::uint32_t actual,
                                   const std::uint32_t expected) {
    std::cerr << "TEST FEHLGESCHLAGEN: " << message << " Offset=" << offset << " Ist=" << actual
              << " Soll=" << expected << '\n';
    std::exit(EXIT_FAILURE);
}

void verify_vram_offset_mapping_exhaustively() {
    std::vector<std::uint8_t> seen(katana::runtime::dreamcast_vram_size, 0u);

    for (std::size_t raw_offset = 0u; raw_offset < katana::runtime::dreamcast_vram_size;
         ++raw_offset) {
        const auto offset = static_cast<std::uint32_t>(raw_offset);
        const auto actual = katana::runtime::dreamcast_vram_32bit_to_linear_offset(offset);
        const auto expected = expected_vram_32bit_linear_offset(offset);

        if (actual != expected || actual >= katana::runtime::dreamcast_vram_size) {
            fail_vram_offset(
                "Die exhaustive 32-Bit-VRAM-Abbildung ist falsch.", offset, actual, expected);
        }

        if (seen[actual] != 0u) {
            fail_vram_offset(
                "Die 32-Bit-VRAM-Abbildung ist nicht bijektiv.", offset, actual, expected);
        }
        seen[actual] = 1u;
    }
}

void verify_vram_bus_boundaries(katana::runtime::Memory& bus,
                                const std::shared_ptr<katana::runtime::LinearMemoryDevice>& vram) {
    constexpr std::uint32_t path_32bit_base = 0x05000000u;
    constexpr std::array<std::uint32_t, 6> word_indices = {
        0x00000000u, 0x00000001u, 0x00000002u, 0x00012345u, 0x0007FFFEu, 0x000FFFFFu};

    for (std::uint32_t bank = 0u; bank < 2u; ++bank) {
        for (const auto word_index : word_indices) {
            for (std::uint32_t byte_index = 0u; byte_index < 4u; ++byte_index) {
                const auto path_offset =
                    bank * katana::runtime::dreamcast_vram_bank_size + word_index * 4u + byte_index;
                const auto linear_offset = expected_vram_32bit_linear_offset(path_offset);
                const auto value = static_cast<std::uint8_t>(
                    (bank * 0x71u + word_index * 0x13u + byte_index * 0x29u) & 0xFFu);

                bus.write_u8(path_32bit_base + path_offset, value);
                if (vram->read_u8(linear_offset) != value) {
                    fail_vram_offset("Byteposition oder Bank des 32-Bit-VRAM-Pfads ist falsch.",
                                     path_offset,
                                     vram->read_u8(linear_offset),
                                     value);
                }
            }
        }
    }

    bus.write_u16(path_32bit_base + 0x003FFFFEu, 0xA1B2u);
    require(vram->read_u16(expected_vram_32bit_linear_offset(0x003FFFFEu)) == 0xA1B2u,
            "Der Halfword-Zugriff direkt vor dem VRAM-Bankwechsel ist falsch.");

    bus.write_u16(path_32bit_base + 0x00400000u, 0xC3D4u);
    require(vram->read_u16(expected_vram_32bit_linear_offset(0x00400000u)) == 0xC3D4u,
            "Der Halfword-Zugriff direkt nach dem VRAM-Bankwechsel ist falsch.");

    bus.write_u16(path_32bit_base + 0x00400002u, 0xE5F6u);
    require(vram->read_u16(expected_vram_32bit_linear_offset(0x00400002u)) == 0xE5F6u,
            "Die zweite Halfword-Position des ersten Worts in Bank 1 ist falsch.");

    bus.write_u32(path_32bit_base + 0x003FFFFCu, 0x11223344u);
    require(vram->read_u32(expected_vram_32bit_linear_offset(0x003FFFFCu)) == 0x11223344u,
            "Das letzte 32-Bit-Wort der ersten VRAM-Bank ist falsch.");

    bus.write_u32(path_32bit_base + 0x00400000u, 0x55667788u);
    require(vram->read_u32(expected_vram_32bit_linear_offset(0x00400000u)) == 0x55667788u,
            "Das erste 32-Bit-Wort der zweiten VRAM-Bank ist falsch.");

    constexpr std::array<std::uint32_t, 5> sequence_starts = {
        0x00000000u, 0x00000100u, 0x00012340u, 0x0007FFF0u, 0x000FFFF0u};

    for (std::uint32_t bank = 0u; bank < 2u; ++bank) {
        for (const auto sequence_start : sequence_starts) {
            for (std::uint32_t index = 0u; index < 16u; ++index) {
                const auto word_index = sequence_start + index;
                const auto path_offset =
                    bank * katana::runtime::dreamcast_vram_bank_size + word_index * 4u;
                const auto value = 0x80000000u ^ (bank << 28u) ^ (word_index * 0x00001021u);

                bus.write_u32(path_32bit_base + path_offset, value);
                require(vram->read_u32(expected_vram_32bit_linear_offset(path_offset)) == value,
                        "Fortlaufende 32-Bit-Woerter unterscheiden sich zwischen interleavter und "
                        "linearer VRAM-Sicht.");
            }
        }
    }

    constexpr std::array<std::uint32_t, 12> alias_offsets = {0x00000000u,
                                                             0x00000001u,
                                                             0x00000002u,
                                                             0x00000003u,
                                                             0x003FFFFCu,
                                                             0x003FFFFDu,
                                                             0x003FFFFEu,
                                                             0x003FFFFFu,
                                                             0x00400000u,
                                                             0x00400001u,
                                                             0x00400002u,
                                                             0x007FFFFFu};

    for (const auto path_offset : alias_offsets) {
        const auto value =
            static_cast<std::uint8_t>((path_offset ^ (path_offset >> 8u) ^ 0xA5u) & 0xFFu);
        bus.write_u8(path_32bit_base + path_offset, value);

        for (const auto segment_base : katana::runtime::dreamcast_direct_segment_bases) {
            for (const auto physical_base : katana::runtime::dreamcast_vram_32bit_physical_bases) {
                require(bus.read_u8(direct_alias(segment_base, physical_base) + path_offset) ==
                            value,
                        "Ein 32-Bit-VRAM-Alias weicht an einer Bank- oder Bytegrenze ab.");
            }
        }
    }
}

} // namespace

int main() {
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
    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::map_dreamcast_aica_ram;
    using katana::runtime::map_dreamcast_main_ram;
    using katana::runtime::map_dreamcast_vram;
    using katana::runtime::Memory;

    Memory bus(0u);
    const auto vram = map_dreamcast_vram(bus);
    const auto aica_ram = map_dreamcast_aica_ram(bus);

    require(vram->size() == dreamcast_vram_size, "Das Dreamcast-VRAM besitzt nicht genau 8 MiB.");
    require(aica_ram->size() == dreamcast_aica_ram_size,
            "Das Dreamcast-AICA-RAM besitzt nicht genau 2 MiB.");
    require(bus.region_count() == dreamcast_vram_alias_count + dreamcast_aica_ram_alias_count,
            "Nicht alle VRAM- und AICA-RAM-Aliasfenster wurden registriert.");
    static_assert(dreamcast_vram_64bit_alias_count == 28u);
    static_assert(dreamcast_vram_32bit_alias_count == 28u);
    static_assert(dreamcast_aica_ram_alias_count == 28u);
    require(vram->read_u8(0u) == 0u &&
                vram->read_u8(static_cast<std::uint32_t>(dreamcast_vram_size - 1u)) == 0u &&
                aica_ram->read_u8(0u) == 0u &&
                aica_ram->read_u8(static_cast<std::uint32_t>(dreamcast_aica_ram_size - 1u)) == 0u,
            "VRAM oder AICA-RAM ist nicht deterministisch nullinitialisiert.");

    verify_vram_offset_mapping_exhaustively();
    verify_vram_bus_boundaries(bus, vram);

    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_64bit_physical_bases) {
            require(bus.contains(direct_alias(segment_base, physical_base), dreamcast_vram_size),
                    "Ein linearer 64-Bit-VRAM-Alias fehlt.");
        }
        for (const auto physical_base : dreamcast_vram_32bit_physical_bases) {
            require(bus.contains(direct_alias(segment_base, physical_base), dreamcast_vram_size),
                    "Ein interleavter 32-Bit-VRAM-Alias fehlt.");
        }
        for (const auto physical_base : dreamcast_aica_ram_physical_bases) {
            require(
                bus.contains(direct_alias(segment_base, physical_base), dreamcast_aica_ram_size),
                "Ein AICA-RAM-Alias fehlt.");
        }
    }

    require(!bus.contains(0xE0800000u) && !bus.contains(0xE4000000u) && !bus.contains(0xE5000000u),
            "P4 darf weder als AICA-RAM noch als VRAM abgebildet werden.");

    constexpr std::uint32_t aica_offset = 0x001ABCDEu;
    bus.write_u8(0xA0E00000u + aica_offset, 0x5Au);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_aica_ram_physical_bases) {
            require(bus.read_u8(direct_alias(segment_base, physical_base) + aica_offset) == 0x5Au,
                    "Ein AICA-RAM-Alias verwendet nicht dasselbe 2-MiB-Backing.");
        }
    }

    bus.write_u32(0x20800100u, 0x89ABCDEFu);
    require(bus.read_u32(0xC0E00100u) == 0x89ABCDEFu && aica_ram->read_u8(0x00000100u) == 0xEFu &&
                aica_ram->read_u8(0x00000103u) == 0x89u,
            "Little-Endian-Zugriffe sind zwischen AICA-RAM-Aliasen inkonsistent.");

    bus.write_u8(0x00800000u, 0x11u);
    bus.write_u8(0xC0FFFFFFu, 0x22u);
    require(bus.read_u8(0x60E00000u) == 0x11u && bus.read_u8(0x20DFFFFFu) == 0x22u,
            "Erstes oder letztes Byte des AICA-RAM-Backings wird nicht gespiegelt.");

    constexpr std::uint32_t vram_offset = 0x00123456u;
    bus.write_u8(0x04000000u + vram_offset, 0xA5u);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_64bit_physical_bases) {
            require(bus.read_u8(direct_alias(segment_base, physical_base) + vram_offset) == 0xA5u,
                    "Ein 64-Bit-VRAM-Alias verwendet nicht dasselbe 8-MiB-Backing.");
        }
    }

    bus.write_u32(0x05000000u, 0x11223344u);
    bus.write_u32(0x05400000u, 0x55667788u);
    bus.write_u32(0x05000004u, 0x99AABBCCu);
    require(bus.read_u32(0x04000000u) == 0x11223344u && bus.read_u32(0x04000004u) == 0x55667788u &&
                bus.read_u32(0x04000008u) == 0x99AABBCCu,
            "Der 32-Bit-VRAM-Pfad interleavt die beiden 32-Bit-Baenke falsch.");

    bus.write_u32(0x04000100u, 0x0BADF00Du);
    for (const auto segment_base : dreamcast_direct_segment_bases) {
        for (const auto physical_base : dreamcast_vram_32bit_physical_bases) {
            require(bus.read_u32(direct_alias(segment_base, physical_base) + 0x00000080u) ==
                        0x0BADF00Du,
                    "Ein 32-Bit-VRAM-Alias verwendet nicht dieselbe interleavte Sicht.");
        }
    }

    vram->write_u8(static_cast<std::uint32_t>(dreamcast_vram_size - 1u), 0xD3u);
    require(bus.read_u8(0xC7FFFFFFu) == 0xD3u,
            "Das letzte VRAM-Byte ist ueber den 32-Bit-Pfad nicht erreichbar.");

    Memory full_bus(0u);
    static_cast<void>(map_dreamcast_main_ram(full_bus));
    static_cast<void>(map_dreamcast_vram(full_bus));
    static_cast<void>(map_dreamcast_aica_ram(full_bus));
    require(full_bus.region_count() == dreamcast_main_ram_alias_count + dreamcast_vram_alias_count +
                                           dreamcast_aica_ram_alias_count,
            "Haupt-RAM, VRAM und AICA-RAM lassen sich nicht gemeinsam abbilden.");

    Memory conflicting_vram_bus(0u);
    conflicting_vram_bus.map_region(
        "collision", 0xA5000000u, std::make_shared<LinearMemoryDevice>(16u));
    require(throws<std::invalid_argument>([&conflicting_vram_bus] {
                static_cast<void>(map_dreamcast_vram(conflicting_vram_bus));
            }),
            "Kollisionen mit einem VRAM-Alias werden nicht abgelehnt.");
    require(conflicting_vram_bus.region_count() == 1u &&
                !conflicting_vram_bus.contains(0x04000000u),
            "Eine fehlgeschlagene VRAM-Konfiguration hinterlaesst Teilabbildungen.");

    Memory conflicting_aica_bus(0u);
    conflicting_aica_bus.map_region(
        "collision", 0x60A00000u, std::make_shared<LinearMemoryDevice>(16u));
    require(throws<std::invalid_argument>([&conflicting_aica_bus] {
                static_cast<void>(map_dreamcast_aica_ram(conflicting_aica_bus));
            }),
            "Kollisionen mit einem AICA-RAM-Alias werden nicht abgelehnt.");
    require(conflicting_aica_bus.region_count() == 1u &&
                !conflicting_aica_bus.contains(0x00800000u),
            "Eine fehlgeschlagene AICA-Konfiguration hinterlaesst Teilabbildungen.");

    std::cout << "Dreamcast-VRAM und AICA-RAM erfolgreich.\n";
    return EXIT_SUCCESS;
}
