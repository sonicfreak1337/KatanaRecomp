#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/wait_loop_trace.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::runtime::GuestInstructionOrigin origin(const std::uint32_t source,
                                                const std::uint32_t runtime) {
    return {source, runtime, true};
}

} // namespace

int main() {
    using namespace katana::runtime;

    static_assert(std::is_trivially_copyable_v<GuestMemoryAccessEvent>);
    static_assert(std::is_standard_layout_v<GuestMemoryAccessEvent>);
    static_assert(std::is_trivially_copyable_v<GuestMemoryAccessSink>);
    static_assert(std::is_standard_layout_v<GuestMemoryAccessSink>);

    constexpr std::uint32_t read_site = 0x8C001100u;
    const std::array descriptors = {
        RuntimeWaitLoopDescriptor{
            0x8C0010F0u, 0x8C001108u, read_site, RuntimeWaitLoopEvidence::ProvenGuard}};
    RuntimeWaitLoopTraceRecorder recorder(descriptors);

    CpuState cpu;
    const auto ram = std::make_shared<LinearMemoryDevice>(16u);
    cpu.memory.map_region("wait-primary", 0x0C000000u, ram);
    cpu.memory.map_region("wait-alias", 0x0D000000u, ram);
    cpu.memory.set_guest_memory_access_sink(recorder.sink());
    require(cpu.memory.has_guest_memory_access_sink(),
            "Der separate POD-Zugriffssink wird nicht als aktiv gemeldet.");

    cpu.retired_guest_instructions = 10u;
    require(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000000u) == 0u,
            "Der instrumentierte Gastread liefert nicht den Backingwert.");
    cpu.memory.write_u32(0x0D000000u, 0x11223344u, CodeWriteSource::Dma);
    cpu.retired_guest_instructions = 12u;
    require(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000000u) ==
                0x11223344u,
            "Aliaswrite und instrumentierter Read divergieren.");
    cpu.retired_guest_instructions = 13u;
    static_cast<void>(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000000u));

    require(recorder.value_runs().size() == 2u &&
                recorder.value_runs()[0].value == 0u &&
                recorder.value_runs()[1].value == 0x11223344u &&
                recorder.value_runs()[1].samples == 2u &&
                recorder.value_runs()[1].writer_linked &&
                recorder.writers().size() == 1u &&
                recorder.writers()[0].source == CodeWriteSource::Dma,
            "Wertlaeufe, Aliasprojektion oder DMA-Writerlink sind nicht deterministisch.");

    cpu.retired_guest_instructions = 20u;
    static_cast<void>(
        guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000004u));
    const std::array<std::uint8_t, 4u> store_queue_payload = {
        0xDDu, 0xCCu, 0xBBu, 0xAAu};
    cpu.memory.write_bytes_at(
        0x0D000004u,
        store_queue_payload,
        GuestMemoryAccessContext{
            0x8D000004u, origin(0x8C001200u, 0x1200u), 21u},
        CodeWriteSource::StoreQueue);
    cpu.retired_guest_instructions = 22u;
    require(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000004u) ==
                0xAABBCCDDu &&
                recorder.writers().size() == 2u &&
                recorder.writers()[1].size == 4u &&
                recorder.writers()[1].source == CodeWriteSource::StoreQueue &&
                recorder.writers()[1].instruction.source_pc == 0x8C001200u,
            "Der originbehaftete Range-Write wird nicht genau als Writer korreliert.");

    ram->write_u32(0u, 0x55667788u);
    GuestMemoryAccessEvent pvr_write;
    pvr_write.operation = MemoryAccessOperation::Write;
    pvr_write.access_origin = GuestMemoryAccessOrigin::PvrRender;
    pvr_write.virtual_address = 0x05000000u;
    pvr_write.physical_address = 0x05000000u;
    pvr_write.width = MemoryAccessWidth::Word;
    pvr_write.value = 0x55667788u;
    pvr_write.size = 4u;
    pvr_write.write_source = CodeWriteSource::Fallback;
    pvr_write.scalar_value_valid = true;
    pvr_write.bytes_changed = true;
    pvr_write.linear_backing = ram.get();
    pvr_write.linear_offset = 0u;
    pvr_write.linear_size = 4u;
    pvr_write.linear_contiguous = true;
    pvr_write.linear_byte_offsets = {0u, 1u, 2u, 3u};
    pvr_write.linear_byte_count = 4u;
    cpu.memory.notify_external_guest_memory_access(pvr_write);
    static_cast<void>(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0x8C000000u));
    require(recorder.writers().size() == 3u &&
                recorder.writers().back().access_origin ==
                    GuestMemoryAccessOrigin::PvrRender &&
                recorder.value_runs().back().writer_linked &&
                recorder.value_runs().back().writer_sequence ==
                    recorder.writers().back().sequence,
            "Ein direkter PVR-Backingwrite wird nicht mit dem naechsten Wertlauf verbunden.");

    std::uint32_t mmio_reads = 0u;
    std::uint32_t mmio_value = 0xCAFEBABEu;
    const auto mmio = std::make_shared<MmioMemoryDevice>(
        4u,
        [&mmio_reads, &mmio_value](const std::uint32_t, const MemoryAccessWidth) {
            ++mmio_reads;
            return mmio_value;
        },
        [&mmio_value](const std::uint32_t,
                      const std::uint32_t value,
                      const MemoryAccessWidth) { mmio_value = value; });
    cpu.memory.map_region("wait-mmio", 0x01000000u, mmio);
    const auto run_count_before_mmio = recorder.value_runs().size();
    require(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0xA1000000u) ==
                0xCAFEBABEu &&
                mmio_reads == 1u &&
                recorder.value_runs().size() == run_count_before_mmio + 1u,
            "Der Tracepfad liest MMIO doppelt oder verwirft den bereits gelesenen Wert.");
    guest_write_u32_at(cpu,
                       origin(0x8C001400u, 0x1400u),
                       0xA1000000u,
                       0x13579BDFu);
    require(guest_read_u32_at(cpu, origin(read_site, 0x1000u), 0xA1000000u) ==
                0x13579BDFu &&
                mmio_reads == 2u &&
                recorder.value_runs().size() == run_count_before_mmio + 2u &&
                recorder.value_runs().back().writer_linked &&
                recorder.value_runs().back().writer_link_kind ==
                    RuntimeWaitLoopWriterLinkKind::PhysicalRangeCandidate &&
                recorder.writers().back().instruction.source_pc == 0x8C001400u,
            "Nichtlineare MMIO-Werte oder ihre echten Gastwriter werden nicht korreliert.");

    CpuState vram_cpu;
    const auto vram = map_dreamcast_vram(vram_cpu.memory);
    RuntimeWaitLoopTraceRecorder vram_recorder(descriptors);
    vram_cpu.memory.set_guest_memory_access_sink(vram_recorder.sink());
    static_cast<void>(
        guest_read_u32_at(vram_cpu, origin(read_site, 0x2000u), 0xA4000000u));
    guest_write_u32_at(vram_cpu,
                       origin(0x8C001300u, 0x2100u),
                       0xA5000000u,
                       0xDEADBEEFu);
    require(guest_read_u32_at(vram_cpu, origin(read_site, 0x2000u), 0xA4000000u) ==
                0xDEADBEEFu &&
                vram_recorder.value_runs().size() == 2u &&
                vram_recorder.value_runs().back().writer_linked,
            "64-/32-Bit-VRAM-Aliase teilen keine beweisbare lineare Projektion.");
    std::size_t conservative_observer_writes = 0u;
    vram_cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        if (event.address == 0x05000000u && event.bytes_changed)
            ++conservative_observer_writes;
    });
    const auto writers_before_vram_noops = vram_recorder.writers().size();
    guest_write_u32_at(vram_cpu,
                       origin(0x8C001310u, 0x2110u),
                       0xA5000000u,
                       0xDEADBEEFu);
    const std::array<std::uint8_t, 4u> unchanged_vram_range{
        0xEFu, 0xBEu, 0xADu, 0xDEu};
    vram_cpu.memory.write_bytes_at(
        0x05000000u,
        unchanged_vram_range,
        GuestMemoryAccessContext{
            0xA5000000u, origin(0x8C001312u, 0x2112u), 25u},
        CodeWriteSource::StoreQueue);
    require(vram_recorder.writers().size() == writers_before_vram_noops &&
                conservative_observer_writes == 2u,
            "Produktobserver und Trace trennen konservative VRAM32-Invalidierung "
            "nicht von bytegenauer No-op-Writer-Evidenz.");
    const std::array<const MemoryDevice*, 1u> permitted_vram = {vram.get()};
    require(peek_guest_u32(vram_cpu, 0xA5000000u, permitted_vram) == 0xDEADBEEFu,
            "Der nebenwirkungsfreie Peek lehnt den VRAM32-Wrapper faelschlich ab.");

    RuntimeWaitLoopTraceRecorder bounded(
        descriptors, RuntimeWaitLoopTraceConfig{1u, 1u, 1u});
    CpuState bounded_cpu;
    const auto bounded_ram = std::make_shared<LinearMemoryDevice>(4u);
    bounded_cpu.memory.map_region("bounded", 0x0C000000u, bounded_ram);
    bounded_cpu.memory.set_guest_memory_access_sink(bounded.sink());
    static_cast<void>(
        guest_read_u32_at(bounded_cpu, origin(read_site, 0x3000u), 0x8C000000u));
    bounded_cpu.memory.write_u32(0x0C000000u, 1u, CodeWriteSource::Dma);
    static_cast<void>(
        guest_read_u32_at(bounded_cpu, origin(read_site, 0x3000u), 0x8C000000u));
    static_cast<void>(
        guest_read_u32_at(bounded_cpu, origin(read_site, 0x3000u), 0x8C000000u));
    require(!bounded.complete() && bounded.counters().dropped_value_runs == 1u,
            "Kapazitaetsverlust wird nicht einmalig und sichtbar ausgewiesen.");

    RuntimeWaitLoopTraceRecorder precise_range_writer(descriptors);
    CpuState precise_range_cpu;
    const auto precise_range_ram = std::make_shared<LinearMemoryDevice>(8u);
    precise_range_cpu.memory.map_region("precise-range", 0x0C000000u, precise_range_ram);
    precise_range_ram->write_u32(0u, 0x44332211u);
    precise_range_cpu.memory.set_guest_memory_access_sink(precise_range_writer.sink());
    static_cast<void>(guest_read_u32_at(
        precise_range_cpu, origin(read_site, 0x4000u), 0x8C000000u));
    const std::array<std::uint8_t, 8u> partly_changed_range{
        0x11u, 0x22u, 0x33u, 0x44u, 0xAAu, 0xBBu, 0xCCu, 0xDDu};
    precise_range_cpu.memory.write_bytes_at(
        0x0C000000u,
        partly_changed_range,
        GuestMemoryAccessContext{
            0x8C000000u, origin(0x8C001500u, 0x1500u), 30u},
        CodeWriteSource::StoreQueue);
    require(precise_range_writer.writers().empty(),
            "Ein nur ausserhalb des Pollworts geaenderter Range-Write wird als Writer verknuepft.");

    LinearMemoryProjection malformed_projection;
    malformed_projection.backing = ram.get();
    malformed_projection.byte_count = 5u;
    require(!malformed_projection,
            "Eine uebergrosse lineare Projektion wird als sicher dereferenzierbar akzeptiert.");
    GuestMemoryAccessEvent malformed_event;
    malformed_event.operation = MemoryAccessOperation::Read;
    malformed_event.instruction = origin(read_site, 0x5000u);
    malformed_event.scalar_value_valid = true;
    malformed_event.value = 0xDEADBEEFu;
    malformed_event.size = 4u;
    malformed_event.width = MemoryAccessWidth::Word;
    malformed_event.linear_backing = ram.get();
    malformed_event.linear_byte_count = 5u;
    RuntimeWaitLoopTraceRecorder malformed_recorder(descriptors);
    malformed_recorder.observe(malformed_event);
    require(malformed_recorder.value_runs().empty() &&
                malformed_recorder.counters().invalid_access_events == 1u &&
                !malformed_recorder.complete(),
            "Ein uebergrosses externes Projektionsereignis wird nicht sicher verworfen.");

    constexpr std::size_t oversized_diagnostic_range =
        guest_memory_access_change_tracking_limit + 1u;
    Memory oversized_copy_memory(0u);
    const auto oversized_source =
        std::make_shared<LinearMemoryDevice>(oversized_diagnostic_range);
    const auto oversized_destination =
        std::make_shared<LinearMemoryDevice>(oversized_diagnostic_range);
    oversized_source->write_u8(
        static_cast<std::uint32_t>(oversized_diagnostic_range - 1u), 0x5Au);
    oversized_copy_memory.map_region(
        "oversized-source", 0x02000000u, oversized_source);
    oversized_copy_memory.map_region(
        "oversized-destination", 0x03000000u, oversized_destination);
    RuntimeWaitLoopTraceRecorder oversized_recorder(descriptors);
    oversized_copy_memory.set_guest_memory_access_sink(oversized_recorder.sink());
    oversized_copy_memory.copy_bytes(
        0x03000000u,
        0x02000000u,
        oversized_diagnostic_range,
        CodeWriteSource::Dma);
    require(oversized_destination->read_u8(
                static_cast<std::uint32_t>(oversized_diagnostic_range - 1u)) == 0x5Au &&
                oversized_recorder.counters().invalid_access_events == 1u &&
                !oversized_recorder.complete(),
            "Ein uebergrosser Diagnose-Range darf den Gastwrite nicht abbrechen "
            "und muss den Trace sichtbar unvollstaendig machen.");

    const auto json = recorder.serialize_json();
    require(recorder.complete() &&
                json.find("\"schema\":\"katana.runtime-wait-loop-trace\"") !=
                    std::string::npos &&
                json.find("\"trace_version\":1") != std::string::npos &&
                json.find("\"contains_raw_guest_values\":true") != std::string::npos &&
                json.find("\"writer_scope\":\"since-previous-sample\"") !=
                    std::string::npos &&
                json.find("\"origin\":\"pvr-render\"") != std::string::npos &&
                json.find("\"instruction_valid\":false") != std::string::npos &&
                json.find("\"scalar_value_valid\":false,\"value\":null") !=
                    std::string::npos,
            "Das terminale Trace-v1-JSON verliert Schema, Version oder Writerursprung.");

    cpu.memory.clear_guest_memory_access_sink();
    require(!cpu.memory.has_guest_memory_access_sink(),
            "Der POD-Zugriffssink laesst sich nicht sicher entfernen.");

    std::cout << "Wait-loop runtime trace tests passed\n";
    return EXIT_SUCCESS;
}
