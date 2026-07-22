#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/crash_report.hpp"
#include "katana/runtime/dispatch_diagnostics.hpp"
#include "katana/runtime/firmware_handoff.hpp"
#include "katana/runtime/runtime_provenance.hpp"
#include "katana/runtime/trace.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

katana::runtime::DispatchDiagnosticEvent dispatch_event() {
    using namespace katana::runtime;
    DispatchDiagnosticEvent event;
    event.callsite = 0x8C001000u;
    event.source_virtual = 0x8C001000u;
    event.source_physical = 0x0C001000u;
    event.virtual_target = 0xAC002000u;
    event.canonical_target = 0x0C002000u;
    event.pr = 0x8C001004u;
    event.block_end = BlockEndKind::DynamicBranch;
    event.origin = DispatchResolutionOrigin::TableLookup;
    event.alias_origin = DispatchAliasOrigin::CanonicalPhysical;
    event.fallback_reason = DispatchFallbackReason::DynamicCode;
    event.fallback_action = DispatchFallbackAction::Interpreter;
    event.guest_instructions = 3u;
    event.exit_pc = 0xAC002002u;
    event.error = DispatchDiagnosticError::None;
    return event;
}

} // namespace

int main() {
    using namespace katana::runtime;

    CpuState cpu;
    cpu.pc = 0x8C020002u;
    cpu.spc = 0x8C020000u;
    cpu.pr = 0x8C010004u;
    cpu.r[3] = 0x12345678u;
    cpu.exception_in_delay_slot = true;
    cpu.last_exception_cause = ExceptionCause::AddressErrorRead;
    CrashReportContext crash_context;
    crash_context.stop_code = "memory-fault";
    crash_context.block_address = BlockAddress{0x8C020000u, 0x8C020000u};
    crash_context.block_variant = BlockVariantKey{1u, 2u, 3u, 4u, 5u};
    crash_context.block_provenance = "main-image";
    crash_context.scheduler_cycle = 77u;
    crash_context.scheduler_pending_events = 2u;
    crash_context.dispatch_callsite = 0x8C010000u;
    crash_context.dispatch_target = 0x8C020000u;
    crash_context.dispatch_origin = "table-lookup";
    crash_context.dispatch_action = "interpreter";
    const auto crash = capture_crash_report(cpu, crash_context);
    const auto crash_json = serialize_crash_report(crash);
    require(crash.context.delay_slot_owner_pc == cpu.spc &&
                crash.context.block_address->physical_address == 0x0C020000u &&
                crash_json.find("\"delay_slot_owner_pc\":\"0x8C020000\"") != std::string::npos &&
                crash_json.find("\"watchpoint_generation\":3") != std::string::npos &&
                crash_json.find("\"pending_events\":2") != std::string::npos &&
                crash_json.find("address-error-read") != std::string::npos,
            "Crashbericht verliert Delay-Slot-Owner, kanonische Adresse oder Laufzeitkontext.");
    auto incomplete = crash_context;
    incomplete.block_variant.reset();
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(capture_crash_report(cpu, incomplete)); },
        "Unvollstaendiger Blockkontext wurde im Crashbericht akzeptiert.");
    auto unportable = crash_context;
    unportable.stop_code = "host path";
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(capture_crash_report(cpu, unportable)); },
        "Freie Hostfehlermeldung wurde als Crashcode akzeptiert.");

    RuntimeTraceRecorder trace({2u, false});
    auto memory = trace.memory_observer(
        RuntimeTraceKind::MemoryWrite,
        RuntimeTraceOrigin::Backend,
        [] { return 10u; },
        [] { return 0x8C001000u; });
    memory(
        {MemoryAccessOperation::Write, 0xA05F6800u, MemoryAccessWidth::Word, 0xDEADBEEFu, "pvr"});
    auto watchpoint = trace.memory_observer(
        RuntimeTraceKind::Watchpoint,
        RuntimeTraceOrigin::Runtime,
        [] { return 11u; },
        [] { return 0x8C001002u; });
    watchpoint(
        {MemoryAccessOperation::Read, 0x8C010000u, MemoryAccessWidth::Halfword, 0x1234u, "ram"});
    trace.record({0u,
                  12u,
                  RuntimeTraceKind::Scheduler,
                  RuntimeTraceOrigin::Runtime,
                  0x8C001004u,
                  std::nullopt,
                  std::nullopt,
                  std::nullopt,
                  "event-budget"});
    require(trace.events().size() == 2u && trace.total_events() == 3u &&
                trace.dropped_events() == 1u &&
                trace.events()[0].kind == RuntimeTraceKind::MemoryWrite &&
                trace.events()[1].kind == RuntimeTraceKind::Watchpoint &&
                !trace.events()[0].value.has_value() &&
                trace.serialize_json().find("DEADBEEF") == std::string::npos,
            "Tracekapazitaet, Ereignisart oder standardmaessige Wertredaktion ist falsch.");
    require_failure<std::invalid_argument>(
        [&] {
            trace.record({0u,
                          9u,
                          RuntimeTraceKind::BlockEntry,
                          RuntimeTraceOrigin::Backend,
                          0x1000u,
                          {},
                          {},
                          {},
                          "backward"});
        },
        "Rueckwaerts laufende Trace-Gastzeit wurde akzeptiert.");
    RuntimeTraceRecorder values({1u, true});
    values.record({0u,
                   1u,
                   RuntimeTraceKind::MemoryRead,
                   RuntimeTraceOrigin::Fallback,
                   0x1000u,
                   0x2000u,
                   MemoryAccessWidth::Word,
                   0xCAFEBABEu,
                   "memory-access"});
    require(values.events()[0].value == 0xCAFEBABEu, "Trace-Wert-Opt-in wurde ignoriert.");

    DispatchDiagnosticRecorder dispatch;
    const auto base = dispatch_event();
    dispatch.record(base);
    dispatch.record(base);
    const std::array origins{DispatchResolutionOrigin::StaticProof,
                             DispatchResolutionOrigin::Override,
                             DispatchResolutionOrigin::TableLookup,
                             DispatchResolutionOrigin::InlineCache,
                             DispatchResolutionOrigin::Fallback};
    for (std::size_t index = 0u; index < origins.size(); ++index) {
        auto event = base;
        event.callsite += static_cast<std::uint32_t>(0x10u + index * 2u);
        event.origin = origins[index];
        dispatch.record(event);
    }
    const std::array reasons{DispatchFallbackReason::UnknownOpcode,
                             DispatchFallbackReason::UnresolvedControlFlow,
                             DispatchFallbackReason::DynamicCode,
                             DispatchFallbackReason::ManifestDenied};
    for (std::size_t index = 0u; index < reasons.size(); ++index) {
        auto event = base;
        event.callsite += static_cast<std::uint32_t>(0x40u + index * 2u);
        event.fallback_reason = reasons[index];
        event.alias_origin = index == 0u ? DispatchAliasOrigin::ExactVirtual
                                         : DispatchAliasOrigin::CanonicalPhysical;
        dispatch.record(event);
    }
    const std::array errors{DispatchDiagnosticError::UnknownCode,
                            DispatchDiagnosticError::UnknownTarget,
                            DispatchDiagnosticError::UnmappedMemory,
                            DispatchDiagnosticError::FirmwareDenied,
                            DispatchDiagnosticError::Misaligned,
                            DispatchDiagnosticError::InvalidBoundary};
    for (std::size_t index = 0u; index < errors.size(); ++index) {
        auto event = base;
        event.callsite += static_cast<std::uint32_t>(0x100u + index * 2u);
        event.error = errors[index];
        event.fallback_action = static_cast<DispatchFallbackAction>(index % 5u);
        dispatch.record(event);
    }
    const auto before_invalid = dispatch.total_occurrences();
    auto invalid = base;
    invalid.canonical_target = 0x0C003000u;
    require(!dispatch.try_record(invalid) && dispatch.total_occurrences() == before_invalid &&
                dispatch.events().front().occurrences == 2u,
            "Ausnahmesichere Dispatchbeobachtung veraendert Zaehler oder dedupliziert nicht.");
    const auto dispatch_json = dispatch.serialize_json();
    const auto hotspot_json = dispatch.serialize_hotspots_json(1u);
    require(hotspot_json.find("katana-dispatch-hotspots") != std::string::npos &&
                hotspot_json.find("\"occurrences\":2") != std::string::npos,
            "Dispatch-Hotspotbericht priorisiert wiederholte Kontrollfluesse nicht.");
    for (const auto marker : {"static-proof",       "override",         "table-lookup",
                              "inline-cache",       "fallback",         "unknown-code",
                              "unknown-target",     "unmapped-memory",  "firmware-denied",
                              "misaligned",         "invalid-boundary", "exact-virtual",
                              "canonical-physical", "unknown-opcode",   "unresolved-control-flow",
                              "dynamic-code",       "manifest-denied",  "abort",
                              "diagnose",           "interpreter",      "user-hook"}) {
        require(dispatch_json.find(marker) != std::string::npos,
                std::string("Dispatchdiagnose verliert Vertragsklasse: ") + marker);
    }
    require(dispatch_json.find("0xAC002000") != std::string::npos &&
                dispatch_json.find("0x0C002000") != std::string::npos,
            "Dispatchdiagnose verdeckt virtuelle Adresse oder kanonisches physisches Ziel.");

    DispatchDiagnosticRecorder bounded_dispatch(2u);
    bounded_dispatch.record(base);
    auto second_bounded = base;
    second_bounded.callsite += 4u;
    bounded_dispatch.record(second_bounded);
    auto dropped_bounded = base;
    dropped_bounded.callsite += 8u;
    bounded_dispatch.record(dropped_bounded);
    require(bounded_dispatch.events().size() == bounded_dispatch.capacity() &&
                bounded_dispatch.dropped_unique_events() == 1u &&
                bounded_dispatch.total_occurrences() == 3u,
            "Dispatchdiagnostik waechst ungebremst oder verliert den Aggregatzaehler.");

    DispatchDiagnosticRecorder late_hotspot(1u);
    late_hotspot.record(base);
    auto frequent_late = base;
    frequent_late.callsite += 0x40u;
    for (std::size_t occurrence = 0u; occurrence < 100u; ++occurrence)
        late_hotspot.record(frequent_late);
    require(late_hotspot.events().size() == 1u &&
                late_hotspot.events().front().callsite == frequent_late.callsite &&
                late_hotspot.events().front().occurrences == 100u &&
                late_hotspot.dropped_unique_events() == 1u &&
                late_hotspot.serialize_hotspots_json().find("\"occurrences\":100") !=
                    std::string::npos,
            "Ein nach Kapazitaetssaettigung auftretender Hotspot wird verschluckt oder als "
            "wiederholt neuer Verlust gezaehlt.");

    ExecutableCodeTracker tracker;
    const std::array block_origins{ExecutableBlockOrigin::ImageSegment,
                                   ExecutableBlockOrigin::RomRamCopy,
                                   ExecutableBlockOrigin::FallbackDecode,
                                   ExecutableBlockOrigin::RuntimeWrite};
    for (std::size_t index = 0u; index < block_origins.size(); ++index) {
        const auto address = 0x0C001000u + static_cast<std::uint32_t>(index * 0x1000u);
        const auto inserted = tracker.register_block(
            {"block-" + std::to_string(index),
             address,
             4u,
             index == 3u ? std::string("runtime-write") : std::string("synthetic-image"),
             {"source-link-" + std::to_string(index)},
             block_origins[index]});
        require(inserted == BlockRegistrationResult::Inserted,
                "Provenienzblock wurde nicht registriert.");
    }
    static_cast<void>(tracker.observe_write(0x8C001000u, 1u, CodeWriteSource::Cpu));
    static_cast<void>(tracker.observe_write(0x0C002000u, 1u, CodeWriteSource::Dma));
    static_cast<void>(tracker.observe_write(0xAC003000u, 1u, CodeWriteSource::Copy));
    require(tracker.page_generation(0x0C001000u) == 1u && tracker.invalidation_count() == 3u &&
                !tracker.valid("block-0") && tracker.valid("block-3"),
            "CPU/DMA/Copy-Invalidierungen oder Seitengenerationen sind nicht nachvollziehbar.");

    FirmwareHandoffMap firmware;
    firmware.map_segment({"rom-p1", FirmwareSegmentKind::Rom, 0x80000000u, 0x00000000u, 0x1000u});
    firmware.map_segment({"rom-p2", FirmwareSegmentKind::Rom, 0xA0000000u, 0x00000000u, 0x1000u});
    firmware.map_segment({"ram", FirmwareSegmentKind::Ram, 0x8C005000u, 0x0C005000u, 0x1000u});
    firmware.record_copy({0x00000100u, 0x0C005000u, 16u, "verified-copy", true, false});
    firmware.install_runtime_symbol(
        {"bios-vector", 0x8C005000u, 0x0C005000u, std::string("local:") + "\\private", 123u});
    require(firmware.canonical_origin_count() == 2u &&
                firmware.resolve(0x8C005000u).copy.has_value() &&
                firmware.resolve(0x8C005000u).statically_proven,
            "Aliasgruppe oder verifizierte ROM-RAM-Kopie ist nicht verbunden.");
    const auto provenance_json = serialize_runtime_provenance_json(tracker, firmware);
    for (const auto marker : {"image-segment",
                              "rom-ram-copy",
                              "fallback-decode",
                              "runtime-write",
                              "\"source\":\"cpu\"",
                              "\"source\":\"dma\"",
                              "\"source\":\"copy\"",
                              "\"generation\":1",
                              "source-link-0",
                              "bios-vector",
                              "\"dropped_invalidation_events\":0"}) {
        require(provenance_json.find(marker) != std::string::npos,
                std::string("Runtimeprovenienz verliert Vertragsfeld: ") + marker);
    }
    require(provenance_json.find("local:") == std::string::npos &&
                provenance_json.find("\"provenance\":\"redacted\"") != std::string::npos,
            "Runtimeprovenienz redigiert unportable lokale Herkunft nicht.");

    std::cout << "KR-3603/3604/3607/3608 Laufzeitdiagnostik erfolgreich.\n";
    return EXIT_SUCCESS;
}
