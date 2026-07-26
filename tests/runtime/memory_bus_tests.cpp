#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

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

void count_guest_memory_access(void* const context,
                               const katana::runtime::GuestMemoryAccessEvent&) noexcept {
    ++*static_cast<std::size_t*>(context);
}

constexpr std::string_view fill_observer_terminate_child =
    "--fill-observer-terminate-child";
constexpr int fill_observer_terminate_exit_code = 86;

int run_fill_observer_terminate_child() {
    std::set_terminate([] { std::_Exit(fill_observer_terminate_exit_code); });
    katana::runtime::Memory memory(0u);
    const auto backing = std::make_shared<katana::runtime::LinearMemoryDevice>(4u);
    memory.map_region("fill-terminate", 0x00200000u, backing);
    memory.set_guest_write_observer([](const katana::runtime::GuestWriteEvent&) {
        throw std::runtime_error("expected fill observer failure");
    });
    static_cast<void>(memory.commit_prevalidated_linear_fill(
        0x00200000u, 4u, 0x7Eu, katana::runtime::CodeWriteSource::Cpu));
    return 87;
}

} // namespace

int main(const int argc, const char* const* argv) {
    if (argc == 2 && std::string_view(argv[1]) == fill_observer_terminate_child)
        return run_fill_observer_terminate_child();

    using katana::runtime::LinearMemoryDevice;
    using katana::runtime::Memory;
    using katana::runtime::MemoryAccessError;
    using katana::runtime::MemoryAccessErrorReason;
    using katana::runtime::MemoryLookupMode;
    using katana::runtime::MemoryRegionAccess;

    require(argc > 0 && argv[0] != nullptr,
            "Testprozess besitzt keinen Pfad fuer den Fail-stop-Kindprozess.");
    const auto executable = std::filesystem::absolute(argv[0]).string();
    const auto terminate_command =
        '"' + executable + "\" " + std::string(fill_observer_terminate_child);
    const auto terminate_status = std::system(terminate_command.c_str());
#ifdef _WIN32
    require(terminate_status == fill_observer_terminate_exit_code,
            "Observer-Ausnahme beendet atomaren Fill nicht mit eindeutigem Fail-stop-Code.");
#else
    require(terminate_status != -1 && WIFEXITED(terminate_status) &&
                WEXITSTATUS(terminate_status) == fill_observer_terminate_exit_code,
            "Observer-Ausnahme beendet atomaren Fill nicht mit eindeutigem Fail-stop-Code.");
#endif

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

    std::uint64_t diagnostic_mmio_reads = 0u;
    const auto diagnostic_mmio = std::make_shared<katana::runtime::MmioMemoryDevice>(
        4u,
        [&](const auto, const auto) {
            ++diagnostic_mmio_reads;
            return 0xDEADBEEFu;
        },
        [](const auto, const auto, const auto) {});
    bus.map_region("diagnostic-mmio", 0x00003000u, diagnostic_mmio);
    const std::array<const katana::runtime::MemoryDevice*, 2u> permitted{
        work_ram.get(), diagnostic_mmio.get()};
    std::uint64_t diagnostic_trace_calls = 0u;
    std::uint64_t diagnostic_watchpoint_calls = 0u;
    bus.set_trace_handler([&](const auto&) { ++diagnostic_trace_calls; });
    const auto diagnostic_watchpoint = bus.add_watchpoint(
        0x00001004u,
        4u,
        katana::runtime::MemoryWatchpointAccess::Read,
        [&](const auto&) { ++diagnostic_watchpoint_calls; });
    bus.set_mmio_access_tracking(true);
    require(bus.read_u32(0x00003000u) == 0xDEADBEEFu,
            "MMIO-Sentinel fuer die Peek-Invarianz konnte nicht gesetzt werden.");
    const auto last_mmio_before_peek = bus.last_mmio_access();
    const auto diagnostic_mmio_reads_before_peek = diagnostic_mmio_reads;
    diagnostic_trace_calls = 0u;
    diagnostic_watchpoint_calls = 0u;
    const auto indexed_counters_before_peek = bus.performance_counters();
    require(bus.peek_u32(0x00001004u, permitted) == 0x89ABCDEFu &&
                throws<katana::runtime::MemoryAccessError>([&] {
                    static_cast<void>(bus.peek_u32(0x00003000u, permitted));
                }) &&
                diagnostic_mmio_reads == diagnostic_mmio_reads_before_peek,
            "Diagnostischer Peek liest ein absichtlich erlaubtes MMIO-Geraet oder umgeht seine "
            "lineare Geraete-Whitelist.");
    const auto indexed_counters_after_peek = bus.performance_counters();
    require(indexed_counters_before_peek.indexed_region_hits ==
                    indexed_counters_after_peek.indexed_region_hits &&
                indexed_counters_before_peek.reference_region_probes ==
                    indexed_counters_after_peek.reference_region_probes &&
                indexed_counters_before_peek.unobserved_accesses ==
                    indexed_counters_after_peek.unobserved_accesses &&
                indexed_counters_before_peek.observed_accesses ==
                    indexed_counters_after_peek.observed_accesses,
            "Diagnostischer Indexed-Peek veraendert Speicherleistungszaehler.");
    const auto last_mmio_after_indexed_peek = bus.last_mmio_access();
    require(last_mmio_before_peek && last_mmio_after_indexed_peek &&
                last_mmio_after_indexed_peek->operation == last_mmio_before_peek->operation &&
                last_mmio_after_indexed_peek->address == last_mmio_before_peek->address &&
                last_mmio_after_indexed_peek->width == last_mmio_before_peek->width &&
                last_mmio_after_indexed_peek->value == last_mmio_before_peek->value &&
                last_mmio_after_indexed_peek->region_name == last_mmio_before_peek->region_name &&
                diagnostic_trace_calls == 0u && diagnostic_watchpoint_calls == 0u,
            "Diagnostischer Peek veraendert MMIO-Sentinel, Trace oder Watchpointzustand.");
    bus.set_lookup_mode(MemoryLookupMode::Reference);
    const auto reference_counters_before_peek = bus.performance_counters();
    require(bus.peek_u32(0x00001004u, permitted) == 0x89ABCDEFu,
            "Diagnostischer Reference-Peek liefert andere Gastbytes.");
    const auto reference_counters_after_peek = bus.performance_counters();
    require(reference_counters_before_peek.indexed_region_hits ==
                    reference_counters_after_peek.indexed_region_hits &&
                reference_counters_before_peek.reference_region_probes ==
                    reference_counters_after_peek.reference_region_probes &&
                reference_counters_before_peek.unobserved_accesses ==
                    reference_counters_after_peek.unobserved_accesses &&
                reference_counters_before_peek.observed_accesses ==
                    reference_counters_after_peek.observed_accesses,
            "Diagnostischer Reference-Peek veraendert Speicherleistungszaehler.");
    bus.set_lookup_mode(MemoryLookupMode::Indexed);
    require(diagnostic_trace_calls == 0u && diagnostic_watchpoint_calls == 0u,
            "Reference-Peek ruft Trace oder Watchpoint auf.");
    static_cast<void>(bus.remove_watchpoint(diagnostic_watchpoint));
    bus.clear_trace_handler();
    bus.set_mmio_access_tracking(false);

    katana::runtime::CpuState diagnostic_cpu;
    const auto mmu_peek_ram = std::make_shared<LinearMemoryDevice>(4096u);
    diagnostic_cpu.memory.map_region("mmu-peek-ram", 0x01000000u, mmu_peek_ram);
    diagnostic_cpu.address_space = std::make_shared<katana::runtime::RuntimeAddressSpace>();
    diagnostic_cpu.address_space->set_mode(katana::runtime::AddressTranslationMode::Mmu);
    katana::runtime::TlbMapping diagnostic_mapping;
    diagnostic_mapping.virtual_page = 0x00400000u;
    diagnostic_mapping.physical_page = 0x01000000u;
    diagnostic_mapping.slot = 3u;
    diagnostic_cpu.address_space->ldtlb(diagnostic_mapping);
    mmu_peek_ram->write_u32(0x40u, 0x13579BDFu);
    diagnostic_cpu.pc = 0x8C001234u;
    diagnostic_cpu.tea = 0x2468ACE0u;
    const auto diagnostic_pc_before = diagnostic_cpu.pc;
    const auto diagnostic_tea_before = diagnostic_cpu.tea;
    const auto diagnostic_exception_before = diagnostic_cpu.last_exception_cause;
    const auto diagnostic_counters_before = diagnostic_cpu.memory.performance_counters();
    const std::array<const katana::runtime::MemoryDevice*, 1u> mmu_permitted{
        mmu_peek_ram.get()};
    require(katana::runtime::peek_guest_u32(
                diagnostic_cpu, 0x00400040u, mmu_permitted) == 0x13579BDFu &&
                diagnostic_cpu.pc == diagnostic_pc_before &&
                diagnostic_cpu.tea == diagnostic_tea_before &&
                diagnostic_cpu.last_exception_cause == diagnostic_exception_before,
            "MMU-bewusster Gast-Peek liefert falsche Bytes oder mutiert den CPU-Zustand.");
    const auto diagnostic_counters_after = diagnostic_cpu.memory.performance_counters();
    require(diagnostic_counters_before.indexed_region_hits ==
                    diagnostic_counters_after.indexed_region_hits &&
                diagnostic_counters_before.reference_region_probes ==
                    diagnostic_counters_after.reference_region_probes &&
                diagnostic_counters_before.unobserved_accesses ==
                    diagnostic_counters_after.unobserved_accesses &&
                diagnostic_counters_before.observed_accesses ==
                    diagnostic_counters_after.observed_accesses,
            "MMU-bewusster Gast-Peek mutiert Speicherleistungszaehler.");
    const auto diagnostic_peek_has_error =
        [&](const std::uint32_t address, const MemoryAccessErrorReason reason) {
            try {
                static_cast<void>(
                    katana::runtime::peek_guest_u32(diagnostic_cpu, address, mmu_permitted));
            } catch (const MemoryAccessError& error) {
                return error.reason() == reason &&
                       error.operation() == katana::runtime::MemoryAccessOperation::Read &&
                       error.address() == address &&
                       error.width() == katana::runtime::MemoryAccessWidth::Word;
            }
            return false;
        };
    const auto counters_before_failed_peeks = diagnostic_cpu.memory.performance_counters();
    require(diagnostic_peek_has_error(0x00500040u, MemoryAccessErrorReason::TlbMiss) &&
                diagnostic_cpu.pc == diagnostic_pc_before &&
                diagnostic_cpu.tea == diagnostic_tea_before &&
                diagnostic_cpu.last_exception_cause == diagnostic_exception_before,
            "Diagnostischer TLB-Miss mutiert den Gastzustand oder verliert den Fehlergrund.");
    auto protected_mapping = diagnostic_mapping;
    protected_mapping.virtual_page = 0x00600000u;
    protected_mapping.slot = 4u;
    protected_mapping.readable = false;
    diagnostic_cpu.address_space->ldtlb(protected_mapping);
    require(diagnostic_peek_has_error(0x00600040u, MemoryAccessErrorReason::TlbProtection) &&
                diagnostic_cpu.pc == diagnostic_pc_before &&
                diagnostic_cpu.tea == diagnostic_tea_before &&
                diagnostic_cpu.last_exception_cause == diagnostic_exception_before,
            "Diagnostischer TLB-Schutzfehler mutiert den Gastzustand oder wird verschluckt.");
    auto duplicate_mapping = diagnostic_mapping;
    duplicate_mapping.slot = 5u;
    diagnostic_cpu.address_space->ldtlb(duplicate_mapping);
    require(diagnostic_peek_has_error(0x00400040u, MemoryAccessErrorReason::TlbMultipleHit) &&
                diagnostic_cpu.pc == diagnostic_pc_before &&
                diagnostic_cpu.tea == diagnostic_tea_before &&
                diagnostic_cpu.last_exception_cause == diagnostic_exception_before,
            "Diagnostischer TLB-Multiple-Hit mutiert den Gastzustand oder wird verschluckt.");
    const auto counters_after_failed_peeks = diagnostic_cpu.memory.performance_counters();
    require(counters_before_failed_peeks.indexed_region_hits ==
                    counters_after_failed_peeks.indexed_region_hits &&
                counters_before_failed_peeks.reference_region_probes ==
                    counters_after_failed_peeks.reference_region_probes &&
                counters_before_failed_peeks.unobserved_accesses ==
                    counters_after_failed_peeks.unobserved_accesses &&
                counters_before_failed_peeks.observed_accesses ==
                    counters_after_failed_peeks.observed_accesses,
            "Fehlgeschlagene Diagnose-Peeks mutieren Speicherleistungszaehler.");

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
        0x00110000u, 4u, katana::runtime::MemoryWatchpointAccess::Read, [](const auto&) {});
    static_cast<void>(indexed.read_u32(0x00110000u));
    require(indexed.performance_counters().observed_accesses == 1u,
            "Watchpoint umgeht den beobachteten Speicherpfad.");
    static_cast<void>(indexed.remove_watchpoint(watchpoint));

    for (std::uint32_t index = 0u; index < 32u; ++index)
        indexed_ram->write_u8(0x100u + index, static_cast<std::uint8_t>(0x40u + index));
    indexed.reset_performance_counters();
    indexed.copy_bytes(0x00100200u, 0x00100100u, 32u);
    require(indexed.read_u8(0x00100200u) == 0x40u && indexed.read_u8(0x0010021Fu) == 0x5Fu &&
                indexed.performance_counters().unobserved_accesses == 66u,
            "Linearer DMA-Kopierfastpath verliert Bytes oder faellt auf Einzelwrites zurueck.");
    require(throws<katana::runtime::MemoryAccessError>(
                [&bus] { bus.copy_bytes(0x00001008u, 0x00001000u, 16u); }),
            "Regionsuebergreifender DMA-Kopierpfad wurde teilweise ausgefuehrt.");

    {
        constexpr std::uint32_t fill_base = 0x00200000u;
        Memory fill_memory(0u);
        const auto fill_backing = std::make_shared<LinearMemoryDevice>(16u);
        const auto adjacent_backing = std::make_shared<LinearMemoryDevice>(16u);
        const auto read_only_backing = std::make_shared<LinearMemoryDevice>(8u);
        std::size_t mmio_write_calls = 0u;
        const auto nonlinear_mmio = std::make_shared<katana::runtime::MmioMemoryDevice>(
            8u,
            [](const auto, const auto) { return 0u; },
            [&](const auto, const auto, const auto) { ++mmio_write_calls; });
        fill_memory.map_region("fill", fill_base, fill_backing);
        fill_memory.map_region("fill-adjacent", fill_base + 16u, adjacent_backing);
        fill_memory.map_region("fill-read-only",
                               fill_base + 0x10000u,
                               read_only_backing,
                               MemoryRegionAccess::ReadOnly);
        fill_memory.map_region("fill-mmio", fill_base + 0x20000u, nonlinear_mmio);

        constexpr std::uint8_t fill_value = 0x7Eu;
        constexpr std::array initial{
            fill_value,
            std::uint8_t{0x11u},
            std::uint8_t{0x22u},
            fill_value,
            fill_value,
            std::uint8_t{0x33u},
            fill_value,
            std::uint8_t{0x44u},
            std::uint8_t{0x55u},
            fill_value,
            fill_value,
            std::uint8_t{0x66u},
        };
        for (std::size_t index = 0u; index < initial.size(); ++index)
            fill_backing->write_u8(static_cast<std::uint32_t>(index), initial[index]);

        std::vector<katana::runtime::GuestWriteEvent> fill_events;
        fill_events.reserve(8u);
        fill_memory.set_guest_write_observer(
            [&](const katana::runtime::GuestWriteEvent& event) { fill_events.push_back(event); });
        fill_memory.reset_performance_counters();
        constexpr std::size_t synthetic_limit_reads = 13u;
        constexpr std::size_t synthetic_indexed_hits = initial.size() * 2u + 1u;
        require(fill_memory.commit_prevalidated_linear_fill(
                    fill_base,
                    initial.size(),
                    fill_value,
                    katana::runtime::CodeWriteSource::Dma,
                    synthetic_limit_reads,
                    synthetic_indexed_hits),
                "Vorvalidierter linearer Mixed-Fill wurde abgelehnt.");

        constexpr std::array<std::uint32_t, 8u> expected_run_offsets{
            0u, 1u, 3u, 5u, 6u, 7u, 9u, 11u};
        constexpr std::array<std::size_t, 8u> expected_run_sizes{1u, 2u, 2u, 1u, 1u, 2u, 2u, 1u};
        constexpr std::array<bool, 8u> expected_run_changes{
            false, true, false, true, false, true, false, true};
        require(fill_events.size() == expected_run_offsets.size(),
                "Mixed-Fill meldet nicht jeden geaenderten/identischen Run.");
        for (std::size_t index = 0u; index < fill_events.size(); ++index) {
            require(fill_events[index].address == fill_base + expected_run_offsets[index] &&
                        fill_events[index].size == expected_run_sizes[index] &&
                        fill_events[index].source == katana::runtime::CodeWriteSource::Dma &&
                        fill_events[index].bytes_changed == expected_run_changes[index],
                    "Mixed-Fill verliert Runadresse, -groesse, Herkunft oder Aenderungsstatus.");
        }
        require(
            std::all_of(fill_backing->bytes().begin(),
                        fill_backing->bytes().begin() + static_cast<std::ptrdiff_t>(initial.size()),
                        [](const auto byte) { return byte == fill_value; }) &&
                fill_memory.performance_counters().unobserved_accesses ==
                    initial.size() + synthetic_limit_reads &&
                fill_memory.performance_counters().indexed_region_hits ==
                    synthetic_indexed_hits &&
                fill_memory.performance_counters().observed_accesses == 0u,
            "Mixed-Fill ist nicht atomar sichtbar oder zaehlt synthetische Zugriffe falsch.");

        fill_memory.reset_performance_counters();
        require(fill_memory.account_prevalidated_unobserved_accesses(
                    std::numeric_limits<std::uint64_t>::max(),
                    std::numeric_limits<std::uint64_t>::max()) &&
                    fill_memory.account_prevalidated_unobserved_accesses(2u, 3u) &&
                    fill_memory.performance_counters().unobserved_accesses == 1u &&
                    fill_memory.performance_counters().indexed_region_hits == 2u,
                "Vorvalidierte Zugriffszaehler verlieren das skalare uint64-Wraparound.");

        fill_events.clear();
        fill_memory.reset_performance_counters();
        require(fill_memory.commit_prevalidated_linear_fill(
                    fill_base + 2u, 6u, fill_value, katana::runtime::CodeWriteSource::Fallback) &&
                    fill_events.size() == 1u && fill_events.front().address == fill_base + 2u &&
                    fill_events.front().size == 6u &&
                    fill_events.front().source == katana::runtime::CodeWriteSource::Fallback &&
                    !fill_events.front().bytes_changed &&
                    fill_memory.performance_counters().unobserved_accesses == 6u &&
                    fill_memory.performance_counters().observed_accesses == 0u,
                "Bytegleicher Fill wird nicht als einzelner identischer Run gemeldet.");

        for (const auto lookup_mode :
             {katana::runtime::MemoryLookupMode::Indexed,
              katana::runtime::MemoryLookupMode::Reference}) {
            fill_memory.set_lookup_mode(lookup_mode);
            fill_memory.reset_performance_counters();
            require(fill_memory.maps_device(
                        fill_base, initial.size(), fill_backing.get(), false) &&
                        fill_memory.is_readable_linear_range(fill_base, initial.size(), false) &&
                        fill_memory.is_writable_linear_range(fill_base, initial.size(), false) &&
                        fill_memory.performance_counters().indexed_region_hits == 0u &&
                        fill_memory.performance_counters().reference_region_probes == 0u,
                    "Metrikneutraler Linear-RAM-Proof veraendert Lookup-Zaehler.");
        }
        fill_memory.set_lookup_mode(katana::runtime::MemoryLookupMode::Reference);
        fill_memory.reset_performance_counters();
        require(!fill_memory.account_prevalidated_unobserved_accesses(1u, 1u) &&
                    fill_memory.performance_counters().indexed_region_hits == 0u &&
                    fill_memory.performance_counters().reference_region_probes == 0u &&
                    fill_memory.performance_counters().unobserved_accesses == 0u,
                "Reference-Lookup akzeptiert vorvalidierte Indexed-Zugriffszaehler.");
        const auto before_reference_synthetic =
            std::vector<std::uint8_t>(fill_backing->bytes().begin(), fill_backing->bytes().end());
        require(!fill_memory.commit_prevalidated_linear_fill(
                    fill_base,
                    initial.size(),
                    std::uint8_t{0xA5u},
                    katana::runtime::CodeWriteSource::Cpu,
                    0u,
                    1u) &&
                    std::equal(before_reference_synthetic.begin(),
                               before_reference_synthetic.end(),
                               fill_backing->bytes().begin()) &&
                    fill_memory.performance_counters().indexed_region_hits == 0u &&
                    fill_memory.performance_counters().reference_region_probes == 0u &&
                    fill_memory.performance_counters().unobserved_accesses == 0u,
                "Reference-Lookup akzeptiert synthetische Indexed-Hits oder mutiert Zustand.");
        fill_memory.set_lookup_mode(katana::runtime::MemoryLookupMode::Indexed);

        const auto require_rejected_without_mutation = [&](const char* const message) {
            const std::vector<std::uint8_t> before(fill_backing->bytes().begin(),
                                                   fill_backing->bytes().end());
            const auto counters_before = fill_memory.performance_counters();
            const auto events_before = fill_events.size();
            require(!fill_memory.commit_prevalidated_linear_fill(
                        fill_base + 4u,
                        4u,
                        std::uint8_t{0xA5u},
                        katana::runtime::CodeWriteSource::Cpu) &&
                        std::equal(before.begin(), before.end(), fill_backing->bytes().begin()) &&
                        fill_memory.performance_counters().indexed_region_hits ==
                            counters_before.indexed_region_hits &&
                        fill_memory.performance_counters().reference_region_probes ==
                            counters_before.reference_region_probes &&
                        fill_memory.performance_counters().unobserved_accesses ==
                            counters_before.unobserved_accesses &&
                        fill_memory.performance_counters().observed_accesses ==
                            counters_before.observed_accesses &&
                        fill_events.size() == events_before,
                    message);
        };

        fill_memory.set_trace_handler([](const auto&) {});
        require_rejected_without_mutation("Aktiver Trace liess einen vorvalidierten Fill durch.");
        fill_memory.clear_trace_handler();

        const auto fill_watchpoint = fill_memory.add_watchpoint(
            fill_base + 15u, 1u, katana::runtime::MemoryWatchpointAccess::Read, [](const auto&) {});
        require_rejected_without_mutation(
            "Aktiver Watchpoint liess einen vorvalidierten Fill durch.");
        static_cast<void>(fill_memory.remove_watchpoint(fill_watchpoint));

        fill_memory.set_mmio_trace_handler([](const auto&) {});
        require_rejected_without_mutation(
            "Aktiver MMIO-Trace liess einen vorvalidierten Fill durch.");
        fill_memory.clear_mmio_trace_handler();

        fill_memory.set_mmio_access_tracking(true);
        require_rejected_without_mutation(
            "Aktives MMIO-Tracking liess einen vorvalidierten Fill durch.");
        fill_memory.set_mmio_access_tracking(false);

        std::size_t guest_sink_calls = 0u;
        fill_memory.set_guest_memory_access_sink({&guest_sink_calls, &count_guest_memory_access});
        require_rejected_without_mutation(
            "Aktiver Gast-Speichersink liess einen vorvalidierten Fill durch.");
        require(guest_sink_calls == 0u, "Abgelehnter Fill benachrichtigte den Gast-Speichersink.");
        fill_memory.clear_guest_memory_access_sink();

        fill_events.clear();
        fill_memory.reset_performance_counters();
        const auto fill_before_invalid_ranges =
            std::vector<std::uint8_t>(fill_backing->bytes().begin(), fill_backing->bytes().end());
        const auto adjacent_before_invalid_ranges = std::vector<std::uint8_t>(
            adjacent_backing->bytes().begin(), adjacent_backing->bytes().end());
        require(!fill_memory.commit_prevalidated_linear_fill(fill_base + 15u, 2u, fill_value) &&
                    !fill_memory.commit_prevalidated_linear_fill(
                        fill_base + 0x10000u, 1u, fill_value) &&
                    !fill_memory.commit_prevalidated_linear_fill(
                        fill_base + 0x20000u, 1u, fill_value) &&
                    !fill_memory.commit_prevalidated_linear_fill(0xFFFFFFFFu, 2u, fill_value) &&
                    !fill_memory.commit_prevalidated_linear_fill(fill_base, 0u, fill_value) &&
                    std::equal(fill_before_invalid_ranges.begin(),
                               fill_before_invalid_ranges.end(),
                               fill_backing->bytes().begin()) &&
                    std::equal(adjacent_before_invalid_ranges.begin(),
                               adjacent_before_invalid_ranges.end(),
                               adjacent_backing->bytes().begin()) &&
                    mmio_write_calls == 0u && fill_events.empty() &&
                    fill_memory.performance_counters().unobserved_accesses == 0u &&
                    fill_memory.performance_counters().observed_accesses == 0u,
                "Cross-Region/Read-only/MMIO/Overflow/Leer-Fill mutiert Speicher, "
                "Observer oder Zaehler.");
    }

    std::cout << "Regionbasierter Speicherbus erfolgreich.\n";
    return EXIT_SUCCESS;
}
