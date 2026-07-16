#include "katana/runtime/memory.hpp"

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

template <typename Function>
bool has_memory_error(Function&& function,
                      const katana::runtime::MemoryAccessErrorReason reason,
                      const katana::runtime::MemoryAccessOperation operation,
                      const std::uint32_t address,
                      const katana::runtime::MemoryAccessWidth width,
                      const std::string& region_name = {}) {
    try {
        function();
    } catch (const katana::runtime::MemoryAccessError& error) {
        return error.reason() == reason && error.operation() == operation &&
               error.address() == address && error.width() == width &&
               error.region_name() == region_name &&
               std::string(error.what()).find("0x") != std::string::npos;
    }
    return false;
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
    using katana::runtime::MemoryAccessErrorReason;
    using katana::runtime::MemoryAccessEvent;
    using katana::runtime::MemoryAccessOperation;
    using katana::runtime::MemoryAccessWidth;
    using katana::runtime::MemoryAlignmentPolicy;
    using katana::runtime::MemoryRegionAccess;
    using katana::runtime::MemoryWatchpointAccess;

    Memory strict_bus(0u);
    const auto strict_ram = std::make_shared<LinearMemoryDevice>(16u);
    strict_bus.map_region("strict-ram", 0x00001000u, strict_ram);

    require(strict_bus.alignment_policy() == MemoryAlignmentPolicy::Strict,
            "Der Speicherbus verwendet nicht standardmaessig strikte Ausrichtung.");
    require(has_memory_error([&strict_bus] { static_cast<void>(strict_bus.read_u16(0x00001001u)); },
                             MemoryAccessErrorReason::Misaligned,
                             MemoryAccessOperation::Read,
                             0x00001001u,
                             MemoryAccessWidth::Halfword),
            "Ein ungerader Halfword-Read liefert keinen typisierten Ausrichtungsfehler.");
    require(has_memory_error([&strict_bus] { strict_bus.write_u32(0x00001002u, 0xFFFFFFFFu); },
                             MemoryAccessErrorReason::Misaligned,
                             MemoryAccessOperation::Write,
                             0x00001002u,
                             MemoryAccessWidth::Word) &&
                strict_ram->read_u8(2u) == 0u,
            "Ein fehlplatzierter Word-Write wird nicht vor dem Geraetezugriff abgelehnt.");

    Memory permissive_bus(8u, MemoryAlignmentPolicy::Permissive);
    permissive_bus.write_u32(1u, 0x89ABCDEFu);
    require(permissive_bus.read_u32(1u) == 0x89ABCDEFu && permissive_bus.read_u8(1u) == 0xEFu &&
                permissive_bus.read_u8(4u) == 0x89u,
            "Der explizit permissive Modus behaelt unaligned Little Endian nicht bei.");
    permissive_bus.set_alignment_policy(MemoryAlignmentPolicy::Strict);
    require(has_memory_error([&permissive_bus] { static_cast<void>(permissive_bus.read_u32(1u)); },
                             MemoryAccessErrorReason::Misaligned,
                             MemoryAccessOperation::Read,
                             1u,
                             MemoryAccessWidth::Word),
            "Die Ausrichtungsrichtlinie laesst sich nicht sichtbar umschalten.");

    require(has_memory_error([&strict_bus] { static_cast<void>(strict_bus.read_u8(0x00003000u)); },
                             MemoryAccessErrorReason::Unmapped,
                             MemoryAccessOperation::Read,
                             0x00003000u,
                             MemoryAccessWidth::Byte),
            "Nicht zugeordnete Adressen liefern keinen typisierten Fehler.");

    Memory cross_region_bus(0u);
    cross_region_bus.map_region(
        "short-region", 0x00002000u, std::make_shared<LinearMemoryDevice>(6u));
    require(has_memory_error(
                [&cross_region_bus] { static_cast<void>(cross_region_bus.read_u32(0x00002004u)); },
                MemoryAccessErrorReason::CrossRegion,
                MemoryAccessOperation::Read,
                0x00002004u,
                MemoryAccessWidth::Word,
                "short-region"),
            "Regionsueberschreitende Zugriffe verlieren Region und Fehlergrund.");

    Memory read_only_bus(0u);
    read_only_bus.map_region("boot-rom",
                             0x00004000u,
                             std::make_shared<LinearMemoryDevice>(8u),
                             MemoryRegionAccess::ReadOnly);
    require(has_memory_error([&read_only_bus] { read_only_bus.write_u16(0x00004000u, 0x1234u); },
                             MemoryAccessErrorReason::ReadOnly,
                             MemoryAccessOperation::Write,
                             0x00004000u,
                             MemoryAccessWidth::Halfword,
                             "boot-rom"),
            "Read-only-Zugriffe liefern keinen strukturierten Regionsfehler.");

    Memory overflow_bus(0u, MemoryAlignmentPolicy::Permissive);
    require(
        has_memory_error([&overflow_bus] { static_cast<void>(overflow_bus.read_u16(0xFFFFFFFFu)); },
                         MemoryAccessErrorReason::AddressOverflow,
                         MemoryAccessOperation::Read,
                         0xFFFFFFFFu,
                         MemoryAccessWidth::Halfword),
        "Adressraumueberlauf wird nicht von ungemappten Adressen unterschieden.");

    Memory observed_bus(0u);
    const auto observed_ram = std::make_shared<LinearMemoryDevice>(16u);
    observed_bus.map_region("observed-primary", 0x00005000u, observed_ram);
    observed_bus.map_region("observed-alias", 0x00006000u, observed_ram);

    std::vector<MemoryAccessEvent> trace_events;
    std::vector<MemoryAccessEvent> read_watch_events;
    std::vector<MemoryAccessEvent> write_watch_events;

    observed_bus.set_trace_handler(
        [&trace_events](const MemoryAccessEvent& event) { trace_events.push_back(event); });
    require(observed_bus.has_trace_handler(),
            "Der globale Trace-Handler wird nicht als aktiv gemeldet.");

    const auto read_watchpoint =
        observed_bus.add_watchpoint(0x00005002u,
                                    4u,
                                    MemoryWatchpointAccess::Read,
                                    [&read_watch_events](const MemoryAccessEvent& event) {
                                        read_watch_events.push_back(event);
                                    });
    const auto write_watchpoint =
        observed_bus.add_watchpoint(0x00006002u,
                                    2u,
                                    MemoryWatchpointAccess::Write,
                                    [&write_watch_events](const MemoryAccessEvent& event) {
                                        write_watch_events.push_back(event);
                                    });
    require(read_watchpoint != write_watchpoint && observed_bus.watchpoint_count() == 2u,
            "Watchpoint-IDs oder Zaehler sind nicht deterministisch.");

    observed_bus.write_u32(0x00005000u, 0x11223344u);
    require(trace_events.size() == 1u &&
                trace_events[0].operation == MemoryAccessOperation::Write &&
                trace_events[0].address == 0x00005000u &&
                trace_events[0].width == MemoryAccessWidth::Word &&
                trace_events[0].value == 0x11223344u &&
                trace_events[0].region_name == "observed-primary" && read_watch_events.empty() &&
                write_watch_events.empty(),
            "Trace oder zugriffsgefilterte Watchpoints melden einen Write falsch.");

    require(observed_bus.read_u32(0x00005000u) == 0x11223344u,
            "Der beobachtete Read liefert einen falschen Wert.");
    require(trace_events.size() == 2u && read_watch_events.size() == 1u &&
                read_watch_events[0].operation == MemoryAccessOperation::Read &&
                read_watch_events[0].address == 0x00005000u &&
                read_watch_events[0].width == MemoryAccessWidth::Word &&
                read_watch_events[0].value == 0x11223344u &&
                read_watch_events[0].region_name == "observed-primary",
            "Ein ueberlappender Read-Watchpoint erhaelt kein vollstaendiges Ereignis.");

    observed_bus.write_u16(0x00006002u, 0xBEEFu);
    require(trace_events.size() == 3u && write_watch_events.size() == 1u &&
                write_watch_events[0].operation == MemoryAccessOperation::Write &&
                write_watch_events[0].address == 0x00006002u &&
                write_watch_events[0].width == MemoryAccessWidth::Halfword &&
                write_watch_events[0].value == 0xBEEFu &&
                write_watch_events[0].region_name == "observed-alias" &&
                observed_ram->read_u8(2u) == 0xEFu && observed_ram->read_u8(3u) == 0xBEu,
            "Alias-Watchpoint oder erfolgreicher Write wird falsch gemeldet.");

    const auto trace_count_before_failure = trace_events.size();
    const auto read_count_before_failure = read_watch_events.size();
    require(
        has_memory_error([&observed_bus] { static_cast<void>(observed_bus.read_u16(0x00005001u)); },
                         MemoryAccessErrorReason::Misaligned,
                         MemoryAccessOperation::Read,
                         0x00005001u,
                         MemoryAccessWidth::Halfword) &&
            trace_events.size() == trace_count_before_failure &&
            read_watch_events.size() == read_count_before_failure,
        "Fehlgeschlagene Zugriffe duerfen Trace und Watchpoints nicht ausloesen.");

    require(observed_bus.remove_watchpoint(read_watchpoint) &&
                !observed_bus.remove_watchpoint(read_watchpoint) &&
                observed_bus.watchpoint_count() == 1u,
            "Watchpoints lassen sich nicht eindeutig entfernen.");
    static_cast<void>(observed_bus.read_u32(0x00005000u));
    require(read_watch_events.size() == 1u, "Ein entfernter Watchpoint wird weiterhin aufgerufen.");

    observed_bus.clear_watchpoints();
    observed_bus.clear_trace_handler();
    require(observed_bus.watchpoint_count() == 0u && !observed_bus.has_trace_handler(),
            "Watchpoints oder Trace-Handler lassen sich nicht vollstaendig leeren.");

    require(throws<std::invalid_argument>([&observed_bus] {
                static_cast<void>(observed_bus.add_watchpoint(0x00005000u,
                                                              0u,
                                                              MemoryWatchpointAccess::ReadWrite,
                                                              [](const MemoryAccessEvent&) {}));
            }) &&
                throws<std::invalid_argument>([&observed_bus] {
                    static_cast<void>(observed_bus.add_watchpoint(0xFFFFFFFFu,
                                                                  2u,
                                                                  MemoryWatchpointAccess::ReadWrite,
                                                                  [](const MemoryAccessEvent&) {}));
                }) &&
                throws<std::invalid_argument>([&observed_bus] {
                    static_cast<void>(
                        observed_bus.add_watchpoint(0x00005000u,
                                                    1u,
                                                    MemoryWatchpointAccess::ReadWrite,
                                                    katana::runtime::MemoryAccessObserver{}));
                }),
            "Ungueltige Watchpoint-Konfigurationen werden akzeptiert.");

    std::cout << "Ausrichtung, Speicherfehler und Watchpoints erfolgreich.\n";
    return EXIT_SUCCESS;
}
