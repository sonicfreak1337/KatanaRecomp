#include "katana/runtime/dreamcast_boot.hpp"

#include "katana/io/json_report.hpp"
#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
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

bool project_identity_name(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

std::optional<std::filesystem::path> environment_path(const char* name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name) != 0)
        throw std::runtime_error("Umgebungsvariable konnte nicht gelesen werden.");
    const std::unique_ptr<char, decltype(&std::free)> value(raw_value, &std::free);
    if (value == nullptr || value_size <= 1u) return std::nullopt;
    return std::filesystem::path(value.get());
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::filesystem::path(value);
#endif
}

} // namespace

std::filesystem::path default_dreamcast_user_data_root() {
    if (const auto configured = environment_path("KATANA_USER_DATA_ROOT"))
        return std::filesystem::absolute(*configured).lexically_normal();
#ifdef _WIN32
    if (const auto local = environment_path("LOCALAPPDATA"))
        return std::filesystem::absolute(*local / "KatanaRecomp").lexically_normal();
#else
    if (const auto data = environment_path("XDG_DATA_HOME"))
        return std::filesystem::absolute(*data / "katana-recomp").lexically_normal();
    if (const auto home = environment_path("HOME"))
        return std::filesystem::absolute(*home / ".local" / "share" / "katana-recomp")
            .lexically_normal();
#endif
    throw std::runtime_error("Nutzerdatenwurzel fuer Flash und VMU ist nicht konfiguriert.");
}

std::shared_ptr<DreamcastMutableStorage>
DreamcastMutableStorage::open(DreamcastMutableStorageConfig config) {
    if (!project_identity_name(config.project_identity))
        throw std::invalid_argument("Persistente Dreamcast-Projektidentitaet ist ungueltig.");
    const auto root =
        (config.storage_root.empty() ? default_dreamcast_user_data_root()
                                     : std::filesystem::absolute(config.storage_root)) /
        "saves" / config.project_identity;
    auto flash = PersistentImage::open({"dreamcast-flash",
                                        std::move(config.flash_source),
                                        root / "flash.katana-work",
                                        dreamcast_flash_size,
                                        0xFFu});
    auto vmu = PersistentImage::open({"dreamcast-vmu",
                                      std::move(config.vmu_source),
                                      root / "vmu-a1.katana-work",
                                      vmu_storage_size,
                                      0xFFu});
    return std::shared_ptr<DreamcastMutableStorage>(
        new DreamcastMutableStorage(std::move(flash), std::move(vmu)));
}

DreamcastMutableStorage::DreamcastMutableStorage(std::shared_ptr<PersistentImage> flash,
                                                 std::shared_ptr<PersistentImage> vmu)
    : flash_(std::move(flash)), vmu_(std::move(vmu)) {
    if (!flash_ || !vmu_) throw std::invalid_argument("Dreamcast-Arbeitskopien fehlen.");
}

const std::shared_ptr<PersistentImage>& DreamcastMutableStorage::flash_image() const noexcept {
    return flash_;
}
const std::shared_ptr<PersistentImage>& DreamcastMutableStorage::vmu_image() const noexcept {
    return vmu_;
}
void DreamcastMutableStorage::save() {
    flash_->save();
    vmu_->save();
}

std::string DreamcastMutableStorage::serialize_status_json() const {
    std::ostringstream output;
    katana::io::write_json_report_header(
        output, "katana-dreamcast-storage-v1", "dreamcast-storage");
    output << ",\"flash\":" << flash_->serialize_status_json()
           << ",\"vmu\":" << vmu_->serialize_status_json() << '}';
    return output.str();
}

DreamcastRuntimeBootImage
load_dreamcast_runtime_boot(const std::filesystem::path& descriptor_path) {
    if (descriptor_path.empty()) {
        throw std::invalid_argument("Dreamcast-Runtime braucht eine GDI-Quelle.");
    }
    auto source = GdiDiscSource::open(descriptor_path);
    const auto data_track_lba = source->primary_data_lba();
    const auto validated_tracks = source->descriptor().tracks.size();
    return load_dreamcast_runtime_boot(std::move(source), data_track_lba, validated_tracks);
}

DreamcastRuntimeBootImage
load_dreamcast_runtime_boot_from_pack(const std::filesystem::path& pack_path) {
    if (pack_path.empty()) {
        throw std::invalid_argument("Dreamcast-Runtime braucht einen Katana-Disc-Pack.");
    }
    auto source = PackedDiscSource::open(pack_path);
    const auto data_track_lba = source->primary_data_lba();
    const auto validated_tracks = source->info().tracks.size();
    return load_dreamcast_runtime_boot(std::move(source), data_track_lba, validated_tracks);
}

DreamcastRuntimeBootImage load_dreamcast_runtime_boot(std::shared_ptr<DiscSource> source,
                                                      const std::uint32_t data_track_lba,
                                                      const std::size_t validated_tracks) {
    if (!source || validated_tracks == 0u) {
        throw std::invalid_argument("Dreamcast-Runtime-Discquelle ist unvollstaendig.");
    }
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
                             const DreamcastRuntimeFirmwareMode firmware_mode,
                             std::shared_ptr<DreamcastMutableStorage> mutable_storage) {
    if (!boot.source || boot.boot_file.empty() || !boot.repeated_reads_match) {
        throw std::invalid_argument("Dreamcast-Runtime-Bootimage ist unvollstaendig.");
    }
    cpu.memory = Memory(0u);
    DreamcastRuntimeState state;
    state.main_ram = map_dreamcast_main_ram(cpu.memory);
    state.vram = map_dreamcast_vram(cpu.memory);
    state.aica_ram = map_dreamcast_aica_ram(cpu.memory);
    state.mutable_storage = std::move(mutable_storage);
    state.flash =
        state.mutable_storage
            ? map_dreamcast_command_flash(cpu.memory, state.mutable_storage->flash_image())
            : map_dreamcast_command_flash(cpu.memory);
    state.scheduler = std::make_shared<EventScheduler>();
    state.rtc_clock = std::make_shared<Sh4RtcClockDomain>();
    state.tmu = std::make_shared<Sh4Tmu>(*state.scheduler, TmuTiming{4u, state.rtc_clock});
    state.rtc = std::make_shared<Sh4Rtc>(*state.scheduler, state.rtc_clock);
    state.dmac = std::make_shared<Sh4Dmac>(
        *state.scheduler, cpu.memory, DmaTiming{}, DmaExecutionMode::DeterministicBatch);
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
    state.pvr_registers = map_pvr_registers(
        cpu.memory, *state.scheduler, [raise_now] { raise_now(SystemAsicEvent::PvrRenderDone); });
    state.aica_registers = map_aica_registers(cpu.memory);
    state.maple = std::make_shared<MapleBus>([raise_now] { raise_now(SystemAsicEvent::MapleDma); });
    if (state.mutable_storage) {
        state.vmu = std::make_shared<MapleVmuDevice>(state.mutable_storage->vmu_image());
        state.maple->attach(0u, 1u, state.vmu);
    }
    state.gdrom = std::make_shared<GdRomAsyncReader>(
        *state.scheduler,
        GdRomDrive(boot.source),
        GdRomTiming{},
        [asic, scheduler](const std::uint64_t cycle) {
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
    state.code_tracker = std::make_shared<ExecutableCodeTracker>();
    state.runtime_blocks->bind_code_tracker(state.code_tracker.get());
    const auto code_tracker = std::weak_ptr<ExecutableCodeTracker>(state.code_tracker);
    const auto runtime_blocks = std::weak_ptr<RuntimeBlockTable>(state.runtime_blocks);
    cpu.memory.set_guest_write_observer([code_tracker,
                                         runtime_blocks](const GuestWriteEvent& event) {
        const auto tracker = code_tracker.lock();
        if (!tracker) return;
        const auto invalidation =
            tracker->observe_write(event.address, event.size, event.source, event.bytes_changed);
        if (!invalidation.byte_identical) {
            if (const auto blocks = runtime_blocks.lock())
                static_cast<void>(blocks->erase_overlapping_physical(event.address, event.size));
        }
    });
    state.store_queue_transfers = std::make_shared<std::vector<StoreQueueTransfer>>();
    state.store_queue_transfers->reserve(1024u);
    state.dropped_store_queue_transfers = std::make_shared<std::uint64_t>(0u);
    const auto transfers = state.store_queue_transfers;
    const auto dropped_transfers = state.dropped_store_queue_transfers;
    auto* const memory = &cpu.memory;
    state.store_queues = std::make_shared<Sh4StoreQueues>(
        cpu.memory,
        [transfers, dropped_transfers, memory](const StoreQueueTransfer& transfer) {
            if (transfers->size() == 1024u) {
                transfers->erase(transfers->begin());
                if (*dropped_transfers != std::numeric_limits<std::uint64_t>::max()) {
                    ++*dropped_transfers;
                }
            }
            transfers->push_back(transfer);
            if (transfer.target != StoreQueueTarget::Ram) return;
            memory->write_bytes(
                transfer.target_address, transfer.bytes, CodeWriteSource::StoreQueue);
        },
        state.code_tracker.get());
    const auto queues = state.store_queues;
    state.cache_control = map_sh4_cache_control(cpu.memory);
    state.io_ports = map_sh4_io_ports(cpu.memory, {dreamcast_composite_port_a_input, 0u});
    auto queue_window = std::make_shared<MmioMemoryDevice>(
        0x04000000u,
        [](const std::uint32_t, const MemoryAccessWidth) -> std::uint32_t {
            throw std::invalid_argument(
                "Das Store-Queue-Schreibfenster unterstuetzt keine Lesezugriffe.");
        },
        [queues](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            queues->write_p4(Sh4StoreQueues::window_start + offset, value, width);
        });
    cpu.memory.map_region("sh4-store-queue-window", Sh4StoreQueues::window_start, queue_window);
    auto queue_read_window = std::make_shared<MmioMemoryDevice>(
        64u,
        [queues](const std::uint32_t offset, const MemoryAccessWidth width) {
            return queues->read_p4(Sh4StoreQueues::read_window_start + offset, width);
        },
        MmioWriteHandler{});
    cpu.memory.map_region(
        "sh4-store-queue-read-window", Sh4StoreQueues::read_window_start, queue_read_window);
    auto qacr = std::make_shared<MmioMemoryDevice>(
        8u,
        [queues](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word || (offset != 0u && offset != 4u)) {
                throw std::invalid_argument("QACR verlangt ausgerichtete 32-Bit-Zugriffe.");
            }
            return queues->qacr(offset / 4u);
        },
        [queues](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word || (offset != 0u && offset != 4u))
                throw std::invalid_argument("QACR verlangt ausgerichtete 32-Bit-Zugriffe.");
            queues->write_qacr(offset / 4u, value);
        });
    cpu.memory.map_region("sh4-qacr", 0xFF000038u, qacr);
    state.firmware_handoff = std::make_shared<FirmwareHandoffMap>();
    state.firmware_handoff->map_segment({"main-ram",
                                         FirmwareSegmentKind::Ram,
                                         0x8C000000u,
                                         0x0C000000u,
                                         static_cast<std::uint32_t>(dreamcast_main_ram_size)});
    if (firmware_mode == DreamcastRuntimeFirmwareMode::HleBiosAbi)
        install_hle_bios_abi(cpu.memory,
                             *state.runtime_blocks,
                             *state.firmware_handoff,
                             {},
                             0u,
                             state.code_tracker.get());
    cpu.memory.write_bytes(dreamcast_disc_boot_address, boot.boot_file, CodeWriteSource::Copy);
    state.loaded_boot_bytes = boot.boot_file.size();
    reset_cpu(cpu,
              ResetState{dreamcast_disc_boot_address,
                         dreamcast_direct_boot_stack,
                         0u,
                         dreamcast_disc_boot_status,
                         0u});
    return state;
}

} // namespace katana::runtime
