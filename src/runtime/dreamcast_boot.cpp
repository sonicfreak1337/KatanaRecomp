#include "katana/runtime/dreamcast_boot.hpp"

#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace katana::runtime {
namespace {

std::string trimmed_ascii(const std::span<const std::uint8_t> bytes,
                          const std::size_t offset,
                          const std::size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    std::string result(reinterpret_cast<const char*>(bytes.data() + offset), length);
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) {
        result.pop_back();
    }
    return result;
}

bool safe_iso_file_name(const std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos && value.find(':') == std::string_view::npos;
}

std::vector<std::uint32_t> extent_bias_candidates(const std::uint32_t data_track_lba) {
    return data_track_lba == 0u ? std::vector<std::uint32_t>{0u}
                                : std::vector<std::uint32_t>{0u, data_track_lba};
}

} // namespace

DreamcastRuntimeBootImage
load_dreamcast_runtime_boot(const std::filesystem::path& descriptor_path) {
    if (descriptor_path.empty()) {
        throw std::invalid_argument("Dreamcast-Runtime braucht eine GDI-Quelle.");
    }
    auto source = GdiDiscSource::open(descriptor_path);
    const auto data_track_lba = source->primary_data_lba();
    if (data_track_lba > std::numeric_limits<std::uint64_t>::max() / 2048u) {
        throw std::out_of_range("Dreamcast-Bootsektoroffset laeuft ueber.");
    }
    const auto boot_sector = source->read(static_cast<std::uint64_t>(data_track_lba) * 2048u, 256u);
    const auto hardware_id = trimmed_ascii(boot_sector, 0x00u, 16u);
    const auto boot_file_name = trimmed_ascii(boot_sector, 0x60u, 16u);
    if (hardware_id != "SEGA SEGAKATANA") {
        throw std::invalid_argument("Dreamcast-Hardwarekennung ist ungueltig.");
    }
    if (!safe_iso_file_name(boot_file_name)) {
        throw std::invalid_argument("Dreamcast-Bootdateiname ist ungueltig.");
    }

    std::vector<std::uint8_t> boot_file;
    std::uint32_t selected_bias = 0u;
    std::string last_error = "ISO9660-Dateisystem konnte nicht geoeffnet werden.";
    for (const auto bias : extent_bias_candidates(data_track_lba)) {
        try {
            Iso9660Filesystem filesystem(source, 2048u, data_track_lba, bias);
            boot_file = filesystem.read_file("/" + boot_file_name);
            selected_bias = bias;
            break;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    if (boot_file.empty()) {
        throw std::runtime_error("Dreamcast-Bootdatei ist leer oder nicht lesbar: " + last_error);
    }
    if (boot_file.size() > dreamcast_main_ram_size - 0x00010000u) {
        throw std::out_of_range("Dreamcast-Bootdatei passt nicht in den Hauptspeicher.");
    }

    Iso9660Filesystem repeated_filesystem(source, 2048u, data_track_lba, selected_bias);
    const auto repeated = repeated_filesystem.read_file("/" + boot_file_name);
    const bool repeated_reads_match = repeated == boot_file;
    const auto validated_tracks = source->descriptor().tracks.size();
    return {std::move(source),
            hardware_id,
            boot_file_name,
            std::move(boot_file),
            data_track_lba,
            selected_bias,
            validated_tracks,
            repeated_reads_match};
}

DreamcastRuntimeState
initialize_dreamcast_runtime(CpuState& cpu,
                             const DreamcastRuntimeBootImage& boot,
                             const DreamcastRuntimeFirmwareMode firmware_mode) {
    if (!boot.source || boot.boot_file.empty() || !boot.repeated_reads_match) {
        throw std::invalid_argument("Dreamcast-Runtime-Bootimage ist unvollstaendig.");
    }
    cpu.memory = Memory(0u);
    DreamcastRuntimeState state;
    state.main_ram = map_dreamcast_main_ram(cpu.memory);
    state.vram = map_dreamcast_vram(cpu.memory);
    state.aica_ram = map_dreamcast_aica_ram(cpu.memory);
    state.flash = map_dreamcast_command_flash(cpu.memory);
    state.scheduler = std::make_shared<EventScheduler>();
    state.rtc_clock = std::make_shared<Sh4RtcClockDomain>(256u);
    state.tmu = std::make_shared<Sh4Tmu>(*state.scheduler, TmuTiming{1u, state.rtc_clock});
    state.rtc = std::make_shared<Sh4Rtc>(*state.scheduler, state.rtc_clock);
    state.dmac = std::make_shared<Sh4Dmac>(*state.scheduler, cpu.memory, DmaTiming{1u});
    state.interrupt_controller = std::make_shared<InterruptController>();
    state.interrupt_router = std::make_shared<PlatformInterruptRouter>(
        *state.interrupt_controller, *state.tmu, *state.rtc, *state.dmac);
    state.system_asic = map_dreamcast_system_asic(cpu.memory, *state.interrupt_router);
    const auto asic = std::weak_ptr<DreamcastSystemAsic>(state.system_asic);
    const auto scheduler = std::weak_ptr<EventScheduler>(state.scheduler);
    const auto raise_now = [asic, scheduler](const SystemAsicEvent event) {
        const auto target = asic.lock();
        const auto clock = scheduler.lock();
        if (!target || !clock)
            throw std::runtime_error("Dreamcast-System-ASIC-Lebenszyklus fehlt.");
        target->raise(event, clock->current_cycle());
    };
    state.pvr_registers =
        map_pvr_registers(cpu.memory, [raise_now] { raise_now(SystemAsicEvent::PvrRenderDone); });
    state.aica_registers = map_aica_registers(cpu.memory);
    state.maple = std::make_shared<MapleBus>([raise_now] { raise_now(SystemAsicEvent::MapleDma); });
    state.gdrom = std::make_shared<GdRomAsyncReader>(
        GdRomDrive(boot.source), GdRomTiming{}, [asic, scheduler](const std::uint64_t cycle) {
            const auto target = asic.lock();
            const auto clock = scheduler.lock();
            if (!target || !clock)
                throw std::runtime_error("Dreamcast-System-ASIC-Lebenszyklus fehlt.");
            static_cast<void>(target->schedule(*clock, SystemAsicEvent::GdromCommand, cycle));
        });
    state.aica = std::make_shared<AicaExecutionController>();
    state.aica->interrupts().set_observer(
        [raise_now] { raise_now(SystemAsicEvent::AicaInterrupt); });
    state.runtime_blocks = std::make_shared<RuntimeBlockTable>();
    state.firmware_handoff = std::make_shared<FirmwareHandoffMap>();
    state.firmware_handoff->map_segment({"main-ram",
                                         FirmwareSegmentKind::Ram,
                                         0x8C000000u,
                                         0x0C000000u,
                                         static_cast<std::uint32_t>(dreamcast_main_ram_size)});
    if (firmware_mode == DreamcastRuntimeFirmwareMode::HleBiosAbi)
        install_hle_bios_abi(cpu.memory, *state.runtime_blocks, *state.firmware_handoff);
    std::copy(boot.boot_file.begin(),
              boot.boot_file.end(),
              state.main_ram->writable_bytes().begin() + 0x00010000u);
    state.loaded_boot_bytes = boot.boot_file.size();
    reset_cpu(cpu,
              ResetState{dreamcast_disc_boot_address, dreamcast_direct_boot_stack, 0u, 0u, 0u});
    return state;
}

} // namespace katana::runtime
