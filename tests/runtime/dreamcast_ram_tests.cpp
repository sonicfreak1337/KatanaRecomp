#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/runtime.hpp"

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

template <typename Exception, typename Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

std::uint32_t alias_base(const std::uint32_t area_base, const std::size_t mirror_index) {
    return area_base +
           static_cast<std::uint32_t>(mirror_index * katana::runtime::dreamcast_main_ram_size);
}

} // namespace

int main() {
    using katana::runtime::dreamcast_main_ram_alias_count;
    using katana::runtime::dreamcast_main_ram_area_bases;
    using katana::runtime::dreamcast_main_ram_mirrors_per_area;
    using katana::runtime::dreamcast_main_ram_size;
    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::map_dreamcast_main_ram;
    using katana::runtime::Memory;

    Memory bus(0u);
    const auto main_ram = map_dreamcast_main_ram(bus);
    const auto direct_read_guard = bus.direct_linear_memory_guard(false);

    require(main_ram->size() == dreamcast_main_ram_size,
            "Das Dreamcast-Haupt-RAM besitzt nicht genau 16 MiB.");
    require(direct_read_guard &&
                bus.direct_linear_memory_guard_current(direct_read_guard, false),
            "Das vollstaendig abgebildete Haupt-RAM besitzt keinen direkten Leseguard.");
    require(bus.region_count() == dreamcast_main_ram_alias_count,
            "Nicht alle Dreamcast-RAM-Aliasfenster wurden registriert.");
    require(main_ram->read_u8(0u) == 0u &&
                main_ram->read_u8(static_cast<std::uint32_t>(dreamcast_main_ram_size - 1u)) == 0u,
            "Dreamcast-Haupt-RAM ist nicht deterministisch nullinitialisiert.");

    for (const auto area_base : dreamcast_main_ram_area_bases) {
        for (std::size_t mirror_index = 0u; mirror_index < dreamcast_main_ram_mirrors_per_area;
             ++mirror_index) {
            const auto base = alias_base(area_base, mirror_index);
            require(bus.contains(base, dreamcast_main_ram_size),
                    "Ein Dreamcast-RAM-Aliasfenster fehlt.");
        }
    }

    require(bus.region(0u).base_address == 0x0C000000u &&
                bus.region(bus.region_count() - 1u).base_address == 0xCF000000u,
            "Dreamcast-RAM-Aliase sind nicht deterministisch sortiert.");
    require(!bus.contains(0xEC000000u),
            "Der P4-Bereich darf nicht als Haupt-RAM gespiegelt werden.");

    constexpr std::uint32_t test_offset = 0x00123456u;
    bus.write_u8(0x0C000000u + test_offset, 0x5Au);

    for (const auto area_base : dreamcast_main_ram_area_bases) {
        for (std::size_t mirror_index = 0u; mirror_index < dreamcast_main_ram_mirrors_per_area;
             ++mirror_index) {
            require(bus.read_u8(alias_base(area_base, mirror_index) + test_offset) == 0x5Au,
                    "Ein RAM-Alias verwendet nicht dasselbe 16-MiB-Backing.");
        }
    }

    bus.write_u8(0xAF000000u + test_offset, 0xA5u);
    require(bus.read_u8(0x0C000000u + test_offset) == 0xA5u &&
                main_ram->read_u8(test_offset) == 0xA5u,
            "Schreiben ueber den ungecachten P2-Alias erreicht das Backing nicht.");

    bus.write_u32(0x8E000100u, 0x89ABCDEFu);
    require(bus.read_u32(0x2F000100u) == 0x89ABCDEFu && main_ram->read_u8(0x00000100u) == 0xEFu &&
                main_ram->read_u8(0x00000103u) == 0x89u,
            "Little-Endian-Mehrbytezugriffe sind zwischen RAM-Aliasen inkonsistent.");

    bus.write_u8(0x4C000000u, 0x11u);
    bus.write_u8(0x6FFFFFFFu, 0x22u);
    require(bus.read_u8(0xCC000000u) == 0x11u && bus.read_u8(0x8FFFFFFFu) == 0x22u,
            "Erstes oder letztes Byte des RAM-Backings wird nicht gespiegelt.");

    bus.reset_performance_counters();
    std::uint32_t direct_value = 0u;
    require(katana::runtime::direct_linear_guard_read_u32(
                direct_read_guard, 0x8E000100u, direct_value) &&
                direct_value == 0x89ABCDEFu,
            "Der gecachte Direktguard liest die gespiegelten Haupt-RAM-Bytes nicht.");
    const auto direct_read_counters = bus.performance_counters();
    require(direct_read_counters.indexed_region_hits == 1u &&
                direct_read_counters.unobserved_accesses == 1u &&
                direct_read_counters.reference_region_probes == 0u &&
                direct_read_counters.observed_accesses == 0u,
            "Ein erfolgreicher Guard-Read zaehlt nicht exakt einen unbeobachteten "
            "Indexed-Zugriff.");

    bus.map_region(
        "guard-generation-probe", 0x01000000u, std::make_shared<LinearMemoryDevice>(16u));
    const auto mapping_counters_before = bus.performance_counters();
    std::uint32_t stale_mapping_value = 0xDEADBEEFu;
    require(!direct_read_guard &&
                !katana::runtime::direct_linear_guard_read_u32(
                    direct_read_guard, 0x8E000100u, stale_mapping_value) &&
                stale_mapping_value == 0xDEADBEEFu &&
                bus.performance_counters().indexed_region_hits ==
                    mapping_counters_before.indexed_region_hits &&
                bus.performance_counters().reference_region_probes ==
                    mapping_counters_before.reference_region_probes &&
                bus.performance_counters().unobserved_accesses ==
                    mapping_counters_before.unobserved_accesses &&
                bus.performance_counters().observed_accesses ==
                    mapping_counters_before.observed_accesses,
            "Eine Mappingaenderung laesst einen alten Direktguard lesen oder zaehlen.");
    const auto refreshed_read_guard = bus.direct_linear_memory_guard(false);
    require(refreshed_read_guard &&
                bus.direct_linear_memory_guard_current(refreshed_read_guard, false),
            "Der direkte RAM-Guard kann nach einer sicheren Mappingaenderung nicht "
            "erneuert werden.");

    katana::runtime::CpuState cpu;
    cpu.memory = Memory(0u);
    static_cast<void>(map_dreamcast_main_ram(cpu.memory));
    cpu.write_sr(katana::runtime::sr_md_mask);
    std::size_t observed_writes = 0u;
    bool last_write_changed = false;
    cpu.memory.set_guest_write_observer(
        [&](const katana::runtime::GuestWriteEvent& event) noexcept {
            ++observed_writes;
            last_write_changed = event.bytes_changed;
        },
        katana::runtime::GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
    katana::runtime::guest_write_u32(cpu, 0x8C000200u, 0x12345678u);
    require(katana::runtime::guest_read_u32(cpu, 0xAF000200u) == 0x12345678u,
            "Der direkte privilegierte P1/P2-Haupt-RAM-Pfad ist inkonsistent.");
    require(observed_writes == 1u && last_write_changed,
            "Der direkte Haupt-RAM-Store verliert den stabilen Write-Observer.");
    katana::runtime::guest_write_u32(cpu, 0xAC000200u, 0x12345678u);
    require(observed_writes == 2u && !last_write_changed,
            "Der direkte Haupt-RAM-Store meldet bytes_changed nicht exakt.");

    bool traced = false;
    bus.set_trace_handler([&traced](const katana::runtime::MemoryAccessEvent&) {
        traced = true;
    });
    const auto trace_counters_before = bus.performance_counters();
    std::uint8_t stale_trace_value = 0xA5u;
    require(!refreshed_read_guard &&
                !bus.direct_linear_memory_guard_current(refreshed_read_guard, false) &&
                !katana::runtime::direct_linear_guard_read_u8(
                    refreshed_read_guard, 0x8C000000u, stale_trace_value) &&
                stale_trace_value == 0xA5u &&
                bus.performance_counters().indexed_region_hits ==
                    trace_counters_before.indexed_region_hits &&
                bus.performance_counters().reference_region_probes ==
                    trace_counters_before.reference_region_probes &&
                bus.performance_counters().unobserved_accesses ==
                    trace_counters_before.unobserved_accesses &&
                bus.performance_counters().observed_accesses ==
                    trace_counters_before.observed_accesses,
            "Ein Diagnoseobserver invalidiert den direkten RAM-Guard nicht.");
    static_cast<void>(bus.read_u8(0x0C000000u));
    require(traced, "Der RAM-Fallback verliert den Diagnoseobserver.");

    Memory conflicting_bus(0u);
    conflicting_bus.map_region("collision", 0x8C000000u, std::make_shared<LinearMemoryDevice>(16u));

    require(throws<std::invalid_argument>(
                [&conflicting_bus] { static_cast<void>(map_dreamcast_main_ram(conflicting_bus)); }),
            "Kollisionen mit einem Dreamcast-RAM-Alias werden nicht abgelehnt.");
    require(conflicting_bus.region_count() == 1u && !conflicting_bus.contains(0x0C000000u),
            "Eine fehlgeschlagene RAM-Konfiguration hinterlaesst Teilabbildungen.");

    std::cout << "Dreamcast-Haupt-RAM und Spiegelungen erfolgreich.\n";
    return EXIT_SUCCESS;
}
