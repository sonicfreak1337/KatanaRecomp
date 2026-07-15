#pragma once

#include "katana/runtime/disc.hpp"

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

class GdiDiscSource final : public DiscSource {
public:
    using DiscSource::read;
    [[nodiscard]] static std::shared_ptr<GdiDiscSource> open(
        const std::filesystem::path& descriptor_path
    );
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] const std::string& identity() const noexcept override;
    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override;
    [[nodiscard]] const GdiDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::uint32_t primary_data_lba() const;
    [[nodiscard]] std::vector<std::uint8_t> read_raw_sector(
        std::uint32_t track_number,
        std::uint64_t sector_index
    ) const;
private:
    explicit GdiDiscSource(GdiDescriptor descriptor);
    [[nodiscard]] std::vector<std::uint8_t> read_data_sector(std::uint64_t absolute_lba) const;
    GdiDescriptor descriptor_;
    std::vector<std::shared_ptr<FileDiscSource>> track_sources_;
    std::string identity_;
    std::uint64_t logical_size_ = 0u;
};

} // namespace katana::runtime
