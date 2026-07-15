#include "katana/platform/dreamcast_disc.hpp"

#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/iso9660.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>

namespace katana::platform {
namespace {

std::string trimmed_ascii(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t length
) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    std::string result(
        reinterpret_cast<const char*>(bytes.data() + offset),
        length
    );
    while (!result.empty() && (result.back() == ' ' || result.back() == '\0')) {
        result.pop_back();
    }
    return result;
}

bool safe_iso_file_name(const std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
        value.find('/') == std::string_view::npos &&
        value.find('\\') == std::string_view::npos &&
        value.find(':') == std::string_view::npos;
}

std::vector<std::uint32_t> extent_bias_candidates(const std::uint32_t data_track_lba) {
    return data_track_lba == 0u
        ? std::vector<std::uint32_t>{0u}
        : std::vector<std::uint32_t>{0u, data_track_lba};
}

} // namespace

DreamcastBootMetadata parse_dreamcast_boot_metadata(
    const std::span<const std::uint8_t> bytes
) {
    if (bytes.size() < 0x70u) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    DreamcastBootMetadata result{
        trimmed_ascii(bytes, 0x00u, 16u),
        trimmed_ascii(bytes, 0x60u, 16u)
    };
    if (result.hardware_id != "SEGA SEGAKATANA") {
        throw std::invalid_argument("Dreamcast-Hardwarekennung ist ungueltig.");
    }
    if (!safe_iso_file_name(result.boot_file_name)) {
        throw std::invalid_argument("Dreamcast-Bootdateiname ist ungueltig.");
    }
    return result;
}

DreamcastDiscBoot load_dreamcast_gdi_boot(
    const std::filesystem::path& descriptor_path
) {
    auto source = runtime::GdiDiscSource::open(descriptor_path);
    const auto data_track_lba = source->primary_data_lba();
    if (data_track_lba > std::numeric_limits<std::uint64_t>::max() / 2048u) {
        throw std::out_of_range("Dreamcast-Bootsektoroffset laeuft ueber.");
    }
    const auto boot_sector = source->read(
        static_cast<std::uint64_t>(data_track_lba) * 2048u,
        256u
    );
    const auto metadata = parse_dreamcast_boot_metadata(boot_sector);

    std::vector<std::uint8_t> boot_file;
    std::uint32_t selected_bias = 0u;
    std::string last_error = "ISO9660-Dateisystem konnte nicht geoeffnet werden.";
    for (const auto bias : extent_bias_candidates(data_track_lba)) {
        try {
            runtime::Iso9660Filesystem filesystem(source, 2048u, data_track_lba, bias);
            boot_file = filesystem.read_file("/" + metadata.boot_file_name);
            selected_bias = bias;
            break;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    if (boot_file.empty()) {
        throw std::runtime_error("Dreamcast-Bootdatei ist leer oder nicht lesbar: " + last_error);
    }
    if (boot_file.size() > runtime::dreamcast_main_ram_size) {
        throw std::out_of_range("Dreamcast-Bootdatei passt nicht in den Hauptspeicher.");
    }

    runtime::Iso9660Filesystem repeated_filesystem(
        source,
        2048u,
        data_track_lba,
        selected_bias
    );
    const auto repeated = repeated_filesystem.read_file("/" + metadata.boot_file_name);
    const bool repeated_reads_match = repeated == boot_file;
    const auto track_count = source->descriptor().tracks.size();

    return {
        std::move(source),
        metadata,
        std::move(boot_file),
        data_track_lba,
        selected_bias,
        track_count,
        repeated_reads_match
    };
}

io::ExecutableImage make_dreamcast_disc_executable(const DreamcastDiscBoot& disc) {
    if (disc.boot_file.empty()) {
        throw std::invalid_argument("Dreamcast-Bootdatei ist leer.");
    }
    io::ExecutableImage image;
    image.add_segment({
        ".text",
        dreamcast_disc_boot_address,
        0u,
        disc.boot_file.size(),
        io::SegmentKind::Code,
        {true, true, true},
        disc.boot_file
    });
    image.add_entry_point(dreamcast_disc_boot_address);
    return image;
}

} // namespace katana::platform
