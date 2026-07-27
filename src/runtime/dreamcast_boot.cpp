#include "katana/runtime/dreamcast_boot.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/iso9660.hpp"
#include "katana/runtime/runtime_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace katana::runtime {
namespace {

class DreamcastManualResetCoordinator final : public ManualResetCoordinator {
  public:
    explicit DreamcastManualResetCoordinator(std::function<void(CpuState&)> reset)
        : reset_(std::move(reset)) {}

    void reset(CpuState& cpu, const ManualResetRequest&) noexcept override {
        try {
            reset_(cpu);
        } catch (...) {
            reset_cpu(cpu,
                      ResetState{tlb_multiple_hit_reset_vector,
                                 0u,
                                 0u,
                                 sr_md_mask | sr_rb_mask | sr_bl_mask | sr_interrupt_mask,
                                 0u});
        }
    }

  private:
    std::function<void(CpuState&)> reset_;
};

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

bool direct_boot_main_ram_address(const std::uint32_t address,
                                  const bool allow_top = false) noexcept {
    constexpr std::uint32_t begin = 0x8C000000u;
    constexpr std::uint32_t end = begin + static_cast<std::uint32_t>(dreamcast_main_ram_size);
    return address >= begin && (address < end || (allow_top && address == end));
}

bool valid_channel2_main_ram_range(const std::uint32_t source,
                                   const std::uint32_t length) noexcept {
    constexpr std::uint32_t area3_begin = 0x0C000000u;
    constexpr std::uint32_t area3_end = 0x10000000u;
    constexpr std::uint32_t transfer_alignment = 32u;
    return source >= area3_begin && source < area3_end && length != 0u &&
           (source & (transfer_alignment - 1u)) == 0u &&
           (length & (transfer_alignment - 1u)) == 0u && length <= area3_end - source;
}

std::string pvr_ta_packet_class(const std::span<const std::uint8_t> packet) {
    if (packet.size() < sizeof(std::uint32_t)) return "truncated";
    const auto parameter_type = static_cast<std::uint8_t>(packet[3u] >> 5u);
    constexpr std::array<std::string_view, 8u> names{
        "end-of-list",
        "user-clip",
        "object-list-set",
        "reserved-parameter-3",
        "polygon-header",
        "sprite-header",
        "reserved-parameter-6",
        "vertex",
    };
    return std::string(names[parameter_type]);
}

std::string_view pvr_ta_error_class(const PvrTaInputErrorReason reason) noexcept {
    switch (reason) {
    case PvrTaInputErrorReason::InvalidPacket:
        return "invalid-packet";
    case PvrTaInputErrorReason::UnsupportedPacket:
        return "unsupported-packet";
    case PvrTaInputErrorReason::InvalidListOrder:
        return "invalid-list-order";
    case PvrTaInputErrorReason::IncompletePacket:
        return "incomplete-packet";
    case PvrTaInputErrorReason::BufferOverflow:
        return "buffer-overflow";
    }
    return "unknown";
}

PvrRegisterFile::RenderJob failed_pvr_render_job(const PvrRenderError error,
                                                  std::string ta_packet_class,
                                                  std::string detail) {
    return [error,
            ta_packet_class = std::move(ta_packet_class),
            detail = std::move(detail)]() -> PvrRenderResult {
        throw PvrRenderJobError(error, ta_packet_class, detail);
    };
}

template <typename Operation>
void for_each_dreamcast_executable_backing_extent(const std::uint32_t address,
                                                  const std::size_t size,
                                                  Operation&& operation) {
    constexpr std::uint32_t area3_begin = 0x0C000000u;
    constexpr std::uint32_t area3_end = 0x10000000u;
    constexpr std::uint32_t backing_mask =
        static_cast<std::uint32_t>(dreamcast_main_ram_size - 1u);
    const auto physical = canonical_physical_address(address);
    if (size != 0u && physical >= area3_begin && physical < area3_end &&
        size <= static_cast<std::uint64_t>(area3_end) - physical) {
        std::size_t source_offset = 0u;
        auto cursor = physical;
        while (source_offset < size) {
            const auto backing_offset = (cursor - area3_begin) & backing_mask;
            const auto extent_size = std::min<std::size_t>(
                size - source_offset, dreamcast_main_ram_size - backing_offset);
            operation(area3_begin + backing_offset, source_offset, extent_size, true);
            source_offset += extent_size;
            cursor += static_cast<std::uint32_t>(extent_size);
        }
        return;
    }
    operation(physical, 0u, size, false);
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

void reset_dreamcast_direct_boot_cpu_state(
    CpuState& cpu,
    const DreamcastPostBiosCpuState& state) noexcept {
    reset_cpu(
        cpu,
        ResetState{
            state.entry_point, state.stack_pointer, state.vector_base, state.status, state.fpscr});
    cpu.gbr = state.gbr;
    cpu.ssr = state.ssr;
    cpu.spc = state.spc;
    cpu.sgr = state.sgr;
    cpu.dbr = state.dbr;
    cpu.pr = state.pr;
}

} // namespace

DreamcastRegion
dreamcast_region_for_console_profile(const DreamcastConsoleProfile profile) noexcept {
    switch (profile) {
    case DreamcastConsoleProfile::JapanNtsc:
        return DreamcastRegion::Japan;
    case DreamcastConsoleProfile::NorthAmericaNtsc:
        return DreamcastRegion::NorthAmerica;
    case DreamcastConsoleProfile::EuropePal:
        return DreamcastRegion::Europe;
    case DreamcastConsoleProfile::Vga:
        return DreamcastRegion::Japan;
    }
    return DreamcastRegion::Japan;
}

std::string_view
dreamcast_console_profile_name(const DreamcastConsoleProfile profile) noexcept {
    switch (profile) {
    case DreamcastConsoleProfile::JapanNtsc:
        return "japan-ntsc";
    case DreamcastConsoleProfile::NorthAmericaNtsc:
        return "north-america-ntsc";
    case DreamcastConsoleProfile::EuropePal:
        return "europe-pal";
    case DreamcastConsoleProfile::Vga:
        return "vga";
    }
    return "unknown";
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
    const auto content_identity = packed_disc_content_identity(*source);
    return load_dreamcast_runtime_boot(
        std::move(source), data_track_lba, validated_tracks, content_identity);
}

DreamcastRuntimeBootImage
load_dreamcast_runtime_boot_from_pack(const std::filesystem::path& pack_path) {
    if (pack_path.empty()) {
        throw std::invalid_argument("Dreamcast-Runtime braucht einen Katana-Disc-Pack.");
    }
    auto source = PackedDiscSource::open(pack_path);
    const auto data_track_lba = source->primary_data_lba();
    const auto validated_tracks = source->info().tracks.size();
    const auto content_identity = source->info().content_identity;
    return load_dreamcast_runtime_boot(
        std::move(source), data_track_lba, validated_tracks, content_identity);
}

DreamcastRuntimeBootImage load_dreamcast_runtime_boot(std::shared_ptr<DiscSource> source,
                                                       const std::uint32_t data_track_lba,
                                                       const std::size_t validated_tracks,
                                                       std::string content_identity) {
    if (!source || validated_tracks == 0u) {
        throw std::invalid_argument("Dreamcast-Runtime-Discquelle ist unvollstaendig.");
    }
    if (data_track_lba > std::numeric_limits<std::uint64_t>::max() / 2048u) {
        throw std::out_of_range("Dreamcast-Bootsektoroffset laeuft ueber.");
    }
    const auto boot_sector = source->read(static_cast<std::uint64_t>(data_track_lba) * 2048u, 256u);
    const auto bootstrap_offset = static_cast<std::uint64_t>(data_track_lba) * 2048u;
    auto system_bootstrap = source->read(bootstrap_offset, dreamcast_system_bootstrap_size);
    const auto repeated_system_bootstrap =
        source->read(bootstrap_offset, dreamcast_system_bootstrap_size);
    const auto hardware_id = trimmed_ascii(boot_sector, 0x00u, 16u);
    const auto disc_type = trimmed_ascii(boot_sector, 0x25u, 6u);
    const auto area_symbols = trimmed_ascii(boot_sector, 0x30u, 8u);
    const auto boot_file_name = trimmed_ascii(boot_sector, 0x60u, 16u);
    if (hardware_id != "SEGA SEGAKATANA") {
        throw std::invalid_argument("Dreamcast-Hardwarekennung ist ungueltig.");
    }
    if (!safe_iso_file_name(boot_file_name)) {
        throw std::invalid_argument("Dreamcast-Bootdateiname ist ungueltig.");
    }
    if (boot_sector.at(0x3Eu) == static_cast<std::uint8_t>('1') ||
        boot_file_name == "0WINCEOS.BIN") {
        throw std::runtime_error("unsupported-dreamcast-wince-boot-layout");
    }
    if (!disc_type.empty() && disc_type != "GD-ROM") {
        throw std::runtime_error("unsupported-scrambled-non-gdrom-boot-layout");
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
    const bool repeated_bootstrap_reads_match =
        repeated_system_bootstrap.size() == dreamcast_system_bootstrap_size &&
        repeated_system_bootstrap == system_bootstrap;
    const bool repeated_reads_match = repeated == boot_file;
    return {std::move(source),
            hardware_id,
            area_symbols,
            boot_file_name,
            std::move(system_bootstrap),
            std::move(boot_file),
            data_track_lba,
             selected_bias,
             validated_tracks,
             repeated_bootstrap_reads_match,
             repeated_reads_match,
             std::move(content_identity)};
}

std::string
dreamcast_boot_executable_byte_identity(const DreamcastRuntimeBootImage& boot) {
    if (boot.boot_file.empty())
        throw std::invalid_argument("direct-boot-executable-empty");
    return disc_load_byte_identity(boot.boot_file);
}

void validate_dreamcast_post_bios_cpu_state(
    const DreamcastPostBiosCpuState& state,
    const std::size_t boot_executable_size) {
    if (state.contract_version != dreamcast_post_bios_cpu_contract_version)
        throw std::invalid_argument("direct-boot-post-bios-contract-unsupported");
    if (boot_executable_size == 0u ||
        boot_executable_size >
            dreamcast_main_ram_size -
                static_cast<std::size_t>(
                    dreamcast_disc_boot_address - 0x8C000000u))
        throw std::invalid_argument("direct-boot-executable-size-invalid");
    const auto entry_offset =
        static_cast<std::uint64_t>(state.entry_point) -
        static_cast<std::uint64_t>(dreamcast_disc_boot_address);
    if ((state.entry_point & 1u) != 0u ||
        state.entry_point < dreamcast_disc_boot_address ||
        entry_offset >= boot_executable_size)
        throw std::invalid_argument("direct-boot-entry-outside-executable");
    if ((state.stack_pointer & 3u) != 0u ||
        !direct_boot_main_ram_address(state.stack_pointer, true))
        throw std::invalid_argument("direct-boot-stack-invalid");
    if ((state.vector_base & 3u) != 0u ||
        !direct_boot_main_ram_address(state.vector_base))
        throw std::invalid_argument("direct-boot-vector-base-invalid");
    if ((state.status & ~sr_writable_mask) != 0u ||
        (state.status & sr_md_mask) == 0u ||
        (state.ssr & ~sr_writable_mask) != 0u)
        throw std::invalid_argument("direct-boot-status-invalid");
    if ((state.fpscr & ~fpscr_writable_mask) != 0u)
        throw std::invalid_argument("direct-boot-fpscr-invalid");
    if ((state.spc & 1u) != 0u || (state.pr & 1u) != 0u ||
        (state.sgr & 3u) != 0u || (state.dbr & 3u) != 0u)
        throw std::invalid_argument("direct-boot-control-register-invalid");
    if (!direct_boot_main_ram_address(state.gbr) ||
        !direct_boot_main_ram_address(state.spc) ||
        !direct_boot_main_ram_address(state.sgr, true) ||
        !direct_boot_main_ram_address(state.dbr))
        throw std::invalid_argument("direct-boot-register-base-invalid");
}

void validate_dreamcast_runtime_boot_config(
    const DreamcastRuntimeBootImage& boot,
    const DreamcastRuntimeBootConfig& boot_config) {
    if (boot_config.post_bios_platform_contract_version !=
        dreamcast_post_bios_platform_contract_version)
        throw std::invalid_argument(
            "dreamcast-post-bios-platform-contract-unsupported");
    validate_dreamcast_post_bios_cpu_state(
        boot_config.post_bios_cpu_state, boot.boot_file.size());

    switch (boot_config.firmware_mode) {
    case DreamcastRuntimeFirmwareMode::Direct:
    case DreamcastRuntimeFirmwareMode::HleBiosAbi:
        break;
    default:
        throw std::invalid_argument("dreamcast-firmware-mode-invalid");
    }
    switch (boot_config.boot_path) {
    case DreamcastRuntimeBootPath::NativeDiscBoot:
        if (boot_config.firmware_mode != DreamcastRuntimeFirmwareMode::HleBiosAbi)
            throw std::invalid_argument("native-disc-boot-requires-hle-bios-abi");
        break;
    case DreamcastRuntimeBootPath::DirectBootExecutable:
        break;
    default:
        throw std::invalid_argument("dreamcast-boot-path-invalid");
    }

    const auto& identity = boot_config.executable_identity;
    if (!identity.content_identity.empty() &&
        identity.content_identity != boot.content_identity)
        throw std::invalid_argument("direct-boot-content-identity-mismatch");
    if (!identity.boot_file_name.empty() &&
        identity.boot_file_name != boot.boot_file_name)
        throw std::invalid_argument("direct-boot-file-name-mismatch");
    if (!identity.boot_byte_identity.empty() &&
        identity.boot_byte_identity !=
            dreamcast_boot_executable_byte_identity(boot))
        throw std::invalid_argument("direct-boot-byte-identity-mismatch");
}

DreamcastRuntimeState
initialize_dreamcast_runtime(CpuState& cpu,
                             const DreamcastRuntimeBootImage& boot,
                             const DreamcastRuntimeBootConfig& boot_config,
                             std::shared_ptr<DreamcastMutableStorage> mutable_storage,
                             const DreamcastConsoleProfile console_profile) {
    if (!boot.source || boot.content_identity.empty() ||
        boot.system_bootstrap.size() != dreamcast_system_bootstrap_size ||
        boot.boot_file.empty() || !boot.repeated_bootstrap_reads_match ||
        !boot.repeated_reads_match) {
        throw std::invalid_argument("Dreamcast-Runtime-Bootimage ist unvollstaendig.");
    }
    validate_dreamcast_runtime_boot_config(boot, boot_config);
    const auto boot_region = dreamcast_region_for_console_profile(console_profile);
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
    map_sh4_exception_event_registers(cpu.memory, cpu);
    state.interrupt_controller = std::make_shared<InterruptController>();
    state.interrupt_router = std::make_shared<PlatformInterruptRouter>(
        *state.interrupt_controller, *state.tmu, *state.rtc, *state.dmac);
    state.interrupt_registers = map_sh4_interrupt_registers(cpu.memory, *state.interrupt_router);
    const auto scif_router = state.interrupt_router;
    state.scif = map_sh4_scif(
        cpu.memory,
        *state.scheduler,
        [scif_router](const Sh4ScifInterrupt source, const bool pending) {
            scif_router->set_scif_pending(static_cast<std::size_t>(source), pending);
        });
    const auto channel2_dmac = std::weak_ptr<Sh4Dmac>(state.dmac);
    state.system_bus_control = map_dreamcast_system_bus_control(
        cpu.memory, [channel2_dmac](const std::uint32_t destination, const std::uint32_t length) {
            const auto dmac = channel2_dmac.lock();
            if (!dmac) throw std::runtime_error("Systembus-Channel-2-DMAC-Lebenszyklus fehlt.");
            constexpr std::size_t channel = 2u;
            constexpr std::size_t unit_size = 32u;
            constexpr std::uint32_t external_memory_to_device = 0x00000200u;
            constexpr std::uint32_t burst_transmit = 0x00000080u;
            PvrChannel2DestinationPlan destination_plan;
            try {
                destination_plan = plan_pvr_channel2_destination(destination, length);
            } catch (const std::out_of_range&) {
                dmac->report_external_fault(
                    channel, DmaFaultReason::MemoryAccess, unit_size);
                return;
            } catch (const std::invalid_argument&) {
                dmac->report_external_fault(
                    channel, DmaFaultReason::ExternalContractMismatch, unit_size);
                return;
            }
            const auto control = dmac->control(channel);
            const auto operation = dmac->operation();
            const auto source = dmac->source(channel) & 0x1FFFFFFFu;
            const bool source_is_main_ram = valid_channel2_main_ram_range(source, length);
            if (dmac->transfer_unit_size(channel) != unit_size ||
                (control & 0x00000F00u) != external_memory_to_device ||
                (control & 0x00003000u) != 0x00001000u ||
                (control & 0x0000C000u) != 0u ||
                (control & burst_transmit) == 0u ||
                (control & Sh4Dmac::channel_enable) == 0u ||
                (operation & Sh4Dmac::master_enable) == 0u ||
                (operation & Sh4Dmac::on_demand_enable) == 0u ||
                (length % unit_size) != 0u || !source_is_main_ram) {
                dmac->report_external_fault(
                    channel, DmaFaultReason::ExternalContractMismatch, unit_size);
                return;
            }
            const auto units = length / static_cast<std::uint32_t>(unit_size);
            if (!dmac->validate_external_transfer(
                    channel, dmac->source(channel), length, unit_size, 2u))
                return;
            dmac->write_destination(channel, destination_plan.initial_address);
            dmac->request_transfer(
                channel,
                units,
                destination_plan.destination_progresses()
                    ? DmaExternalDestinationProgression::IncrementByTransferUnit
                    : DmaExternalDestinationProgression::AddressMode);
        });
    state.system_asic =
        map_dreamcast_system_asic(cpu.memory, *state.interrupt_router, &state.system_asic_device);
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
        raise_now(SystemAsicEvent::Channel2Dma);
    });
    state.dmac->set_fault_observer(
        [channel2_control, raise_now](const DmaFault& fault) {
            if (fault.channel != 2u) return;
            if (const auto control = channel2_control.lock()) control->complete_channel2();
            raise_now(SystemAsicEvent::PvrIllegalAddress);
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
    state.pvr_renderer->set_guest_memory_access_memory(&cpu.memory);
    state.pvr_registers = map_pvr_registers(
        cpu.memory,
        *state.scheduler,
        {},
        {},
        [raise_now](const bool entering) {
            raise_now(entering ? SystemAsicEvent::PvrVblank : SystemAsicEvent::PvrVblankOut);
        });
    const auto video_mode = console_profile == DreamcastConsoleProfile::EuropePal
                                ? DreamcastVideoMode::PalInterlaced
                            : console_profile == DreamcastConsoleProfile::Vga
                                ? DreamcastVideoMode::Vga
                                : DreamcastVideoMode::NtscInterlaced;
    configure_dreamcast_video(*state.pvr_registers, video_mode);
    state.pvr_ta_aperture =
        std::make_shared<PvrTaFifoMemoryDevice>(state.pvr_ta_fifo, state.pvr_registers);
    const auto reset_ta_fifo = std::weak_ptr<PvrTaFifo>(state.pvr_ta_fifo);
    const auto reset_ta_aperture =
        std::weak_ptr<PvrTaFifoMemoryDevice>(state.pvr_ta_aperture);
    state.pvr_registers->set_ta_reset_observer([reset_ta_fifo, reset_ta_aperture] {
        if (const auto fifo = reset_ta_fifo.lock()) fifo->reset();
        if (const auto aperture = reset_ta_aperture.lock()) aperture->reset();
    });
    state.pvr_registers->set_ta_continue_observer([reset_ta_fifo] {
        if (const auto fifo = reset_ta_fifo.lock()) fifo->continue_list();
    });
    state.pvr_yuv_converter = std::make_shared<PvrYuvConverterMemoryDevice>(
        state.pvr_registers,
        state.vram,
        [raise_now] { raise_now(SystemAsicEvent::PvrYuvDone); });
    state.pvr_yuv_converter->set_guest_memory_access_memory(&cpu.memory);
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
    state.pvr_registers->set_render_job_factory(
        [ta_fifo, pvr_renderer, vram, raise_now](
            const PvrRegisterSnapshot& register_snapshot,
            const std::uint64_t request,
            const std::uint64_t generation,
            const std::uint64_t) -> PvrRegisterFile::PreparedRenderJob {
            static_assert(std::is_nothrow_move_assignable_v<PvrTaFifo>);
            const auto fifo = ta_fifo.lock();
            const auto renderer = pvr_renderer.lock();
            const auto target = vram.lock();
            if (!fifo || !renderer || !target) {
                constexpr std::string_view detail = "PVR-Renderpfad-Lebenszyklus fehlt.";
                if (renderer)
                    renderer->record_error(PvrRenderError::InternalLifecycle,
                                           request,
                                           std::string(detail));
                return {failed_pvr_render_job(
                            PvrRenderError::InternalLifecycle, "none", std::string(detail)),
                        {}};
            }
            auto staged_fifo = *fifo;
            const auto payload_digest =
                hash_pvr_render_payload(register_snapshot, staged_fifo.snapshot());
            PvrTaFrame frame;
            try {
                frame = staged_fifo.finish_frame();
            } catch (const PvrTaParserException& error) {
                renderer->record_error(PvrRenderError::InvalidTaState, request, error.what());
                return {failed_pvr_render_job(PvrRenderError::InvalidTaState,
                                              std::string(pvr_ta_error_class(error.reason())),
                                              error.what()),
                        {},
                        payload_digest};
            } catch (const std::out_of_range& error) {
                renderer->record_error(PvrRenderError::MemoryRange, request, error.what());
                return {failed_pvr_render_job(
                            PvrRenderError::MemoryRange, "none", error.what()),
                        {},
                        payload_digest};
            } catch (const std::invalid_argument& error) {
                renderer->record_error(PvrRenderError::InvalidConfiguration, request, error.what());
                return {failed_pvr_render_job(
                            PvrRenderError::InvalidConfiguration, "none", error.what()),
                        {},
                        payload_digest};
            } catch (const std::logic_error& error) {
                renderer->record_error(PvrRenderError::InvalidTaState, request, error.what());
                return {failed_pvr_render_job(
                            PvrRenderError::InvalidTaState, "none", error.what()),
                        {},
                        payload_digest};
            } catch (const std::runtime_error& error) {
                renderer->record_error(PvrRenderError::UnsupportedFeature, request, error.what());
                return {failed_pvr_render_job(
                            PvrRenderError::UnsupportedFeature, "none", error.what()),
                        {},
                        payload_digest};
            } catch (...) {
                constexpr std::string_view detail =
                    "Unbekannter Fehler beim Einfrieren des PVR-Auftrags.";
                renderer->record_error(PvrRenderError::InternalLifecycle,
                                       request,
                                       std::string(detail));
                return {failed_pvr_render_job(
                            PvrRenderError::InternalLifecycle, "none", std::string(detail)),
                        {},
                        payload_digest};
            }
            return {
                [frame = std::move(frame),
                 registers = register_snapshot,
                 pvr_renderer,
                 vram,
                 raise_now,
                 request,
                 generation]() mutable {
                    const auto renderer = pvr_renderer.lock();
                    const auto target = vram.lock();
                    if (!renderer || !target) {
                        constexpr std::string_view detail =
                            "Eingefrorener PVR-Auftrag verlor sein Ziel.";
                        if (renderer)
                            renderer->record_error(PvrRenderError::InternalLifecycle,
                                                   request,
                                                   std::string(detail));
                        throw PvrRenderJobError(PvrRenderError::InternalLifecycle,
                                                "none",
                                                std::string(detail));
                    }
                    try {
                        renderer->render(frame, registers, *target, generation);
                    } catch (const std::out_of_range& error) {
                        renderer->record_error(PvrRenderError::MemoryRange, request, error.what());
                        throw PvrRenderJobError(
                            PvrRenderError::MemoryRange, "none", error.what());
                    } catch (const std::invalid_argument& error) {
                        renderer->record_error(
                            PvrRenderError::InvalidConfiguration, request, error.what());
                        throw PvrRenderJobError(
                            PvrRenderError::InvalidConfiguration, "none", error.what());
                    } catch (const std::logic_error& error) {
                        renderer->record_error(
                            PvrRenderError::InvalidTaState, request, error.what());
                        throw PvrRenderJobError(
                            PvrRenderError::InvalidTaState, "none", error.what());
                    } catch (const std::runtime_error& error) {
                        renderer->record_error(
                            PvrRenderError::UnsupportedFeature, request, error.what());
                        throw PvrRenderJobError(
                            PvrRenderError::UnsupportedFeature, "none", error.what());
                    } catch (...) {
                        constexpr std::string_view detail =
                            "Unbekannter Fehler an der PVR-Produktgrenze.";
                        renderer->record_error(PvrRenderError::InternalLifecycle,
                                               request,
                                               std::string(detail));
                        throw PvrRenderJobError(PvrRenderError::InternalLifecycle,
                                                "none",
                                                std::string(detail));
                    }
                    raise_now(SystemAsicEvent::PvrRenderDone);
                    return PvrRenderResult::Success;
                },
                [fifo, staged_fifo = std::move(staged_fifo)]() mutable noexcept {
                    *fifo = std::move(staged_fifo);
                },
                payload_digest,
            };
        });
    state.pvr_registers->set_render_overrun_observer(
        [raise_now] { raise_now(SystemAsicEvent::PvrOverrun); });
    state.aica = std::make_shared<AicaExecutionController>(state.scheduler.get());
    state.aica->interrupts().set_observer(
        [raise_now] { raise_now(SystemAsicEvent::AicaInterrupt); });
    state.aica_registers = map_aica_registers(cpu.memory, state.aica, state.aica_ram);
    state.aica_rtc = map_aica_rtc(cpu.memory, state.scheduler.get());
    state.maple = std::make_shared<MapleBus>([raise_now] { raise_now(SystemAsicEvent::MapleDma); });
    state.maple_controller =
        map_dreamcast_maple_controller(cpu.memory, *state.scheduler, state.maple, {}, [raise_now] {
            raise_now(SystemAsicEvent::MapleDma);
        });
    const auto maple_controller = std::weak_ptr<DreamcastMapleController>(state.maple_controller);
    state.pvr_registers->set_vblank_observer(
        [raise_now,
         maple_controller,
         pvr_registers,
         pvr_renderer,
         vram](const bool entering) {
            raise_now(entering ? SystemAsicEvent::PvrVblank : SystemAsicEvent::PvrVblankOut);
            if (!entering) return;
            const auto registers = pvr_registers.lock();
            const auto renderer = pvr_renderer.lock();
            const auto target = vram.lock();
            if (registers && renderer && target) {
                try {
                    renderer->observe_vblank_scanout(*registers, target->bytes());
                } catch (const std::out_of_range& error) {
                    renderer->record_error(PvrRenderError::MemoryRange,
                                           registers->render_request_count(),
                                           error.what());
                } catch (const std::invalid_argument& error) {
                    renderer->record_error(PvrRenderError::InvalidConfiguration,
                                           registers->render_request_count(),
                                           error.what());
                } catch (const std::runtime_error& error) {
                    renderer->record_error(PvrRenderError::UnsupportedFeature,
                                           registers->render_request_count(),
                                           error.what());
                } catch (...) {
                    renderer->record_error(PvrRenderError::InternalLifecycle,
                                           registers->render_request_count(),
                                           "Unbekannter Fehler am PVR-VBlank-Scanout.");
                }
            }
            if (const auto maple = maple_controller.lock()) maple->hardware_trigger();
        });
    state.pvr_registers->set_hblank_observer(
        [raise_now] { raise_now(SystemAsicEvent::PvrHblank); });
    if (state.mutable_storage) {
        state.vmu = std::make_shared<MapleVmuDevice>(state.mutable_storage->vmu_image());
        state.maple->attach(0u, 1u, state.vmu);
    }
    state.runtime_blocks = std::make_shared<RuntimeBlockTable>();
    state.code_tracker = std::make_shared<ExecutableCodeTracker>();
    state.runtime_blocks->bind_code_tracker(state.code_tracker.get());
    state.module_catalog = std::make_shared<ExecutableModuleCatalog>();
    state.disc_load_transactions =
        std::make_shared<ExecutableDiscLoadTransactionCoordinator>(
            cpu.memory,
            *state.module_catalog,
            *state.runtime_blocks,
            *state.code_tracker,
            [](const std::uint32_t physical, const std::size_t size) {
                std::vector<DiscLoadCommittedRange> ranges;
                for_each_dreamcast_executable_backing_extent(
                    physical,
                    size,
                    [&](const std::uint32_t backing,
                        const std::size_t source_offset,
                        const std::size_t extent_size,
                        const bool main_ram) {
                        ranges.push_back(
                            {canonical_physical_address(
                                 physical + static_cast<std::uint32_t>(source_offset)),
                             backing,
                             static_cast<std::uint32_t>(source_offset),
                             static_cast<std::uint32_t>(extent_size),
                             {},
                             main_ram,
                             false,
                             false});
                    });
                return ranges;
            });
    const auto disc_load_transactions =
        std::weak_ptr<ExecutableDiscLoadTransactionCoordinator>(
            state.disc_load_transactions);
    state.gdrom = map_dreamcast_gdrom(
        cpu.memory,
        *state.scheduler,
        GdRomDrive(boot.source),
        [asic, scheduler](const std::uint64_t cycle) {
            const auto target = asic.lock();
            const auto clock = scheduler.lock();
            if (!target || !clock)
                throw std::runtime_error("Dreamcast-System-ASIC-Lebenszyklus fehlt.");
            if (cycle != clock->current_cycle())
                throw std::logic_error("GD-ROM-Completion liegt nicht am aktuellen Gastzyklus.");
            target->raise(SystemAsicEvent::GdromCommand, cycle);
        },
        {},
        [asic] {
            const auto target = asic.lock();
            if (!target) throw std::runtime_error("Dreamcast-System-ASIC-Lebenszyklus fehlt.");
            target->write(0x04u, 1u);
        },
        [disc_load_transactions](const DiscLoadRequest& request) {
            const auto transactions = disc_load_transactions.lock();
            if (!transactions)
                throw std::runtime_error("Disc-Ladetransaktions-Lebenszyklus fehlt.");
            return transactions->execute(request);
        },
        boot.content_identity);
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
    state.gdrom->bind_g1_bus(state.holly_dma.g1.get());
    state.holly_dma.g1->set_fault_observer([gdrom](const G1DmaFault& fault) {
        if (const auto controller = gdrom.lock()) controller->handle_g1_dma_fault(fault);
    });
    const auto system_reset_asic =
        std::weak_ptr<DreamcastSystemAsic>(state.system_asic);
    const auto system_reset_pvr =
        std::weak_ptr<PvrRegisterFile>(state.pvr_registers);
    const auto system_reset_yuv =
        std::weak_ptr<PvrYuvConverterMemoryDevice>(state.pvr_yuv_converter);
    const auto system_reset_g1 =
        std::weak_ptr<DreamcastG1BusController>(state.holly_dma.g1);
    const auto system_reset_g2 =
        std::weak_ptr<DreamcastG2DmaController>(state.holly_dma.g2);
    const auto system_reset_pvr_dma =
        std::weak_ptr<DreamcastPvrDmaController>(state.holly_dma.pvr);
    state.system_bus_control->set_system_reset_observer(
        [system_reset_asic,
         system_reset_pvr,
         system_reset_yuv,
         system_reset_g1,
         system_reset_g2,
         system_reset_pvr_dma] {
            const auto asic = system_reset_asic.lock();
            const auto pvr = system_reset_pvr.lock();
            const auto yuv = system_reset_yuv.lock();
            const auto g1 = system_reset_g1.lock();
            const auto g2 = system_reset_g2.lock();
            const auto pvr_dma = system_reset_pvr_dma.lock();
            if (!asic || !pvr || !yuv || !g1 || !g2 || !pvr_dma)
                throw std::runtime_error("Holly-Systemreset-Lebenszyklus fehlt.");

            // SB_SFRES resets Holly/PVR device state without synthesizing an SH-4
            // power-on reset or clearing guest RAM/VRAM.
            asic->reset();
            pvr->reset();
            yuv->reset();
            g1->reset();
            g2->reset();
            pvr_dma->reset();
        });
    cpu.g1_bus = state.holly_dma.g1.get();
    const auto boot_sectors = static_cast<std::uint32_t>(
        (boot.boot_file.size() + dreamcast_data_sector_size - 1u) /
        dreamcast_data_sector_size);
    state.holly_dma.g1->configure_bios_handoff(
        dreamcast_disc_boot_address + boot_sectors * dreamcast_data_sector_size);
    state.holly_dma.pvr->bind_sh4_dmac(state.dmac, 0u);
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
    const auto runtime_modules = std::weak_ptr<ExecutableModuleCatalog>(state.module_catalog);
    const auto direct_scanout = std::weak_ptr<PvrSoftwareRenderer>(state.pvr_renderer);
    cpu.memory.set_guest_write_observer(
        [code_tracker,
         runtime_blocks,
         runtime_modules,
         disc_load_transactions,
         direct_scanout](
            const GuestWriteEvent& event) noexcept {
            if (const auto renderer = direct_scanout.lock())
                renderer->observe_vram_write(event.address, event.size, event.bytes_changed);
            if (const auto transactions = disc_load_transactions.lock();
                transactions && transactions->consume_guest_write(event))
                return;
            const auto tracker = code_tracker.lock();
            for_each_dreamcast_executable_backing_extent(
                event.address,
                event.size,
                [&](const std::uint32_t backing,
                    const std::size_t,
                    const std::size_t extent_size,
                    const bool main_ram) {
                    if (main_ram) {
                        if (const auto modules = runtime_modules.lock()) {
                            modules->record_runtime_write(
                                backing, extent_size, event.source, event.bytes_changed);
                        }
                    }
                    if (!tracker ||
                        (!main_ram && !tracker->tracks_address(backing, extent_size)))
                        return;
                    const auto invalidation = tracker->observe_write(
                        backing, extent_size, event.source, event.bytes_changed);
                    if (!invalidation.byte_identical) {
                        if (const auto blocks = runtime_blocks.lock())
                            static_cast<void>(
                                blocks->erase_overlapping_physical(backing, extent_size));
                    }
                });
        },
        GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
    state.store_queue_transfers = std::make_shared<std::vector<StoreQueueTransfer>>();
    state.store_queue_transfers->reserve(1024u);
    state.dropped_store_queue_transfers = std::make_shared<std::uint64_t>(0u);
    const auto transfers = state.store_queue_transfers;
    const auto dropped_transfers = state.dropped_store_queue_transfers;
    const auto store_queue_ta = state.pvr_ta_fifo;
    const auto store_queue_pvr = state.pvr_registers;
    auto* const memory = &cpu.memory;
    auto* const store_queue_cpu = &cpu;
    state.store_queues = std::make_shared<Sh4StoreQueues>(
        cpu.memory,
        [transfers,
         dropped_transfers,
         memory,
         store_queue_cpu,
         store_queue_ta,
         store_queue_pvr](
            const StoreQueueTransfer& transfer) {
            if (transfers->size() == 1024u) {
                transfers->erase(transfers->begin());
                if (*dropped_transfers != std::numeric_limits<std::uint64_t>::max()) {
                    ++*dropped_transfers;
                }
            }
            transfers->push_back(transfer);
            if (transfer.target == StoreQueueTarget::TileAccelerator) {
                const auto result = store_queue_ta->submit_guest(transfer.bytes);
                if (!result.accepted) {
                    const auto reason =
                        result.error &&
                                result.error->reason == PvrTaInputErrorReason::UnsupportedPacket
                            ? StoreQueueSinkErrorReason::UnsupportedInput
                            : StoreQueueSinkErrorReason::DeviceRejected;
                    throw StoreQueueSinkError(
                        reason,
                        result.error ? result.error->detail
                                     : "TA-Eingabe wurde ohne Detail abgelehnt.",
                        pvr_ta_packet_class(transfer.bytes));
                }
                store_queue_pvr->record_ta_packet(
                    static_cast<std::uint32_t>(transfer.bytes.size()));
            } else {
                memory->write_bytes_at(
                    transfer.target_address,
                    transfer.bytes,
                    GuestMemoryAccessContext{transfer.target_address,
                                             transfer.instruction,
                                             transfer.retired_guest_instructions,
                                             GuestMemoryAccessOrigin::Memory,
                                             transfer.attempted_guest_instructions},
                    CodeWriteSource::StoreQueue);
            }
        },
        state.code_tracker.get());
    state.store_queues->bind_runtime_code_invalidation(
        state.code_tracker, state.runtime_blocks);
    state.store_queues->set_prefetch_address_translator(
        [store_queue_cpu](const std::uint32_t address) {
            return translate_store_queue_prefetch(*store_queue_cpu, address);
        });
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
    if (boot_config.firmware_mode == DreamcastRuntimeFirmwareMode::HleBiosAbi) {
        install_hle_bios_abi(cpu.memory,
                             *state.runtime_blocks,
                             *state.firmware_handoff,
                             {},
                             0u,
                             state.code_tracker.get());
    }
    // DirectBootExecutable does not execute this bootstrap, but retaining the
    // validated immutable module keeps disc identity, AOT provenance and the
    // compatibility overload independent of the selected first guest entry.
    cpu.memory.write_bytes(
        dreamcast_system_bootstrap_address, boot.system_bootstrap, CodeWriteSource::Copy);
    state.loaded_system_bootstrap_bytes = boot.system_bootstrap.size();
    cpu.memory.write_bytes(dreamcast_disc_boot_address, boot.boot_file, CodeWriteSource::Copy);
    state.loaded_boot_bytes = boot.boot_file.size();
    const auto publish_initial_executable = [&](const std::string& id,
                                                const std::uint32_t guest_start,
                                                const std::span<const std::uint8_t> bytes) {
        ExecutableModule module;
        module.id = id;
        module.content_identity = boot.content_identity;
        module.byte_identity = disc_load_byte_identity(bytes);
        // Static export templates already bind this historical raw-byte source identity.
        module.source_identity = module.byte_identity;
        module.guest_start = guest_start;
        module.bytes.assign(bytes.begin(), bytes.end());
        module.kind = ExecutableModuleKind::Module;
        module.executable_permission = true;
        module.control_transfer_promotion_allowed = false;
        module.writable = true;
        state.module_catalog->publish(std::move(module));
    };
    publish_initial_executable(std::string(dreamcast_initial_disc_bootstrap_module_id),
                               dreamcast_system_bootstrap_address,
                               boot.system_bootstrap);
    publish_initial_executable(std::string(dreamcast_initial_boot_executable_module_id),
                               dreamcast_disc_boot_address,
                               boot.boot_file);
    reset_dreamcast_direct_boot_cpu_state(
        cpu, boot_config.post_bios_cpu_state);
    if (boot_config.boot_path == DreamcastRuntimeBootPath::NativeDiscBoot)
        cpu.pc = dreamcast_system_bootstrap_entry_address;
    state.dmac->write_operation(dreamcast_bios_handoff_dmaor);
    state.aica_registers->write(0x289Cu, 0x48u, MemoryAccessWidth::Halfword);
    state.aica_registers->write(0x28A8u, 0x18u, MemoryAccessWidth::Byte);
    state.aica_registers->write(0x28ACu, 0x50u, MemoryAccessWidth::Byte);
    state.aica_registers->write(0x28B0u, 0x08u, MemoryAccessWidth::Byte);
    state.io_ports->write_control_a(dreamcast_bios_handoff_pctra);
    if (boot_region == DreamcastRegion::Europe)
        state.io_ports->write_data_a(dreamcast_bios_handoff_pal_pdtra);
    state.manual_reset = std::make_shared<DreamcastManualResetCoordinator>(
        [scheduler = state.scheduler,
         rtc_clock = state.rtc_clock,
         tmu = state.tmu,
         rtc = state.rtc,
         dmac = state.dmac,
         mmu_control = state.mmu_control,
         interrupt_controller = state.interrupt_controller,
         interrupt_router = state.interrupt_router,
         interrupt_registers = state.interrupt_registers,
         scif = state.scif,
         system_bus_control = state.system_bus_control,
         system_asic = state.system_asic,
         pvr_registers = state.pvr_registers,
         pvr_ta_fifo = state.pvr_ta_fifo,
         pvr_renderer = state.pvr_renderer,
         vram = state.vram,
         aica_registers = state.aica_registers,
         aica_rtc = state.aica_rtc,
         maple_controller = state.maple_controller,
         holly_dma = state.holly_dma,
         gdrom = state.gdrom,
         cache_control = state.cache_control,
         store_queues = state.store_queues,
         io_ports = state.io_ports](CpuState& reset_cpu_state) {
            const auto best_effort = [](auto&& operation) noexcept {
                try {
                    operation();
                } catch (...) {
                }
            };
            best_effort([&] { scheduler->reset(); });
            reset_cpu(reset_cpu_state,
                      ResetState{tlb_multiple_hit_reset_vector,
                                 0u,
                                 0u,
                                 sr_md_mask | sr_rb_mask | sr_bl_mask | sr_interrupt_mask,
                                 0u});
            mmu_control->reset();
            cache_control->reset();
            store_queues->reset();
            interrupt_controller->clear();
            interrupt_router->reset();
            interrupt_registers->reset();
            dmac->reset();
            tmu->reset();
            best_effort([&] { rtc_clock->reset_phase(0u); });
            best_effort([&] { rtc->reset_divider(); });
            scif->reset();
            system_bus_control->reset();
            system_asic->reset();
            best_effort([&] { pvr_registers->reset(); });
            pvr_ta_fifo->reset();
            best_effort(
                [&] { pvr_renderer->reset_guest_frame_evidence(vram->bytes()); });
            aica_registers->reset();
            aica_rtc->reset();
            maple_controller->reset();
            if (holly_dma.g1) holly_dma.g1->reset();
            if (holly_dma.g2) holly_dma.g2->reset();
            if (holly_dma.pvr) holly_dma.pvr->reset();
            gdrom->reset();
            io_ports->reset();
        });
    cpu.manual_reset_sink = state.manual_reset->sink();
    return state;
}

void reset_dreamcast_direct_boot_cpu(CpuState& cpu) noexcept {
    reset_dreamcast_direct_boot_cpu_state(
        cpu, dreamcast_post_bios_cpu_state);
}

DreamcastRuntimeState
initialize_dreamcast_runtime(CpuState& cpu,
                             const DreamcastRuntimeBootImage& boot,
                             const DreamcastRuntimeFirmwareMode firmware_mode,
                             std::shared_ptr<DreamcastMutableStorage> mutable_storage,
                             const DreamcastConsoleProfile console_profile) {
    const auto boot_path =
        firmware_mode == DreamcastRuntimeFirmwareMode::HleBiosAbi
            ? DreamcastRuntimeBootPath::NativeDiscBoot
            : DreamcastRuntimeBootPath::DirectBootExecutable;
    DreamcastRuntimeBootConfig boot_config;
    boot_config.firmware_mode = firmware_mode;
    boot_config.boot_path = boot_path;
    return initialize_dreamcast_runtime(cpu,
                                        boot,
                                        boot_config,
                                        std::move(mutable_storage),
                                        console_profile);
}

} // namespace katana::runtime
