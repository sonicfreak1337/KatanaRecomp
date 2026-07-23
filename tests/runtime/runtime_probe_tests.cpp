#include "katana/runtime/pvr.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/runtime_probe.hpp"
#include "katana/runtime/system_asic.hpp"
#include "katana/runtime/system_replay.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

template <typename Snapshot>
void require_snapshot_mutation_changes(const Snapshot& before,
                                       const Snapshot& after,
                                       const std::string& message) {
    const std::array baseline = {make_runtime_probe_device_snapshot(before)};
    const std::array changed = {make_runtime_probe_device_snapshot(after)};
    require(hash_runtime_probe_devices(baseline) !=
                hash_runtime_probe_devices(changed),
            message);
}

katana::runtime::RuntimeProbeCpuSnapshot
filled_cpu_snapshot(std::uint32_t value,
                    const std::uint64_t retired_guest_instructions) {
    using namespace katana::runtime;
    RuntimeProbeCpuSnapshot cpu;
    const auto next = [&value]() { return value++; };
    for (auto& entry : cpu.r) entry = next();
    for (auto& entry : cpu.r_bank) entry = next();
    for (auto& entry : cpu.fr) entry = next();
    for (auto& entry : cpu.xf) entry = next();
    cpu.pc = next();
    cpu.pr = next();
    cpu.gbr = next();
    cpu.vbr = next();
    cpu.ssr = next();
    cpu.spc = next();
    cpu.sgr = next();
    cpu.dbr = next();
    cpu.tra = next();
    cpu.tea = next();
    cpu.expevt = next();
    cpu.intevt = next();
    cpu.pteh = next();
    cpu.ptel = next();
    cpu.ptea = next();
    cpu.ttb = next();
    cpu.mmucr = next();
    for (auto& entry : cpu.utlb) {
        entry.pteh = next();
        entry.ptel = next();
        entry.ptea = next();
    }
    cpu.tlb_load_count = next();
    cpu.mach = next();
    cpu.macl = next();
    cpu.fpul = next();
    cpu.fpscr = next();
    cpu.sr = next();
    cpu.t = true;
    cpu.s = true;
    cpu.q = false;
    cpu.m = true;
    cpu.trap_pending = true;
    cpu.last_exception_cause = ExceptionCause::TlbProtectionWrite;
    cpu.exception_in_delay_slot = true;
    cpu.sleeping = false;
    cpu.last_prefetch_address = next();
    cpu.prefetch_count = next();
    cpu.retired_guest_instructions = retired_guest_instructions;
    cpu.last_prefetch_was_store_queue = true;
    return cpu;
}

std::vector<katana::runtime::RuntimeProbeDeviceSnapshot>
complete_device_profile(
    const katana::runtime::RuntimeProbeDeviceSnapshot& pvr,
    const katana::runtime::RuntimeProbeDeviceSnapshot& system_bus) {
    using namespace katana::runtime;
    std::vector<RuntimeProbeDeviceSnapshot> devices;
    devices.reserve(runtime_probe_deterministic_v1_device_schemas.size());
    for (const auto& schema : runtime_probe_deterministic_v1_device_schemas) {
        RuntimeProbeDeviceSnapshot device;
        device.kind = schema.kind;
        device.instance = schema.instance;
        device.fields.reserve(schema.field_count);
        for (std::uint32_t field = 1u; field <= schema.field_count; ++field)
            device.fields.push_back(
                {field, field == 1u ? runtime_probe_device_schema_version : 0u});
        if (schema.kind == RuntimeProbeDeviceKind::Pvr)
            device = pvr;
        else if (schema.kind == RuntimeProbeDeviceKind::SystemBus)
            device = system_bus;
        devices.push_back(std::move(device));
    }
    return devices;
}

} // namespace

int main() {
    using namespace katana::runtime;

    require(throws<std::invalid_argument>([] {
                const DreamcastRuntimeState incomplete;
                static_cast<void>(
                    capture_runtime_probe_dreamcast(incomplete, 0u, 0u, 0u));
            }),
            "Unvollstaendiger Produktzustand darf keinen scheinbar gueltigen Snapshot liefern.");

    const std::array<std::uint8_t, 5u> hello = {'h', 'e', 'l', 'l', 'o'};
    RuntimeProbeFnv1a64LeV1 hello_hash;
    hello_hash.append_bytes(hello);
    require(hello_hash.value() == 0xA430D84680AABD0Bull,
            "FNV-1a-64 stimmt nicht mit dem portablen Referenzvektor ueberein.");

    RuntimeProbeFnv1a64LeV1 scalar_hash;
    scalar_hash.append_u32(0x04030201u);
    const std::array<std::uint8_t, 4u> scalar_bytes = {1u, 2u, 3u, 4u};
    RuntimeProbeFnv1a64LeV1 byte_hash;
    byte_hash.append_bytes(scalar_bytes);
    require(scalar_hash.value() == byte_hash.value(),
            "Der Runtime-Probe-Codec kodiert u32 nicht fest little-endian.");

    require(static_cast<std::uint8_t>(RuntimeProbeTermination::BudgetReached) == 3u &&
                static_cast<std::uint8_t>(RuntimeProbeTermination::Failed) == 5u &&
                static_cast<std::uint8_t>(RuntimeProbeTermination::Hang) == 6u &&
                static_cast<std::uint8_t>(
                    RuntimeProbeTermination::GuestException) == 7u &&
                static_cast<std::uint8_t>(
                    RuntimeProbeTermination::DispatchMiss) == 8u,
            "Die Runtime-Probe-Endklassen sind nicht stabil nummeriert.");
    require(runtime_probe_fault_line_prefix == "KATANA_RUNTIME_PROBE_FAULT ",
            "Der parsebare Runtime-Probe-Fehlerprefix ist instabil.");

    const std::array fault_classes = {
        std::pair{RuntimeProbeTermination::Hang, std::string_view{"hang"}},
        std::pair{RuntimeProbeTermination::GuestException,
                  std::string_view{"guest-exception"}},
        std::pair{RuntimeProbeTermination::DispatchMiss,
                  std::string_view{"dispatch-miss"}},
        std::pair{RuntimeProbeTermination::Failed, std::string_view{"failed"}},
    };
    for (const auto& [termination, name] : fault_classes) {
        RuntimeProbeObservationState state;
        const auto cpu = filled_cpu_snapshot(0xA0000000u, 37u);
        require(state.latch_fault(termination, cpu),
                "Eine stabile Runtime-Probe-Fehlerklasse wurde nicht gelatcht.");
        const auto envelope =
            state.fault_envelope(RuntimeProbeTermination::BudgetReached);
        const auto json = serialize_runtime_probe_fault_envelope_json(envelope);
        require(envelope.termination == termination &&
                    envelope.first_fault.has_value() &&
                    envelope.first_fault->termination == termination &&
                    envelope.first_fault->cpu == cpu &&
                    json.find("\"termination\":\"" + std::string{name} + "\"") !=
                        std::string::npos &&
                    json.find("\"first_fault\":\"" + std::string{name} + "\"") !=
                        std::string::npos,
                "Eine Runtime-Probe-Fehlerklasse verlor Klasse oder CPU-Snapshot.");
    }

    for (const auto non_fault : {RuntimeProbeTermination::Unknown,
                                 RuntimeProbeTermination::Completed,
                                 RuntimeProbeTermination::GuestLifecycle,
                                 RuntimeProbeTermination::BudgetReached,
                                 RuntimeProbeTermination::HostShutdown}) {
        RuntimeProbeObservationState non_fault_observation;
        require(throws<std::invalid_argument>([&] {
                    static_cast<void>(serialize_runtime_probe_fault_envelope_json(
                        non_fault_observation.fault_envelope(non_fault)));
                }),
                "Fault-Envelope akzeptiert eine regulaere oder unbekannte Endklasse.");
    }

    RuntimeProbeObservationState observation;
    auto runtime_cpu = filled_cpu_snapshot(0xDEADBEEFu, 10u);
    const auto expected_runtime_cpu = runtime_cpu;
    require(!observation.observe_checkpoint(RuntimeProbeCheckpoint::None,
                                            runtime_cpu) &&
                !observation.latch_fault(RuntimeProbeTermination::BudgetReached,
                                         runtime_cpu) &&
                observation.observe_checkpoint(
                    RuntimeProbeCheckpoint::RuntimeStarted, runtime_cpu),
            "Runtime-Probe akzeptiert ungueltige Observationen oder verwirft den Start.");
    runtime_cpu.pc = 0u;
    require(observation.last_stable_checkpoint().has_value() &&
                observation.last_stable_checkpoint()->cpu ==
                    expected_runtime_cpu,
            "Der Checkpoint haelt keinen vollstaendigen eigenen CPU-Snapshot.");

    const auto duplicate_cpu = filled_cpu_snapshot(0xB0000000u, 11u);
    const auto regressive_cpu = filled_cpu_snapshot(0xC0000000u, 9u);
    require(!observation.observe_checkpoint(RuntimeProbeCheckpoint::RuntimeStarted,
                                            duplicate_cpu) &&
                !observation.observe_checkpoint(
                    RuntimeProbeCheckpoint::GuestProgramEntered,
                    regressive_cpu) &&
                observation.last_stable_checkpoint()->cpu ==
                    expected_runtime_cpu,
            "Doppelter oder zaehlerregressiver Checkpoint veraendert die Latch.");

    const auto guest_cpu = filled_cpu_snapshot(0xD0000000u, 12u);
    require(observation.observe_checkpoint(
                RuntimeProbeCheckpoint::GuestProgramEntered, guest_cpu) &&
                !observation.observe_checkpoint(
                    RuntimeProbeCheckpoint::RuntimeStarted,
                    duplicate_cpu),
            "Runtime-Probe erzwingt keine strikt monotone Checkpointklasse.");

    const auto first_fault_cpu = filled_cpu_snapshot(0xE0000000u, 13u);
    const auto later_fault_cpu = filled_cpu_snapshot(0xF0000000u, 14u);
    require(observation.latch_fault(RuntimeProbeTermination::GuestException,
                                    first_fault_cpu) &&
                !observation.latch_fault(RuntimeProbeTermination::DispatchMiss,
                                         later_fault_cpu) &&
                !observation.observe_checkpoint(
                    RuntimeProbeCheckpoint::ControlledRetailScene,
                    later_fault_cpu),
            "First-Fault oder Checkpoint-Freeze ist nicht unveraenderlich.");

    const auto fault_envelope =
        observation.fault_envelope(RuntimeProbeTermination::HostShutdown);
    require(fault_envelope.termination ==
                RuntimeProbeTermination::GuestException &&
                fault_envelope.first_fault.has_value() &&
                fault_envelope.first_fault->cpu == first_fault_cpu &&
                fault_envelope.last_stable_checkpoint.has_value() &&
                fault_envelope.last_stable_checkpoint->checkpoint ==
                    RuntimeProbeCheckpoint::GuestProgramEntered &&
                fault_envelope.last_stable_checkpoint->cpu == guest_cpu,
            "Fehlerpaket verlor First-Fault oder letzten stabilen Checkpoint.");

    const auto fault_json =
        serialize_runtime_probe_fault_envelope_json(fault_envelope);
    require(
        fault_json ==
            "{\"schema\":\"katana.runtime-probe-fault\",\"report_version\":1,"
            "\"termination\":\"guest-exception\",\"first_fault_present\":true,"
            "\"first_fault\":\"guest-exception\","
            "\"last_checkpoint_present\":true,"
            "\"last_checkpoint\":\"guest-program-entered\"}",
        "Das Runtime-Probe-Fehlerpaket besitzt kein exaktes v1-Allowlist-Schema.");
    const std::array<std::string_view, 11u> forbidden_fault_json_tokens = {
        "\"cpu\"",       "\"pc\"",      "\"register\"", "\"address\"",
        "\"guest_cycle\"", "\"retired\"", "\"hash\"",     "\"path\"",
        "\"raw\"",       "\"log\"",     "deadbeef",
    };
    for (const auto token : forbidden_fault_json_tokens) {
        require(fault_json.find(token) == std::string::npos,
                "Redigiertes Fehlerpaket leakt CPU-, Identitaets- oder Rohdaten.");
    }

    auto invalid_fault_envelope = fault_envelope;
    invalid_fault_envelope.report_version = 2u;
    require(throws<std::invalid_argument>([&] {
                static_cast<void>(serialize_runtime_probe_fault_envelope_json(
                    invalid_fault_envelope));
            }),
            "Fehlerpaket akzeptiert eine unbekannte Reportversion.");
    invalid_fault_envelope = {};
    invalid_fault_envelope.termination = RuntimeProbeTermination::DispatchMiss;
    require(throws<std::invalid_argument>([&] {
                static_cast<void>(serialize_runtime_probe_fault_envelope_json(
                    invalid_fault_envelope));
            }),
            "Fehlerpaket akzeptiert eine Fehlerklasse ohne First-Fault-Snapshot.");
    invalid_fault_envelope = {};
    invalid_fault_envelope.termination = RuntimeProbeTermination::Completed;
    invalid_fault_envelope.last_stable_checkpoint =
        RuntimeProbeCheckpointObservation{};
    require(throws<std::invalid_argument>([&] {
                static_cast<void>(serialize_runtime_probe_fault_envelope_json(
                    invalid_fault_envelope));
            }),
            "Fehlerpaket akzeptiert einen nicht gesetzten Checkpoint.");

    const std::array schema_samples = {
        make_runtime_probe_device_snapshot(PvrRegisterSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastSystemBusSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastSystemAsicSnapshot{}),
        make_runtime_probe_device_snapshot(AicaRegisterSnapshot{}),
        make_runtime_probe_device_snapshot(AicaRtcSnapshot{}),
        make_runtime_probe_device_snapshot(AicaExecutionController::Snapshot{}),
        make_runtime_probe_device_snapshot(DreamcastGdRomSnapshot{}),
        make_runtime_probe_device_snapshot(MapleBusSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastMapleControllerSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4TmuSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4RtcClockDomain::Snapshot{}),
        make_runtime_probe_device_snapshot(Sh4RtcSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4ScifSnapshot{}),
        make_runtime_probe_device_snapshot(PvrTaFifoSnapshot{}),
        make_runtime_probe_device_snapshot(PvrTaFifoMemoryDevice::Snapshot{}),
        make_runtime_probe_device_snapshot(PvrYuvConverterMemoryDevice::Snapshot{}),
        make_runtime_probe_device_snapshot(PvrSoftwareRendererSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4IoPortSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4DmacSnapshot{}),
        make_runtime_probe_device_snapshot(InterruptControllerSnapshot{}),
        make_runtime_probe_device_snapshot(PlatformInterruptRouterSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4InterruptRegistersSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4CacheControlSnapshot{}),
        make_runtime_probe_device_snapshot(RuntimeAddressSpaceSnapshot{}),
        make_runtime_probe_device_snapshot(RuntimeBlockTableSnapshot{}),
        make_runtime_probe_device_snapshot(ExecutableCodeTrackerSnapshot{}),
        make_runtime_probe_device_snapshot(ExecutableModuleCatalogSnapshot{}),
        make_runtime_probe_device_snapshot(FirmwareHandoffSnapshot{}),
        make_runtime_probe_device_snapshot(Sh4StoreQueueSnapshot{}),
        make_runtime_probe_device_snapshot(FlashMemorySnapshot{}),
        make_runtime_probe_device_snapshot(MapleVmuSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastG1DmaSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastPvrDmaSnapshot{}),
        make_runtime_probe_device_snapshot(DreamcastG2DmaSnapshot{}),
    };
    for (const auto& device : schema_samples) {
        const auto schema = std::find_if(
            runtime_probe_deterministic_v1_device_schemas.begin(),
            runtime_probe_deterministic_v1_device_schemas.end(),
            [&device](const auto& value) {
                return value.kind == device.kind &&
                       value.instance == device.instance;
            });
        require(schema != runtime_probe_deterministic_v1_device_schemas.end() &&
                    device.fields.size() == schema->field_count &&
                    !device.fields.empty() &&
                    device.fields.front().id == 1u &&
                    device.fields.front().value ==
                        runtime_probe_device_schema_version,
                "Ein Vollsnapshot stimmt nicht mit seinem stabilen Device-Schema ueberein.");
    }

    PvrRegisterSnapshot pvr_register_before;
    auto pvr_register_after = pvr_register_before;
    pvr_register_after.registers[37u] = 0x11223344u;
    require_snapshot_mutation_changes(
        pvr_register_before,
        pvr_register_after,
        "Die volle PVR-Registerapertur fehlt im Devicehash.");

    DreamcastSystemBusSnapshot bus_before;
    auto bus_after = bus_before;
    bus_after.registers[17u] = 0x55667788u;
    require_snapshot_mutation_changes(
        bus_before, bus_after, "Die volle Systembusbank fehlt im Devicehash.");

    AicaRegisterSnapshot aica_register_before;
    auto aica_register_after = aica_register_before;
    aica_register_after.registers[0x2800u] = 0xA5u;
    require_snapshot_mutation_changes(
        aica_register_before,
        aica_register_after,
        "Die volle AICA-Registerbank fehlt im Devicehash.");
    aica_register_after = aica_register_before;
    aica_register_after.channels[7u].adpcm_predictor = -123;
    require_snapshot_mutation_changes(
        aica_register_before,
        aica_register_after,
        "Der AICA-Kanallaufzustand fehlt im Devicehash.");

    AicaExecutionController::Snapshot aica_execution_before;
    auto aica_execution_after = aica_execution_before;
    aica_execution_after.timers[1u].remainder = 99u;
    require_snapshot_mutation_changes(
        aica_execution_before,
        aica_execution_after,
        "Der AICA-Ausfuehrungszustand fehlt im Devicehash.");

    AicaRtcSnapshot aica_rtc_before;
    auto aica_rtc_after = aica_rtc_before;
    aica_rtc_after.write_latch = 0x10203040u;
    require_snapshot_mutation_changes(
        aica_rtc_before,
        aica_rtc_after,
        "Der AICA-RTC-Latch fehlt im Devicehash.");

    DreamcastGdRomSnapshot gdrom_before;
    auto gdrom_after = gdrom_before;
    gdrom_after.reader.pending.push_back(
        {3u, 44u, {GdRomCommand::ReadSectors, 7u, 2u}, 9u});
    require_snapshot_mutation_changes(
        gdrom_before,
        gdrom_after,
        "Die asynchrone GD-ROM-Queue fehlt im Devicehash.");
    gdrom_after = gdrom_before;
    gdrom_after.bios_requests.push_back({});
    gdrom_after.bios_requests.back().stream_sector_cache = {1u, 2u, 3u};
    require_snapshot_mutation_changes(
        gdrom_before,
        gdrom_after,
        "Der GD-ROM-BIOS-/Streamzustand fehlt im Devicehash.");

    MapleBusSnapshot maple_bus_before;
    auto maple_bus_after = maple_bus_before;
    maple_bus_after.history.push_back(
        {1u, 0u, 0u, MapleCommand::GetCondition, MapleResponseCode::DataTransfer});
    require_snapshot_mutation_changes(
        maple_bus_before,
        maple_bus_after,
        "Die Maple-Bushistorie fehlt im Devicehash.");

    DreamcastMapleControllerSnapshot maple_controller_before;
    auto maple_controller_after = maple_controller_before;
    maple_controller_after.pending_responses.push_back(
        {0x0C000100u, {0x11223344u}});
    require_snapshot_mutation_changes(
        maple_controller_before,
        maple_controller_after,
        "Der Maple-Controllerzustand fehlt im Devicehash.");

    DreamcastSystemAsicSnapshot asic_before;
    auto asic_after = asic_before;
    asic_after.events.push_back({8u, 2u, SystemAsicEvent::PvrVblank});
    require_snapshot_mutation_changes(
        asic_before, asic_after, "Die ASIC-Ereignisqueue fehlt im Devicehash.");

    Sh4TmuSnapshot tmu_before;
    auto tmu_after = tmu_before;
    tmu_after.channels[2u].stored_counter = 17u;
    tmu_after.channels[2u].event_deadline = 91u;
    require_snapshot_mutation_changes(
        tmu_before, tmu_after, "Der volle TMU-Kanalzustand fehlt im Devicehash.");

    Sh4RtcClockDomain::Snapshot rtc_clock_before;
    auto rtc_clock_after = rtc_clock_before;
    rtc_clock_after.observer_ids.push_back(7u);
    require_snapshot_mutation_changes(
        rtc_clock_before,
        rtc_clock_after,
        "Die RTC-Clock-Beobachter fehlen im Devicehash.");

    Sh4RtcSnapshot rtc_before;
    auto rtc_after = rtc_before;
    rtc_after.divider_256hz_phase = 3u;
    rtc_after.alarm_registers[4u] = 0x80u;
    require_snapshot_mutation_changes(
        rtc_before, rtc_after, "Der volle SH-4-RTC-Zustand fehlt im Devicehash.");

    Sh4ScifSnapshot scif_before;
    auto scif_after = scif_before;
    scif_after.receive_fifo.push_back(0x41u);
    scif_after.status_last_read = 0x20u;
    require_snapshot_mutation_changes(
        scif_before, scif_after, "Der volle SCIF-Zustand fehlt im Devicehash.");

    PvrTaFifoSnapshot ta_before;
    auto ta_after = ta_before;
    ta_after.active_material.texture_format = 5u;
    ta_after.pending_modifier_vertex_packet = std::array<std::uint8_t, 32u>{};
    (*ta_after.pending_modifier_vertex_packet)[3u] = 0x7Fu;
    require_snapshot_mutation_changes(
        ta_before, ta_after, "Die TA/FSM-Zustaende fehlen im Devicehash.");

    PvrTaFifoMemoryDevice::Snapshot aperture_before;
    auto aperture_after = aperture_before;
    aperture_after.packet_active = true;
    aperture_after.packet[5u] = 0xCCu;
    require_snapshot_mutation_changes(
        aperture_before,
        aperture_after,
        "Der TA-Aperturzustand fehlt im Devicehash.");

    PvrYuvConverterMemoryDevice::Snapshot yuv_before;
    auto yuv_after = yuv_before;
    yuv_after.input = {4u, 5u, 6u};
    require_snapshot_mutation_changes(
        yuv_before, yuv_after, "Der PVR-YUV-Eingang fehlt im Devicehash.");

    PvrSoftwareRendererSnapshot renderer_before;
    auto renderer_after = renderer_before;
    renderer_after.direct_vram_shadow = {9u, 8u, 7u};
    require_snapshot_mutation_changes(
        renderer_before,
        renderer_after,
        "Der Renderer-Folge-/Shadowzustand fehlt im Devicehash.");

    Sh4IoPortSnapshot io_before;
    auto io_after = io_before;
    io_after.data_a_latch = 0x1234u;
    require_snapshot_mutation_changes(
        io_before, io_after, "Der SH-4-IO-Port-Latch fehlt im Devicehash.");

    const Sh4StoreQueueSnapshot store_queue_snapshot;
    StoreQueueTransfer first_transfer;
    first_transfer.queue = 0u;
    first_transfer.source_address = 0xE0000000u;
    first_transfer.target_address = 0x10000000u;
    first_transfer.target = StoreQueueTarget::TileAccelerator;
    first_transfer.instruction = {0x8C010000u, 0xAC010000u, true};
    first_transfer.retired_guest_instructions = 7u;
    first_transfer.bytes[0u] = 0x11u;
    auto second_transfer = first_transfer;
    second_transfer.queue = 1u;
    second_transfer.target_address = 0x0C002000u;
    second_transfer.target = StoreQueueTarget::Ram;
    second_transfer.bytes[0u] = 0x22u;
    const std::array ordered_transfers = {first_transfer, second_transfer};
    const std::array reversed_transfers = {second_transfer, first_transfer};
    const std::array ordered_snapshot = {
        make_runtime_probe_device_snapshot(
            store_queue_snapshot, ordered_transfers, 0u)};
    const std::array reversed_snapshot = {
        make_runtime_probe_device_snapshot(
            store_queue_snapshot, reversed_transfers, 0u)};
    const std::array dropped_snapshot = {
        make_runtime_probe_device_snapshot(
            store_queue_snapshot, ordered_transfers, 1u)};
    require(hash_runtime_probe_devices(ordered_snapshot) !=
                hash_runtime_probe_devices(reversed_snapshot),
            "Die Store-Queue-Transferreihenfolge fehlt im Devicehash.");
    require(hash_runtime_probe_devices(ordered_snapshot) !=
                hash_runtime_probe_devices(dropped_snapshot),
            "Verworfene Store-Queue-Transfers fehlen im Devicehash.");

    try {
        throw RuntimeProbeBudgetReached(77u);
    } catch (const RuntimeProbeBudgetReached& error) {
        require(std::string(error.what()) == "runtime-probe-budget-reached" &&
                    error.final_guest_cycle() == 77u,
                "Das typisierte Budgetende besitzt keinen stabilen Vertrag.");
    }

    CpuState cpu;
    cpu.r[3] = 0x11223344u;
    cpu.r_bank[2] = 0x55667788u;
    cpu.fr[4] = 0x3F800000u;
    cpu.xf[5] = 0x40000000u;
    cpu.pc = 0xAC008300u;
    cpu.pteh = 0x12345000u;
    cpu.ptel = 0x0C0011D4u;
    cpu.ptea = 0x00000002u;
    cpu.ttb = 0x0C100000u;
    cpu.mmucr = 0x00000001u;
    cpu.utlb[7] = {0x11111000u, 0x22222000u, 0x3u};
    cpu.tlb_load_count = 9u;
    cpu.write_sr(sr_md_mask | sr_t_mask | sr_s_mask);
    cpu.write_fpscr(fpscr_dn_mask | fpscr_sz_mask);
    cpu.retired_guest_instructions = 1234u;
    const auto cpu_snapshot = capture_runtime_probe_cpu(cpu);
    require(cpu_snapshot.pc == cpu.pc && cpu_snapshot.utlb[7].ptea == 3u &&
                cpu_snapshot.retired_guest_instructions == 1234u,
            "Der CPU-Snapshot verliert MMU-/TLB- oder Fortschrittszustand.");
    const auto cpu_hash = hash_runtime_probe_cpu(cpu_snapshot);
    auto changed_cpu = cpu_snapshot;
    ++changed_cpu.utlb[7].ptea;
    require(hash_runtime_probe_cpu(changed_cpu) != cpu_hash,
            "Eine TLB-Aenderung bleibt im CPU-Hash unsichtbar.");

    EventScheduler scheduler;
    scheduler.set_guest_cycle_budget(500u);
    static_cast<void>(scheduler.schedule_at(
        25u, [](const auto, const auto) {}, SchedulerEventKind::MediaVideo));
    static_cast<void>(scheduler.schedule_at(
        40u, [](const auto, const auto) {}, SchedulerEventKind::MediaAudio));
    const auto scheduler_snapshot = capture_runtime_probe_scheduler(scheduler);
    require(scheduler_snapshot.current_cycle == 0u &&
                scheduler_snapshot.next_event_cycle == 25u &&
                scheduler_snapshot.pending_event_count == 2u &&
                scheduler_snapshot.pending_events.size() == 2u &&
                scheduler_snapshot.pending_events[1].guest_cycle == 40u &&
                scheduler_snapshot.pending_events[1].kind == SchedulerEventKind::MediaAudio &&
                scheduler_snapshot.guest_cycle_budget == 500u,
            "Der Scheduler-Snapshot verliert seine kanonische Ereignisqueue.");
    const auto scheduler_hash = hash_runtime_probe_scheduler(scheduler_snapshot);
    auto changed_scheduler_deadline = scheduler_snapshot;
    ++changed_scheduler_deadline.pending_events[1].guest_cycle;
    require(hash_runtime_probe_scheduler(changed_scheduler_deadline) != scheduler_hash,
            "Eine spaetere Schedulerfrist bleibt im Hash unsichtbar.");
    auto changed_scheduler_kind = scheduler_snapshot;
    changed_scheduler_kind.pending_events[1].kind = SchedulerEventKind::PvrRender;
    require(hash_runtime_probe_scheduler(changed_scheduler_kind) != scheduler_hash,
            "Ein anderer Scheduler-Callbacktyp bleibt im Hash unsichtbar.");
    auto changed_scheduler_next_id = scheduler_snapshot;
    ++changed_scheduler_next_id.next_event_id;
    require(hash_runtime_probe_scheduler(changed_scheduler_next_id) != scheduler_hash,
            "Die zukuenftige Scheduler-ID-Vergabe bleibt im Hash unsichtbar.");

    SystemReplayLog replay_log;
    replay_log.record({0u,
                       3u,
                       SystemReplayEventKind::Timer,
                       "probe-timer",
                       std::nullopt,
                       std::nullopt,
                       7u,
                       11u,
                       false,
                       0u});
    replay_log.seal(0x1020304050607080ull);
    const auto replay_snapshot = capture_runtime_probe_replay(replay_log);
    require(replay_snapshot.replay_schema_version == system_replay_schema_version &&
                replay_snapshot.event_count == 1u && replay_snapshot.dropped_events == 0u &&
                replay_snapshot.event_hash == replay_snapshot.ordering_digest &&
                replay_snapshot.coverage_complete &&
                replay_snapshot.complete && replay_snapshot.sealed &&
                replay_snapshot.final_guest_state_hash == 0x1020304050607080ull,
            "Der Replay-Snapshot bindet Sequenz, Coverage und Seal nicht explizit.");
    SystemReplayLog incomplete_replay(
        {SystemReplayConfig::default_capacity,
         false,
         SystemReplayProfile::DeterministicV1});
    const auto incomplete_replay_snapshot =
        capture_runtime_probe_replay(incomplete_replay);
    require(!incomplete_replay_snapshot.coverage_complete &&
                !incomplete_replay_snapshot.complete &&
                incomplete_replay_snapshot.required_coverage != 0u,
            "Fehlende deterministic-v1-Coverage gilt als vollstaendiger Replay.");

    std::array<std::uint8_t, 4u> main_bytes = {1u, 2u, 3u, 4u};
    std::array<std::uint8_t, 3u> vram_bytes = {5u, 6u, 7u};
    const std::array memory_a = {
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::VideoRam, 0x20u, vram_bytes},
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::MainRam, 0x10u, main_bytes},
    };
    const std::array memory_b = {
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::MainRam, 0x10u, main_bytes},
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::VideoRam, 0x20u, vram_bytes},
    };
    const auto memory_hash = hash_runtime_probe_memory(memory_a);
    require(memory_hash == hash_runtime_probe_memory(memory_b),
            "Memoryrange-Reihenfolge veraendert den kanonischen Hash.");
    ++main_bytes[2];
    require(memory_hash != hash_runtime_probe_memory(memory_a),
            "Eine Speicherbyteaenderung bleibt im Memoryhash unsichtbar.");
    --main_bytes[2];

    std::array<std::uint8_t, 2u> flash_bytes = {0xA5u, 0x5Au};
    std::array<std::uint8_t, 2u> vmu_bytes = {0x11u, 0x22u};
    const std::array persistent = {
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::Vmu, 0x40u, vmu_bytes},
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::Flash, 0x20u, flash_bytes},
    };
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(hash_runtime_probe_memory(persistent)); }) &&
                throws<std::invalid_argument>(
                    [&] { static_cast<void>(hash_runtime_probe_persistent(memory_a)); }),
            "Fluechtige und persistente Runtime-Probe-Ranges werden nicht getrennt.");

    const std::array overlapping = {
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::MainRam, 2u, main_bytes},
        RuntimeProbeMemoryRange{RuntimeProbeMemoryRegion::MainRam, 4u, vram_bytes},
    };
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(hash_runtime_probe_memory(overlapping)); }),
            "Ueberlappende Runtime-Probe-Ranges werden nicht abgelehnt.");

    PvrRegisterSnapshot pvr;
    pvr.framebuffer_read_control = 0x11u;
    pvr.render_requests = 12u;
    pvr.render_completions = 11u;
    pvr.in_vblank = true;
    DreamcastSystemBusSnapshot system_bus;
    system_bus.channel2_destination = 0x10000000u;
    system_bus.channel2_length = 32u;
    system_bus.channel2_start = 1u;
    auto pvr_device = make_runtime_probe_device_snapshot(pvr);
    auto system_bus_device = make_runtime_probe_device_snapshot(system_bus);
    const std::array devices_a = {pvr_device, system_bus_device};
    const std::array devices_b = {system_bus_device, pvr_device};
    const auto device_hash = hash_runtime_probe_devices(devices_a);
    require(device_hash == hash_runtime_probe_devices(devices_b),
            "Geraetereihenfolge veraendert den kanonischen Hash.");
    std::reverse(pvr_device.fields.begin(), pvr_device.fields.end());
    const std::array reordered_fields = {pvr_device, system_bus_device};
    require(device_hash == hash_runtime_probe_devices(reordered_fields),
            "Geraetefeldreihenfolge veraendert den kanonischen Hash.");
    auto duplicate_fields = pvr_device;
    duplicate_fields.fields.push_back(duplicate_fields.fields.front());
    const std::array invalid_devices = {duplicate_fields};
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(hash_runtime_probe_devices(invalid_devices)); }),
            "Doppelte Geraetefeld-IDs werden nicht abgelehnt.");

    SystemReplayLog product_replay(
        {SystemReplayConfig::default_capacity,
         false,
         SystemReplayProfile::DeterministicV1});
    product_replay.enable_coverage(product_replay.required_coverage());
    product_replay.record({0u,
                           3u,
                           SystemReplayEventKind::Timer,
                           "profile-timer",
                           std::nullopt,
                           std::nullopt,
                           1u,
                           2u,
                           false,
                           0u});
    const auto unsealed_product_replay =
        capture_runtime_probe_replay(product_replay);
    require(throws<std::invalid_argument>([&] {
                validate_runtime_probe_deterministic_v1(
                    scheduler_snapshot,
                    memory_a,
                    persistent,
                    devices_a,
                    unsealed_product_replay);
            }),
            "Ein partielles deterministic-v1-Speicherprofil wird akzeptiert.");

    std::vector<std::uint8_t> full_main_ram(dreamcast_main_ram_size);
    std::vector<std::uint8_t> full_vram(dreamcast_vram_size);
    std::vector<std::uint8_t> full_aica_ram(dreamcast_aica_ram_size);
    std::vector<std::uint8_t> full_flash(dreamcast_flash_size);
    std::vector<std::uint8_t> full_vmu(vmu_storage_size);
    const std::array full_memory = {
        RuntimeProbeMemoryRange{
            RuntimeProbeMemoryRegion::AicaRam, 0u, full_aica_ram},
        RuntimeProbeMemoryRange{
            RuntimeProbeMemoryRegion::MainRam, 0u, full_main_ram},
        RuntimeProbeMemoryRange{
            RuntimeProbeMemoryRegion::VideoRam, 0u, full_vram},
    };
    const std::array full_persistent = {
        RuntimeProbeMemoryRange{
            RuntimeProbeMemoryRegion::Vmu, 0u, full_vmu},
        RuntimeProbeMemoryRange{
            RuntimeProbeMemoryRegion::Flash, 0u, full_flash},
    };
    const auto full_devices =
        complete_device_profile(pvr_device, system_bus_device);
    validate_runtime_probe_deterministic_v1(
        scheduler_snapshot,
        full_memory,
        full_persistent,
        full_devices,
        unsealed_product_replay);
    auto missing_device = full_devices;
    missing_device.pop_back();
    require(throws<std::invalid_argument>([&] {
                validate_runtime_probe_deterministic_v1(
                    scheduler_snapshot,
                    full_memory,
                    full_persistent,
                    missing_device,
                    unsealed_product_replay);
            }),
            "Deterministic-v1 akzeptiert eine fehlende Produktinstanz.");
    auto wrong_schema = full_devices;
    const auto schema_field = std::find_if(
        wrong_schema.front().fields.begin(),
        wrong_schema.front().fields.end(),
        [](const auto& field) { return field.id == 1u; });
    schema_field->value = runtime_probe_device_schema_version + 1u;
    require(throws<std::invalid_argument>([&] {
                validate_runtime_probe_deterministic_v1(
                    scheduler_snapshot,
                    full_memory,
                    full_persistent,
                    wrong_schema,
                    unsealed_product_replay);
            }),
            "Deterministic-v1 akzeptiert eine falsche Device-Schemaversion.");
    auto short_schema = full_devices;
    short_schema.front().fields.pop_back();
    require(throws<std::invalid_argument>([&] {
                validate_runtime_probe_deterministic_v1(
                    scheduler_snapshot,
                    full_memory,
                    full_persistent,
                    short_schema,
                    unsealed_product_replay);
            }),
            "Deterministic-v1 akzeptiert eine falsche Device-Feldzahl.");

    const auto full_device_hash = hash_runtime_probe_devices(full_devices);
    const auto product_guest_state_hash = combine_runtime_probe_guest_state_hashes(
        cpu_hash,
        hash_runtime_probe_scheduler(scheduler_snapshot),
        hash_runtime_probe_memory(full_memory),
        hash_runtime_probe_persistent(full_persistent),
        full_device_hash);
    product_replay.seal(product_guest_state_hash);
    const auto bound_replay = capture_runtime_probe_replay(product_replay);
    auto report = make_runtime_probe_report(
        cpu_snapshot,
        scheduler_snapshot,
        full_memory,
        full_persistent,
        full_devices,
        bound_replay);
    require(report.status == RuntimeProbeStatus::Complete &&
                report.guest_cycle_budget == 500u &&
                report.memory_byte_count ==
                    dreamcast_main_ram_size + dreamcast_vram_size +
                        dreamcast_aica_ram_size &&
                report.memory_range_count == 3u &&
                report.persistent_byte_count ==
                    dreamcast_flash_size + vmu_storage_size &&
                report.persistent_range_count == 2u &&
                report.device_count ==
                    runtime_probe_deterministic_v1_device_schemas.size() &&
                report.device_field_count == 867u &&
                report.hashes.combined ==
                    combine_runtime_probe_hashes(report.hashes.guest_state,
                                                 report.hashes.replay),
            "Der kompakte Runtime-Probe-Report ist inkonsistent.");

    auto changed_replay = bound_replay;
    ++changed_replay.event_hash;
    ++changed_replay.ordering_digest;
    const auto replay_changed_report = make_runtime_probe_report(
        cpu_snapshot,
        scheduler_snapshot,
        full_memory,
        full_persistent,
        full_devices,
        changed_replay);
    require(report.hashes.guest_state == replay_changed_report.hashes.guest_state &&
                report.hashes.replay != replay_changed_report.hashes.replay &&
                report.hashes.combined != replay_changed_report.hashes.combined,
            "Replaydiagnose verunreinigt den semantischen Gastzustandshash.");

    report.termination = RuntimeProbeTermination::BudgetReached;
    report.diagnostics_enabled = true;
    const auto json = serialize_runtime_probe_report_json(report);
    require(json == serialize_runtime_probe_report_json(report) &&
                json.find("\"schema\":\"katana.runtime-probe\"") != std::string::npos &&
                json.find("\"profile\":\"deterministic-v1\"") != std::string::npos &&
                json.find("\"hash_contract\":\"fnv1a64-le-v1\"") !=
                    std::string::npos &&
                json.find("\"termination\":\"budget-reached\"") !=
                    std::string::npos &&
                json.find("\"diagnostics_enabled\":true") != std::string::npos &&
                json.find("\"guest_cycle_budget\":500") != std::string::npos &&
                json.find("\"persistent\":\"") != std::string::npos &&
                json.find("\"guest_state\":\"") != std::string::npos &&
                json.find("\"event_hash\"") == std::string::npos &&
                json.find("\"final_guest_state_hash\"") == std::string::npos &&
                json.find("probe-timer") == std::string::npos,
            "Der JSON-Report ist nicht deterministisch oder leakt Roh-/Replaywerte.");

    std::cout << "Runtime-Probe-Tests bestanden.\n";
    return EXIT_SUCCESS;
}
