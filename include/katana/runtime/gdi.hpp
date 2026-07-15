#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace katana::runtime {

enum class GdiTrackType : std::uint8_t {
    Audio = 0u,
    Data = 4u
};

struct GdiTrack {
    std::uint32_t number = 0u;
    std::uint32_t lba = 0u;
    GdiTrackType type = GdiTrackType::Data;
    std::uint32_t sector_size = 0u;
    std::string file_name;
    std::uint64_t file_offset = 0u;
    std::uint64_t sector_count = 0u;
    std::size_t descriptor_line = 0u;
    std::filesystem::path resolved_path;
};

struct GdiDescriptor {
    std::string descriptor_name;
    std::vector<GdiTrack> tracks;
};

[[nodiscard]] GdiDescriptor parse_gdi_descriptor(const std::filesystem::path& descriptor_path);

} // namespace katana::runtime
