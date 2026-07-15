#include "katana/platform/phase6_gate.hpp"

#include "katana/platform/dreamcast.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/disc.hpp"
#include "katana/runtime/dma.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/interrupt.hpp"
#include "katana/runtime/platform_interrupt.hpp"
#include "katana/runtime/scheduler.hpp"
#include "katana/runtime/timers.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::platform {
namespace {

constexpr std::uint32_t dma_auto_byte_increment = 0x00000410u;

bool inside_boot_program(
    const std::uint32_t address,
    const std::size_t boot_size
) {
    const auto start = static_cast<std::uint64_t>(dreamcast_disc_boot_address);
    const auto end = start + boot_size;
    return address >= start && static_cast<std::uint64_t>(address) < end;
}

bool controlled_probe_stop(const std::string_view message) {
    return message == "Nicht aufgeloester Sprung" ||
        message == "Nicht aufgeloester Aufruf" ||
        message == "PC liegt ausserhalb der generierten Funktion";
}

void require_gate(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("Phase-6-Gate: " + message);
    }
}

std::string bool_json(const bool value) {
    return value ? "true" : "false";
}

} // namespace

Phase6GateReport run_phase6_gate(
    const std::filesystem::path& descriptor_path,
    const Phase6BlockExecutor execute_block,
    const std::size_t block_instruction_count,
    const std::uint64_t guest_cycle_budget
) {
    if (execute_block == nullptr) {
        throw std::invalid_argument("Phase-6-Gate braucht einen Block-Executor.");
    }
    if (block_instruction_count == 0u || block_instruction_count > guest_cycle_budget) {
        throw std::invalid_argument("Phase-6-Block liegt ausserhalb des Gastzyklusbudgets.");
    }

    const auto disc = load_dreamcast_gdi_boot(descriptor_path);
    Phase6GateReport report;
    report.gdi_loaded = true;
    report.tracks_validated = disc.validated_tracks;
    report.iso9660_mounted = true;
    report.boot_metadata_read = true;
    report.boot_file_loaded = !disc.boot_file.empty();
    report.repeated_reads_match = disc.repeated_reads_match;
    require_gate(report.tracks_validated != 0u, "keine validierten Tracks.");
    require_gate(report.repeated_reads_match, "wiederholte Bootdatei-Reads unterscheiden sich.");

    runtime::CpuState cpu;
    cpu.memory = runtime::Memory(0u);
    const auto image = make_dreamcast_disc_executable(disc);
    const auto boot = boot_homebrew(cpu, image);
    const auto cache_control = runtime::map_sh4_cache_control(cpu.memory);
    require_gate(
        boot.entry_point == dreamcast_disc_boot_address &&
            inside_boot_program(cpu.pc, disc.boot_file.size()),
        "Programmeinstieg wurde nicht in den geladenen Hauptprogrammbereich gesetzt."
    );

    try {
        execute_block(cpu);
    } catch (const std::runtime_error& error) {
        if (!controlled_probe_stop(error.what())) {
            throw;
        }
        ++report.fallbacks;
    }
    require_gate(
        !cpu.trap_pending && cpu.last_exception_cause == runtime::ExceptionCause::None,
        "Bootblock endete mit einer CPU-Ausnahme."
    );
    report.last_guest_pc = cpu.pc;
    require_gate(
        inside_boot_program(report.last_guest_pc, disc.boot_file.size()),
        "letzter Gast-PC liegt ausserhalb des geladenen Hauptprogramms."
    );
    report.main_executable_entered = true;
    report.executed_blocks = 1u;
    report.guest_cycles = block_instruction_count;
    report.cache_invalidations = cache_control->instruction_invalidation_count();

    runtime::EventScheduler scheduler;
    runtime::Memory platform_memory(256u, runtime::MemoryAlignmentPolicy::Strict);
    runtime::Sh4Tmu tmu(scheduler, runtime::TmuTiming{1u, 64u});
    runtime::Sh4Rtc rtc(scheduler, 256u);
    runtime::Sh4Dmac dmac(scheduler, platform_memory, runtime::DmaTiming{1u});
    runtime::InterruptController interrupts;
    runtime::PlatformInterruptRouter router(interrupts, tmu, rtc, dmac);
    router.set_tmu_level(0u, 6u);
    router.set_dma_level(8u);

    tmu.write_constant(0u, 0u);
    tmu.write_counter(0u, 0u);
    tmu.write_control(0u, runtime::Sh4Tmu::underflow_interrupt_enable);
    tmu.write_start(1u);

    platform_memory.write_u8(0x00u, 0xA5u);
    dmac.write_source(0u, 0x00u);
    dmac.write_destination(0u, 0x40u);
    dmac.write_count(0u, 1u);
    dmac.write_control(
        0u,
        dma_auto_byte_increment |
            runtime::Sh4Dmac::interrupt_enable |
            runtime::Sh4Dmac::channel_enable
    );
    dmac.write_operation(runtime::Sh4Dmac::master_enable);

    runtime::GdRomAsyncReader gdrom(
        runtime::GdRomDrive(disc.source),
        runtime::GdRomTiming{2u, 1u}
    );
    static_cast<void>(gdrom.submit({
        runtime::GdRomCommand::ReadSectors,
        disc.data_track_lba,
        1u
    }));
    static_cast<void>(scheduler.schedule_at(
        3u,
        [&](const runtime::SchedulerEventId, const std::uint64_t cycle) {
            gdrom.advance_to(cycle);
            const auto completion = gdrom.take_completed();
            require_gate(
                completion.has_value() &&
                    completion->response.status == runtime::GdRomStatus::Good &&
                    completion->response.transferred_sectors == 1u,
                "asynchroner GD-ROM-Read wurde nicht erfolgreich abgeschlossen."
            );
            ++report.gdrom_completions;
            router.set_external_pending(0u, true);
        }
    ));

    const auto advance = scheduler.advance_to(4u, 16u);
    require_gate(
        advance.status == runtime::SchedulerAdvanceStatus::ReachedTarget,
        "Scheduler-Ereignisbudget wurde unerwartet erschoepft."
    );
    report.scheduler_events = scheduler.processed_event_count();
    report.scheduler_cycle = scheduler.current_cycle();
    report.scheduler_pending_events = scheduler.pending_event_count();
    report.tmu_events = tmu.underflow_count(0u);
    report.dma_events = dmac.completed_transfer_units(0u);
    require_gate(report.gdrom_completions == 1u, "GD-ROM-Completion fehlt.");
    require_gate(report.tmu_events != 0u, "TMU-Ereignis fehlt.");
    require_gate(report.dma_events != 0u, "DMA-Ereignis fehlt.");
    require_gate(platform_memory.read_u8(0x40u) == 0xA5u, "DMA-Nutzdaten fehlen.");

    static_cast<void>(router.synchronize());
    cpu.set_interrupt_mask(0u);
    if (router.accept(cpu)) {
        ++report.interrupts_delivered;
    }
    require_gate(
        report.interrupts_delivered == 1u &&
            cpu.intevt == static_cast<std::uint32_t>(runtime::PlatformInterruptSource::ExternalIrl13),
        "GD-ROM-Abschlussinterrupt wurde nicht angenommen."
    );

    report.checkpoint = "SA_PHASE6_MAIN_EXECUTION_STARTED";
    return report;
}

std::string serialize_phase6_gate_report(const Phase6GateReport& report) {
    std::ostringstream output;
    output << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"checkpoint\": \"" << report.checkpoint << "\",\n"
        << "  \"gdi_loaded\": " << bool_json(report.gdi_loaded) << ",\n"
        << "  \"tracks_validated\": " << report.tracks_validated << ",\n"
        << "  \"iso9660_mounted\": " << bool_json(report.iso9660_mounted) << ",\n"
        << "  \"boot_metadata_read\": " << bool_json(report.boot_metadata_read) << ",\n"
        << "  \"boot_file_loaded\": " << bool_json(report.boot_file_loaded) << ",\n"
        << "  \"repeated_reads_match\": " << bool_json(report.repeated_reads_match) << ",\n"
        << "  \"main_executable_entered\": " << bool_json(report.main_executable_entered) << ",\n"
        << "  \"executed_blocks\": " << report.executed_blocks << ",\n"
        << "  \"guest_cycles\": " << report.guest_cycles << ",\n"
        << "  \"scheduler_events\": " << report.scheduler_events << ",\n"
        << "  \"gdrom_completions\": " << report.gdrom_completions << ",\n"
        << "  \"tmu_events\": " << report.tmu_events << ",\n"
        << "  \"dma_events\": " << report.dma_events << ",\n"
        << "  \"interrupts_delivered\": " << report.interrupts_delivered << ",\n"
        << "  \"cache_invalidations\": " << report.cache_invalidations << ",\n"
        << "  \"indirect_dispatches\": " << report.indirect_dispatches << ",\n"
        << "  \"fallbacks\": " << report.fallbacks << ",\n"
        << "  \"silent_failures\": " << report.silent_failures << ",\n"
        << "  \"pvr_frames\": " << report.pvr_frames << ",\n"
        << "  \"audio_sample_frames\": " << report.audio_sample_frames << ",\n"
        << "  \"maple_transactions\": " << report.maple_transactions << ",\n"
        << "  \"last_guest_pc\": \"0x" << std::hex << std::uppercase
        << std::setw(8) << std::setfill('0') << report.last_guest_pc << "\",\n"
        << std::dec
        << "  \"scheduler_cycle\": " << report.scheduler_cycle << ",\n"
        << "  \"scheduler_pending_events\": " << report.scheduler_pending_events << "\n"
        << "}\n";
    return output.str();
}

void write_phase6_gate_report(
    const Phase6GateReport& report,
    const std::filesystem::path& output_path
) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Phase-6-Bericht konnte nicht geoeffnet werden.");
    }
    const auto serialized = serialize_phase6_gate_report(report);
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    if (!output) {
        throw std::runtime_error("Phase-6-Bericht konnte nicht geschrieben werden.");
    }
}

} // namespace katana::platform
