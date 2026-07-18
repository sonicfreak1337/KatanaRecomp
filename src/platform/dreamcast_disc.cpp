#include "katana/platform/dreamcast_disc.hpp"

#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <stdexcept>
#include <string_view>

namespace katana::platform {
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

} // namespace

DreamcastBootMetadata parse_dreamcast_boot_metadata(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 0x70u) {
        throw std::invalid_argument("Dreamcast-Bootmetadaten sind abgeschnitten.");
    }
    DreamcastBootMetadata result{trimmed_ascii(bytes, 0x00u, 16u),
                                 trimmed_ascii(bytes, 0x60u, 16u)};
    if (result.hardware_id != "SEGA SEGAKATANA") {
        throw std::invalid_argument("Dreamcast-Hardwarekennung ist ungueltig.");
    }
    if (!safe_iso_file_name(result.boot_file_name)) {
        throw std::invalid_argument("Dreamcast-Bootdateiname ist ungueltig.");
    }
    return result;
}

DreamcastDiscBoot load_dreamcast_gdi_boot(const std::filesystem::path& descriptor_path) {
    auto boot = runtime::load_dreamcast_runtime_boot(descriptor_path);
    return {std::move(boot.source),
            {std::move(boot.hardware_id), std::move(boot.boot_file_name)},
            std::move(boot.boot_file),
            boot.data_track_lba,
            boot.extent_lba_bias,
            boot.validated_tracks,
            boot.repeated_reads_match};
}

io::ExecutableImage make_dreamcast_disc_executable(const DreamcastDiscBoot& disc) {
    if (disc.boot_file.empty()) {
        throw std::invalid_argument("Dreamcast-Bootdatei ist leer.");
    }
    io::ExecutableImage image;
    image.set_guest_call_abi(io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    io::ImageSegment boot_segment{".text",
                                  dreamcast_disc_boot_address,
                                  0u,
                                  disc.boot_file.size(),
                                  io::SegmentKind::Mixed,
                                  {true, true, true},
                                  disc.boot_file};
    boot_segment.source_kind = io::ImageSourceKind::DiscBootFile;
    boot_segment.load_phase = io::ImageLoadPhase::Initial;
    boot_segment.local_source_name = disc.metadata.boot_file_name;
    image.add_segment(std::move(boot_segment));
    image.add_entry_point(dreamcast_disc_boot_address);
    return image;
}

} // namespace katana::platform
