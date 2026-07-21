#pragma once

#include "katana/runtime/disc.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

enum class GdiTrackType : std::uint8_t { Audio = 0u, Data = 4u };

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
    std::string sha256;
};

struct GdiDescriptor {
    std::string descriptor_name;
    std::filesystem::path resolved_path;
    std::uint64_t size = 0u;
    std::string sha256;
    std::vector<GdiTrack> tracks;
};

enum class DiscCacheMode : std::uint8_t { Enabled, DisabledReference };

struct GdiIoCounters {
    std::uint64_t persistent_track_opens = 0u;
    std::uint64_t track_lookups = 0u;
    std::uint64_t raw_read_operations = 0u;
    std::uint64_t decoded_sectors = 0u;
    std::uint64_t sector_cache_hits = 0u;
    std::uint64_t sector_cache_misses = 0u;
    std::uint64_t sector_cache_evictions = 0u;
};

[[nodiscard]] GdiDescriptor parse_gdi_descriptor(const std::filesystem::path& descriptor_path);
[[nodiscard]] std::string gdi_content_identity(const GdiDescriptor& descriptor);

class GdiDiscSource final : public DiscSource {
  public:
    using DiscSource::read;
    [[nodiscard]] static std::shared_ptr<GdiDiscSource>
    open(const std::filesystem::path& descriptor_path);
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] const std::string& identity() const noexcept override;
    [[nodiscard]] std::vector<DiscTrackLayout> layout() const override;
    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override;
    [[nodiscard]] const GdiDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::uint32_t primary_data_lba() const;
    [[nodiscard]] std::vector<std::uint8_t> read_raw_sector(std::uint32_t track_number,
                                                            std::uint64_t sector_index) const;
    [[nodiscard]] std::vector<std::uint8_t> read_raw_sectors(std::uint32_t track_number,
                                                             std::uint64_t first_sector,
                                                             std::size_t count) const;
    void set_cache_mode(DiscCacheMode mode) noexcept;
    [[nodiscard]] DiscCacheMode cache_mode() const noexcept;
    [[nodiscard]] const GdiIoCounters& io_counters() const noexcept;
    void reset_io_counters() const noexcept;
    [[nodiscard]] std::size_t sector_cache_size() const noexcept;
    [[nodiscard]] std::size_t sector_cache_capacity() const noexcept;

  private:
    explicit GdiDiscSource(GdiDescriptor descriptor);
    [[nodiscard]] std::vector<std::uint8_t> read_data_sector(std::uint64_t absolute_lba) const;
    [[nodiscard]] std::vector<std::uint8_t> read_data_sectors(std::uint64_t absolute_lba,
                                                              std::size_t count) const;
    [[nodiscard]] std::size_t track_index_for_lba(std::uint64_t absolute_lba) const;
    [[nodiscard]] std::vector<std::uint8_t> decode_track_sectors(std::size_t track_index,
                                                                 std::uint64_t first_sector,
                                                                 std::size_t count) const;
    GdiDescriptor descriptor_;
    std::vector<std::shared_ptr<FileDiscSource>> track_sources_;
    std::unordered_map<std::uint32_t, std::size_t> track_number_index_;
    std::string identity_;
    std::uint64_t logical_size_ = 0u;
    std::size_t sector_cache_capacity_ = 256u;
    mutable std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> sector_cache_;
    mutable std::vector<std::uint64_t> sector_cache_order_;
    mutable std::mutex cache_mutex_;
    DiscCacheMode cache_mode_ = DiscCacheMode::Enabled;
    mutable GdiIoCounters io_counters_;
};

} // namespace katana::runtime
