#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/memory.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
    try {
        ExecutableCodeTracker tracker;
        static_cast<void>(
            tracker.register_block({"a", 0x0C001FFCu, 8u, "static-a", {"caller-1", "caller-2"}}));
        static_cast<void>(tracker.register_block({"b", 0x0C002004u, 4u, "static-b", {"caller-2"}}));
        static_cast<void>(tracker.register_block({"c", 0x0C004000u, 4u, "static-c", {}}));

        const auto cpu = tracker.observe_write(0xAC001FFEu, 8u, CodeWriteSource::Cpu);
        require(cpu.invalidated_blocks.size() == 2u && !tracker.valid("a") && !tracker.valid("b") &&
                    tracker.valid("c") && cpu.changed_pages.size() == 2u &&
                    cpu.unlinked_sources.size() == 2u,
                "Aliaswrite invalidiert nicht jeden ueberlappenden Block oder loest Links falsch.");
        require(tracker.page_generation(0x8C001000u) == 1u &&
                    tracker.page_generation(0xAC002000u) == 1u,
                "P1-/P2-Aliase teilen keine Seitengeneration.");
        const auto tracker_snapshot = tracker.snapshot();
        require(tracker_snapshot.page_generations ==
                    std::vector<CodeInvalidationPage>{{0x0C001000u, 1u},
                                                      {0x0C002000u, 1u}} &&
                    tracker_snapshot.invalidation_count == 2u &&
                    tracker_snapshot.next_provenance_sequence == 1u &&
                    tracker_snapshot.blocks.size() == 3u &&
                    !tracker_snapshot.blocks[0u].valid && !tracker_snapshot.blocks[1u].valid,
                "Code-Tracker-Snapshot verliert Seitengeneration, Sequenz oder Blockvaliditaet.");

        Memory observed_memory(0u);
        auto observed_backing = std::make_shared<LinearMemoryDevice>(0x100u);
        observed_memory.map_region("p0", 0x0C000000u, observed_backing);
        observed_memory.map_region("p1", 0x8C000000u, observed_backing);
        observed_memory.map_region("p2", 0xAC000000u, observed_backing);
        observed_backing->write_u16(0x20u, 0x1234u);
        ExecutableCodeTracker observed_tracker;
        for (const auto [name, address, size] : {std::tuple{"cpu", 0x0C000010u, 1u},
                                                 std::tuple{"fpu-identical", 0x0C000020u, 2u},
                                                 std::tuple{"dma", 0x0C000030u, 4u},
                                                 std::tuple{"sq", 0x0C000040u, 32u},
                                                 std::tuple{"copy", 0x0C000070u, 4u},
                                                 std::tuple{"fallback", 0x0C000080u, 1u}}) {
            static_cast<void>(
                observed_tracker.register_block({name, address, size, "write-source", {"caller"}}));
        }
        std::vector<GuestWriteEvent> write_events;
        observed_memory.set_guest_write_observer([&](const auto& event) {
            write_events.push_back(event);
            static_cast<void>(observed_tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
        observed_memory.write_u8(0x8C000010u, 0xA5u, CodeWriteSource::Cpu);
        observed_memory.write_u16(0xAC000020u, 0x1234u, CodeWriteSource::Fpu);
        observed_memory.write_u32(0x0C000030u, 0x44332211u, CodeWriteSource::Dma);
        std::array<std::uint8_t, 32u> sq_bytes{};
        for (std::size_t index = 0u; index < sq_bytes.size(); ++index)
            sq_bytes[index] = static_cast<std::uint8_t>(index);
        observed_memory.write_bytes(0x8C000040u, sq_bytes, CodeWriteSource::StoreQueue);
        const std::array<std::uint8_t, 4u> copy_bytes{0x11u, 0x22u, 0x33u, 0x44u};
        observed_memory.write_bytes(0xAC000070u, copy_bytes, CodeWriteSource::Copy);
        observed_memory.write_u8(0x0C000080u, 0x5Au, CodeWriteSource::Fallback);
        const std::array expected_sources{CodeWriteSource::Cpu,
                                          CodeWriteSource::Fpu,
                                          CodeWriteSource::Dma,
                                          CodeWriteSource::StoreQueue,
                                          CodeWriteSource::Copy,
                                          CodeWriteSource::Fallback};
        require(write_events.size() == expected_sources.size(),
                "Gastwrites werden nicht genau einmal an der Commitgrenze gemeldet.");
        for (std::size_t index = 0u; index < expected_sources.size(); ++index)
            require(write_events[index].source == expected_sources[index],
                    "Eine Gastwrite-Herkunft ging an der Commitgrenze verloren.");
        require(write_events[0].size == 1u && write_events[1].size == 2u &&
                    write_events[2].size == 4u && write_events[3].size == 32u &&
                    write_events[4].size == 4u && write_events[5].size == 1u &&
                    !write_events[1].bytes_changed && observed_tracker.valid("fpu-identical") &&
                    !observed_tracker.valid("cpu") && !observed_tracker.valid("dma") &&
                    !observed_tracker.valid("sq") && !observed_tracker.valid("copy") &&
                    !observed_tracker.valid("fallback"),
                "Breite, Bytevergleich oder Invalidierung einer Gastwrite-Quelle ist falsch.");

        Memory atomic_memory(0u);
        auto writable_prefix = std::make_shared<LinearMemoryDevice>(4u);
        auto read_only_suffix = std::make_shared<LinearMemoryDevice>(4u);
        writable_prefix->write_u8(2u, 0xA1u);
        writable_prefix->write_u8(3u, 0xB2u);
        read_only_suffix->write_u8(0u, 0xC3u);
        read_only_suffix->write_u8(1u, 0xD4u);
        atomic_memory.map_region("atomic-prefix", 0x0C010000u, writable_prefix);
        atomic_memory.map_region(
            "atomic-read-only", 0x0C010004u, read_only_suffix, MemoryRegionAccess::ReadOnly);
        ExecutableCodeTracker atomic_tracker;
        static_cast<void>(atomic_tracker.register_block(
            {"atomic-code", 0x0C010002u, 2u, "write-bytes-preflight", {"caller"}}));
        std::vector<GuestWriteEvent> atomic_events;
        atomic_memory.set_guest_write_observer([&](const auto& event) {
            atomic_events.push_back(event);
            static_cast<void>(atomic_tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
        const std::array<std::uint8_t, 4u> crossing_write{0x11u, 0x22u, 0x33u, 0x44u};
        require(
            throws<MemoryAccessError>([&] {
                atomic_memory.write_bytes(0x0C010002u, crossing_write, CodeWriteSource::StoreQueue);
            }) &&
                writable_prefix->read_u8(2u) == 0xA1u && writable_prefix->read_u8(3u) == 0xB2u &&
                read_only_suffix->read_u8(0u) == 0xC3u && read_only_suffix->read_u8(1u) == 0xD4u &&
                atomic_events.empty() && atomic_tracker.valid("atomic-code") &&
                atomic_tracker.page_generation(0x0C010002u) == 0u,
            "Fehlgeschlagener gebuendelter Write veraendert ein Praefix oder invalidiert "
            "nicht atomar.");

        Memory write_only_memory(0u);
        std::vector<std::uint8_t> mmio_bytes;
        auto write_only_mmio = std::make_shared<MmioMemoryDevice>(
            4u,
            MmioReadHandler{},
            [&](const std::uint32_t offset,
                const std::uint32_t value,
                const MemoryAccessWidth width) {
                require(width == MemoryAccessWidth::Byte && offset == mmio_bytes.size(),
                        "Gebündelter MMIO-Write verlor Bytebreite oder Reihenfolge.");
                mmio_bytes.push_back(static_cast<std::uint8_t>(value));
            });
        write_only_memory.map_region("write-only-mmio", 0x0C020000u, write_only_mmio);
        std::vector<GuestWriteEvent> mmio_events;
        write_only_memory.set_guest_write_observer(
            [&](const auto& event) { mmio_events.push_back(event); });
        const std::array<std::uint8_t, 4u> mmio_write{0x10u, 0x20u, 0x30u, 0x40u};
        write_only_memory.write_bytes(0x0C020000u, mmio_write, CodeWriteSource::Fallback);
        require(mmio_bytes == std::vector<std::uint8_t>(mmio_write.begin(), mmio_write.end()) &&
                    mmio_events.size() == 1u && mmio_events.front().address == 0x0C020000u &&
                    mmio_events.front().size == mmio_write.size() &&
                    mmio_events.front().source == CodeWriteSource::Fallback &&
                    mmio_events.front().bytes_changed,
                "Write-only-MMIO wird vorab gelesen oder nicht pessimistisch invalidiert.");

        const auto identical = tracker.observe_write(0x8C004000u, 4u, CodeWriteSource::Dma, false);
        require(identical.byte_identical && tracker.valid("c") && identical.changed_pages.empty(),
                "Nachweislich bytegleicher DMA-Write invalidiert Code.");
        const auto copy = tracker.observe_write(0x8C004000u, 4u, CodeWriteSource::Copy);
        require(copy.source == CodeWriteSource::Copy && !tracker.valid("c") &&
                    tracker.hotspots().at(0x0C004000u) == 1u && tracker.invalidation_count() == 3u,
                "Copy-Pfad oder Hotspotdiagnose umgeht die Invalidierung.");

        ExecutableCodeTracker repeated;
        require(
            repeated.register_block({"fallback-op", 0x0C008000u, 4u, "runtime-op", {"caller"}}) ==
                    BlockRegistrationResult::Inserted &&
                repeated.register_block(
                    {"fallback-op", 0xAC008000u, 4u, "runtime-op", {"caller"}}) ==
                    BlockRegistrationResult::AlreadyValid &&
                repeated.block_count() == 1u && repeated.incoming_link_count("fallback-op") == 1u,
            "Idempotente Blockregistrierung dupliziert Block oder eingehende Links.");
        static_cast<void>(repeated.observe_write(0x8C008000u, 1u, CodeWriteSource::Cpu));
        require(
            repeated.register_block({"fallback-op", 0x0C008000u, 4u, "runtime-op", {"caller"}}) ==
                    BlockRegistrationResult::Reactivated &&
                repeated.valid("fallback-op") && repeated.block_count() == 1u &&
                repeated.invalidation_count() == 1u,
            "Invalidierter Block wird nicht zaehlerstabil reaktiviert.");
        require(throws<std::invalid_argument>([&] {
                    static_cast<void>(repeated.register_block(
                        {"fallback-op", 0x0C009000u, 4u, "runtime-op", {}}));
                }) &&
                    throws<std::invalid_argument>([&] {
                        static_cast<void>(repeated.register_block(
                            {"fallback-op", 0x0C008000u, 8u, "runtime-op", {}}));
                    }),
                "Gleiche Blockidentitaet darf Adresse oder Groesse nicht still wechseln.");

        ExecutableCodeTracker indexed(2u);
        ExecutableCodeTracker reference(2u);
        for (auto* candidate : {&indexed, &reference}) {
            static_cast<void>(
                candidate->register_block({"near", 0x0C010000u, 8u, "benchmark", {"near-caller"}}));
            static_cast<void>(
                candidate->register_block({"far", 0x0C100000u, 8u, "benchmark", {"far-caller"}}));
        }
        reference.set_lookup_mode(CodeInvalidationLookupMode::ReferenceScan);
        const auto indexed_result = indexed.observe_write(0x8C010004u, 1u, CodeWriteSource::Cpu);
        const auto reference_result =
            reference.observe_write(0x8C010004u, 1u, CodeWriteSource::Cpu);
        require(indexed_result.invalidated_blocks == reference_result.invalidated_blocks &&
                    indexed_result.unlinked_sources == reference_result.unlinked_sources &&
                    indexed.performance_counters().indexed_candidates == 1u &&
                    reference.performance_counters().reference_candidates == 2u,
                "Page-to-Block-Index und Referenzscan divergieren oder sind nicht messbar.");
        for (std::uint32_t address = 0x0C200000u; address < 0x0C204000u; address += 0x1000u) {
            static_cast<void>(indexed.observe_write(address, 1u, CodeWriteSource::Dma, false));
        }
        require(indexed.invalidation_events().size() == indexed.provenance_capacity() &&
                    indexed.dropped_provenance_events() >= 3u,
                "Invalidierungsprovenienz waechst ueber ihre feste Kapazitaet hinaus.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
