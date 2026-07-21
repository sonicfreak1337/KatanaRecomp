#include "katana/runtime/dreamcast_boot.hpp"

#include "katana/io/json_report.hpp"
#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <array>
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

std::uint16_t flash_block_crc(const std::span<const std::uint8_t> block) {
    std::uint16_t value = 0xFFFFu;
    for (std::size_t index = 0u; index < 62u; ++index) {
        value ^= static_cast<std::uint16_t>(block[index]) << 8u;
        for (unsigned bit = 0u; bit < 8u; ++bit)
            value = (value & 0x8000u) != 0u ? static_cast<std::uint16_t>((value << 1u) ^ 0x1021u)
                                            : static_cast<std::uint16_t>(value << 1u);
    }
    return static_cast<std::uint16_t>(~value);
}

void seed_flash_partition_header(PersistentImage& image,
                                 const std::size_t offset,
                                 const std::uint8_t partition) {
    std::array<std::uint8_t, 64u> header;
    header.fill(0xFFu);
    constexpr std::string_view magic = "KATANA_FLASH____";
    std::copy(magic.begin(), magic.end(), header.begin());
    header[16] = partition;
    header[17] = 0u;
    image.write(offset, header);
}

void seed_erased_dreamcast_flash(PersistentImage& image, const DreamcastRegion region) {
    if (!std::all_of(image.bytes().begin(), image.bytes().end(), [](const auto byte) {
            return byte == 0xFFu;
        }))
        return;

    const std::string_view factory = region == DreamcastRegion::Europe         ? "00211Dreamcast  "
                                     : region == DreamcastRegion::NorthAmerica ? "00110Dreamcast  "
                                                                               : "00000Dreamcast  ";
    const auto factory_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(factory.data()), factory.size());
    image.write(0x1A000u, factory_bytes);
    image.write(0x1A0A0u, factory_bytes);
    seed_flash_partition_header(image, 0x00000u, 4u);
    seed_flash_partition_header(image, 0x10000u, 3u);
    seed_flash_partition_header(image, 0x1C000u, 2u);

    std::array<std::uint8_t, 64u> syscfg;
    syscfg.fill(0xFFu);
    syscfg[0] = 0x05u;
    syscfg[1] = 0x00u;
    syscfg[2] = syscfg[3] = syscfg[4] = syscfg[5] = 0u;
    syscfg[6] = 0u;
    syscfg[7] = 1u;
    syscfg[8] = 0u;
    syscfg[9] = 0u;
    const auto crc = flash_block_crc(syscfg);
    syscfg[62] = static_cast<std::uint8_t>(crc);
    syscfg[63] = static_cast<std::uint8_t>(crc >> 8u);
    image.write(0x1C040u, syscfg);
    std::array<std::uint8_t, 64u> bitmap;
    bitmap.fill(0xFFu);
    bitmap[0] = 0x7Fu;
    image.write(0x1FFC0u, bitmap);
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

DreamcastRegion dreamcast_region_from_area_symbols(const std::string_view area_symbols) noexcept {
    if (area_symbols.find('E') != std::string_view::npos) return DreamcastRegion::Europe;
    if (area_symbols.find('U') != std::string_view::npos) return DreamcastRegion::NorthAmerica;
    return DreamcastRegion::Japan;
}

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
    seed_erased_dreamcast_flash(*flash, config.region);
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
    const auto area_symbols = trimmed_ascii(boot_sector, 0x30u, 8u);
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
            area_symbols,
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
    map_sh4_tmu_registers(cpu.memory, state.tmu);
    map_sh4_rtc_registers(cpu.memory, state.rtc);
    state.dmac = std::make_shared<Sh4Dmac>(
        *state.scheduler, cpu.memory, DmaTiming{}, DmaExecutionMode::DeterministicBatch);
    map_sh4_dmac_registers(cpu.memory, state.dmac);
    state.address_space = std::make_shared<RuntimeAddressSpace>();
    cpu.address_space = state.address_space;
    state.mmu_control = map_sh4_mmu_control(cpu.memory, cpu, *state.address_space);
    state.interrupt_controller = std::make_shared<InterruptController>();
    state.interrupt_router = std::make_shared<PlatformInterruptRouter>(
        *state.interrupt_controller, *state.tmu, *state.rtc, *state.dmac);
    state.interrupt_registers = map_sh4_interrupt_registers(cpu.memory, *state.interrupt_router);
    const auto channel2_dmac = std::weak_ptr<Sh4Dmac>(state.dmac);
    state.system_bus_control = map_dreamcast_system_bus_control(
        cpu.memory, [channel2_dmac](const std::uint32_t destination, const std::uint32_t length) {
            const auto dmac = channel2_dmac.lock();
            if (!dmac) throw std::runtime_error("Systembus-Channel-2-DMAC-Lebenszyklus fehlt.");
            constexpr std::size_t channel = 2u;
            constexpr std::size_t unit_size = 32u;
            const auto control = dmac->control(channel);
            if (dmac->transfer_unit_size(channel) != unit_size ||
                (control & 0x00000F00u) != 0x00000800u ||
                (control & 0x00003000u) != 0x00001000u ||
                (control & 0x0000C000u) != 0u ||
                (control & Sh4Dmac::channel_enable) == 0u ||
                (dmac->operation() & Sh4Dmac::master_enable) == 0u) {
                throw std::runtime_error(
                    "Systembus-Channel-2 braucht aktivierten externen 32-Byte-DMAC mit "
                    "inkrementierender Quelle und festem TA-Ziel.");
            }
            if ((length % unit_size) != 0u)
                throw std::invalid_argument(
                    "Systembus-Channel-2-Laenge muss ein Vielfaches von 32 Byte sein.");
            const auto units = length / static_cast<std::uint32_t>(unit_size);
            dmac->write_destination(channel, destination);
            dmac->write_count(channel, units);
            dmac->request_transfer(channel, units);
        });
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
    const auto channel2_control = std::weak_ptr<DreamcastSystemBusControl>(state.system_bus_control);
    state.dmac->set_completion_observer([channel2_control, raise_now](const std::size_t channel) {
        if (channel != 2u) return;
        const auto control = channel2_control.lock();
        if (!control) throw std::runtime_error("Systembus-Channel-2-Lebenszyklus fehlt.");
        control->complete_channel2();
        raise_now(SystemAsicEvent::PvrDma);
    });
    state.pvr_ta_fifo = std::make_shared<PvrTaFifo>([raise_now](const PvrListType list) {
        switch (list) {
        case PvrListType::Opaque:
            raise_now(SystemAsicEvent::PvrOpaqueList);
            return;
        case PvrListType::OpaqueModifier:
            raise_now(SystemAsicEvent::PvrOpaqueModifierList);
            return;
        case PvrListType::PunchThrough:
            raise_now(SystemAsicEvent::PvrPunchThroughList);
            return;
        case PvrListType::Translucent:
            raise_now(SystemAsicEvent::PvrTranslucentList);
            return;
        case PvrListType::TranslucentModifier:
            raise_now(SystemAsicEvent::PvrTranslucentModifierList);
            return;
        }
        throw std::logic_error("Ungueltiger TA-Listentyp.");
    });
    state.pvr_renderer = std::make_shared<PvrSoftwareRenderer>();
    state.pvr_registers = map_pvr_registers(
        cpu.memory,
        *state.scheduler,
        {},
        {},
        [raise_now](const bool entering) {
            raise_now(entering ? SystemAsicEvent::PvrVblank : SystemAsicEvent::PvrVblankOut);
        });
    configure_dreamcast_video(
        *state.pvr_registers,
        dreamcast_region_from_area_symbols(boot.area_symbols) == DreamcastRegion::Europe
            ? DreamcastVideoMode::PalInterlaced
            : DreamcastVideoMode::NtscInterlaced);
    state.pvr_ta_aperture =
        std::make_shared<PvrTaFifoMemoryDevice>(state.pvr_ta_fifo, state.pvr_registers);
    const auto reset_ta_fifo = std::weak_ptr<PvrTaFifo>(state.pvr_ta_fifo);
    state.pvr_registers->set_ta_reset_observer([reset_ta_fifo] {
        if (const auto fifo = reset_ta_fifo.lock()) fifo->reset();
    });
    state.pvr_yuv_converter = std::make_shared<PvrYuvConverterMemoryDevice>(
        state.pvr_registers,
        state.vram,
        [raise_now] { raise_now(SystemAsicEvent::PvrYuvDone); });
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto ta_base = segment + 0x10000000u;
        const auto ta_mirror = segment + 0x12000000u;
        const auto yuv_base = segment + 0x10800000u;
        const auto yuv_mirror = segment + 0x12800000u;
        cpu.memory.map_region(
            "dreamcast-ta-fifo-" + std::to_string(ta_base), ta_base, state.pvr_ta_aperture);
        cpu.memory.map_region("dreamcast-ta-fifo-" + std::to_string(ta_mirror),
                              ta_mirror,
                              state.pvr_ta_aperture);
        cpu.memory.map_region(
            "dreamcast-ta-yuv-" + std::to_string(yuv_base), yuv_base, state.pvr_yuv_converter);
        cpu.memory.map_region("dreamcast-ta-yuv-" + std::to_string(yuv_mirror),
                              yuv_mirror,
                              state.pvr_yuv_converter);
    }
    map_dreamcast_ta_vram_aliases(cpu.memory, state.vram);
    const auto ta_fifo = std::weak_ptr<PvrTaFifo>(state.pvr_ta_fifo);
    const auto pvr_registers = std::weak_ptr<PvrRegisterFile>(state.pvr_registers);
    const auto pvr_renderer = std::weak_ptr<PvrSoftwareRenderer>(state.pvr_renderer);
    const auto vram = std::weak_ptr<LinearMemoryDevice>(state.vram);
    state.pvr_registers->set_render_observer(
        [ta_fifo, pvr_registers, pvr_renderer, vram, raise_now] {
            const auto fifo = ta_fifo.lock();
            const auto registers = pvr_registers.lock();
            const auto renderer = pvr_renderer.lock();
            const auto target = vram.lock();
            if (!fifo || !registers || !renderer || !target) {
                if (renderer)
                    renderer->record_error(PvrRenderError::InternalLifecycle,
                                           registers ? registers->render_request_count() : 0u,
                                           "PVR-Renderpfad-Lebenszyklus fehlt.");
                return;
            }
            const auto request = registers->render_request_count();
            bool rendered = false;
            try {
                auto frame = fifo->finish_frame();
                if (frame.modifier_volumes_present) {
                    renderer->record_error(PvrRenderError::UnsupportedFeature,
                                           request,
                                           "PVR-Modifier-Volumes wurden erfasst, aber nicht gerastert.");
                } else {
                    renderer->render(frame, *registers, *target);
                    rendered = true;
                }
            } catch (const std::out_of_range& error) {
                renderer->record_error(PvrRenderError::MemoryRange, request, error.what());
            } catch (const std::invalid_argument& error) {
                renderer->record_error(PvrRenderError::InvalidConfiguration, request, error.what());
            } catch (const std::logic_error& error) {
                renderer->record_error(PvrRenderError::InvalidTaState, request, error.what());
            } catch (const std::runtime_error& error) {
                renderer->record_error(PvrRenderError::UnsupportedFeature, request, error.what());
            } catch (...) {
                renderer->record_error(PvrRenderError::InternalLifecycle,
                                       request,
                                       "Unbekannter Fehler an der PVR-Produktgrenze.");
            }
            if (rendered) raise_now(SystemAsicEvent::PvrRenderDone);
        });
    state.aica = std::make_shared<AicaExecutionController>(state.scheduler.get());
    state.aica->interrupts().set_observer(
        [raise_now] { raise_now(SystemAsicEvent::AicaInterrupt); });
    state.aica_registers = map_aica_registers(cpu.memory, state.aica, state.aica_ram);
    state.maple = std::make_shared<MapleBus>([raise_now] { raise_now(SystemAsicEvent::MapleDma); });
    state.maple_controller =
        map_dreamcast_maple_controller(cpu.memory, *state.scheduler, state.maple, {}, [raise_now] {
            raise_now(SystemAsicEvent::MapleDma);
        });
    const auto maple_controller = std::weak_ptr<DreamcastMapleController>(state.maple_controller);
    state.pvr_registers->set_vblank_observer([raise_now, maple_controller](const bool entering) {
        raise_now(entering ? SystemAsicEvent::PvrVblank : SystemAsicEvent::PvrVblankOut);
        if (entering) {
            if (const auto maple = maple_controller.lock()) maple->hardware_trigger();
        }
    });
    if (state.mutable_storage) {
        state.vmu = std::make_shared<MapleVmuDevice>(state.mutable_storage->vmu_image());
        state.maple->attach(0u, 1u, state.vmu);
    }
    state.runtime_blocks = std::make_shared<RuntimeBlockTable>();
    state.code_tracker = std::make_shared<ExecutableCodeTracker>();
    state.runtime_blocks->bind_code_tracker(state.code_tracker.get());
    state.module_catalog = std::make_shared<ExecutableModuleCatalog>();
    const auto module_sequence = std::make_shared<std::uint64_t>(1u);
    const auto module_catalog = state.module_catalog;
    const auto module_blocks = state.runtime_blocks;
    const auto module_tracker = state.code_tracker;
    state.gdrom = map_dreamcast_gdrom(
        cpu.memory,
        *state.scheduler,
        GdRomDrive(boot.source),
        [asic, scheduler](const std::uint64_t cycle) {
            const auto target = asic.lock();
            const auto clock = scheduler.lock();
            if (!target || !clock)
                throw std::runtime_error("Dreamcast-System-ASIC-Lebenszyklus fehlt.");
            static_cast<void>(target->schedule(*clock, SystemAsicEvent::GdromCommand, cycle));
        },
        [module_sequence, module_catalog, module_blocks, module_tracker](
            const std::uint32_t destination,
            const std::span<const std::uint8_t> bytes,
            const std::string_view source_identity) {
            if (bytes.empty() || (destination & 1u) != 0u) return;
            const auto physical = canonical_physical_address(destination);
            const auto module_address =
                physical >= 0x0C000000u && physical < 0x0D000000u ? physical | 0x80000000u
                                                                  : destination;
            ExecutableModule module;
            module.id = "gdrom-load-" + std::to_string((*module_sequence)++);
            module.source_identity = std::string(source_identity);
            module.guest_start = module_address;
            module.bytes.assign(bytes.begin(), bytes.end());
            module.kind = ExecutableModuleKind::Overlay;
            module.executable_permission = false;
            module.control_transfer_promotion_allowed = true;
            module.range_roles.push_back(
                {0u,
                 static_cast<std::uint32_t>(module.bytes.size()),
                 ExecutableStorageRole::ProvenData});
            module_catalog->publish_loaded_range(
                std::move(module), *module_blocks, *module_tracker);
        });
    cpu.gdrom_services = state.gdrom.get();
    const auto gdrom = std::weak_ptr<DreamcastGdRomController>(state.gdrom);
    state.holly_dma = map_dreamcast_holly_dma(
        cpu.memory,
        *state.scheduler,
        {},
        [raise_now](const auto event) { raise_now(event); },
        [gdrom](const auto address, const auto length, const auto direction) {
            const auto controller = gdrom.lock();
            if (!controller) throw std::runtime_error("G1-GD-ROM-Lebenszyklus fehlt.");
            controller->dma_to_memory(address, length, direction);
        });
    const auto g2_dma = std::weak_ptr<DreamcastG2DmaController>(state.holly_dma.g2);
    const auto pvr_dma = std::weak_ptr<DreamcastPvrDmaController>(state.holly_dma.pvr);
    state.system_asic->set_dma_trigger_observers(
        [pvr_dma](const SystemAsicEvent) {
            if (const auto controller = pvr_dma.lock()) controller->hardware_trigger();
        },
        [g2_dma](const SystemAsicEvent event) {
            if (const auto controller = g2_dma.lock()) controller->interrupt_trigger(event);
        });
    state.aica->set_dma_request_observer([g2_dma] {
        if (const auto controller = g2_dma.lock()) controller->hardware_trigger(0u);
    });
    const auto code_tracker = std::weak_ptr<ExecutableCodeTracker>(state.code_tracker);
    const auto runtime_blocks = std::weak_ptr<RuntimeBlockTable>(state.runtime_blocks);
    cpu.memory.set_guest_write_observer([code_tracker,
                                         runtime_blocks](const GuestWriteEvent& event) {
        const auto tracker = code_tracker.lock();
        if (!tracker) return;
        const auto invalidation =
            tracker->observe_write(canonical_physical_address(event.address),
                                   event.size,
                                   event.source,
                                   event.bytes_changed);
        if (!invalidation.byte_identical) {
            if (const auto blocks = runtime_blocks.lock())
                static_cast<void>(blocks->erase_overlapping_physical(
                    canonical_physical_address(event.address), event.size));
        }
    });
    state.store_queue_transfers = std::make_shared<std::vector<StoreQueueTransfer>>();
    state.store_queue_transfers->reserve(1024u);
    state.dropped_store_queue_transfers = std::make_shared<std::uint64_t>(0u);
    const auto transfers = state.store_queue_transfers;
    const auto dropped_transfers = state.dropped_store_queue_transfers;
    const auto store_queue_ta = state.pvr_ta_fifo;
    auto* const memory = &cpu.memory;
    state.store_queues = std::make_shared<Sh4StoreQueues>(
        cpu.memory,
        [transfers, dropped_transfers, memory, store_queue_ta](const StoreQueueTransfer& transfer) {
            if (transfers->size() == 1024u) {
                transfers->erase(transfers->begin());
                if (*dropped_transfers != std::numeric_limits<std::uint64_t>::max()) {
                    ++*dropped_transfers;
                }
            }
            transfers->push_back(transfer);
            if (transfer.target == StoreQueueTarget::TileAccelerator) {
                store_queue_ta->submit(transfer.bytes);
            } else {
                memory->write_bytes(
                    transfer.target_address, transfer.bytes, CodeWriteSource::StoreQueue);
            }
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
    if (firmware_mode == DreamcastRuntimeFirmwareMode::HleBiosAbi) {
        install_hle_bios_abi(cpu.memory,
                             *state.runtime_blocks,
                             *state.firmware_handoff,
                             {},
                             0u,
                             state.code_tracker.get());
    }
    cpu.memory.write_bytes(dreamcast_disc_boot_address, boot.boot_file, CodeWriteSource::Copy);
    state.loaded_boot_bytes = boot.boot_file.size();
    reset_cpu(cpu,
              ResetState{dreamcast_disc_boot_address,
                         dreamcast_direct_boot_stack,
                         dreamcast_direct_boot_vector_base,
                         dreamcast_disc_boot_status,
                         0u});
    return state;
}

} // namespace katana::runtime
