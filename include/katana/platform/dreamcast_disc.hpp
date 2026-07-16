#pragma once

#include "katana/io/executable_image.hpp"
#include "katana/runtime/gdi.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::platform {

inline constexpr std::uint32_t dreamcast_disc_boot_address = 0x8C010000u;

struct DreamcastBootMetadata {
    std::string hardware_id;
    std::string boot_file_name;
};

struct DreamcastDiscBoot {
    std::shared_ptr<runtime::GdiDiscSource> source;
    DreamcastBootMetadata metadata;
    std::vector<std::uint8_t> boot_file;
    std::uint32_t data_track_lba = 0u;
    std::uint32_t extent_lba_bias = 0u;
    std::size_t validated_tracks = 0u;
    bool repeated_reads_match = false;
};

[[nodiscard]] DreamcastBootMetadata
parse_dreamcast_boot_metadata(std::span<const std::uint8_t> bytes);

[[nodiscard]] DreamcastDiscBoot
load_dreamcast_gdi_boot(const std::filesystem::path& descriptor_path);

[[nodiscard]] io::ExecutableImage make_dreamcast_disc_executable(const DreamcastDiscBoot& disc);

} // namespace katana::platform
