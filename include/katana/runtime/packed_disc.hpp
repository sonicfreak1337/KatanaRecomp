#pragma once

#include "katana/runtime/disc.hpp"
#include "katana/runtime/gdi.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t packed_disc_format_version = 2u;
inline constexpr std::uint32_t packed_disc_chunk_sectors = 64u;

enum class PackedDiscPayloadKind : std::uint32_t {
    NotData = 0u,
    Fixed = 1u,
    SectorMode = 2u,
};

struct PackedDiscTrack {
    std::uint32_t number = 0u;
    std::uint32_t lba = 0u;
    GdiTrackType type = GdiTrackType::Data;
    std::uint32_t sector_size = 0u;
    PackedDiscPayloadKind payload_kind = PackedDiscPayloadKind::NotData;
    std::uint32_t payload_offset = 0u;
    std::uint32_t session = 0u;
    std::uint64_t sector_count = 0u;
    std::uint64_t first_chunk = 0u;
    std::uint64_t chunk_count = 0u;
    std::string integrity_sha256;
};

struct PackedDiscInfo {
    std::uint32_t format_version = packed_disc_format_version;
    std::string content_identity;
    std::string job_generation;
    std::uint64_t logical_size = 0u;
    std::uint64_t packed_sectors = 0u;
    std::uint64_t uncompressed_size = 0u;
    std::uint64_t pack_size = 0u;
    std::uint64_t chunk_count = 0u;
    std::vector<PackedDiscTrack> tracks;
};

class PackedDiscSource final : public DiscSource {
  public:
    using DiscSource::read;
    ~PackedDiscSource() override;
    [[nodiscard]] static std::shared_ptr<PackedDiscSource> open(const std::filesystem::path& path);
    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] const std::string& identity() const noexcept override;
    [[nodiscard]] std::vector<DiscTrackLayout> layout() const override;
    void read(std::uint64_t offset, std::span<std::uint8_t> destination) const override;
    [[nodiscard]] const PackedDiscInfo& info() const noexcept;
    [[nodiscard]] std::uint32_t primary_data_lba() const;
    [[nodiscard]] std::vector<std::uint8_t> read_raw_sector(std::uint32_t track_number,
                                                            std::uint64_t sector_index) const;
    [[nodiscard]] std::vector<std::uint8_t> read_raw_sectors(std::uint32_t track_number,
                                                             std::uint64_t first_sector,
                                                             std::size_t count) const;
    void verify_all_chunks() const;
    [[nodiscard]] std::size_t chunk_cache_size() const noexcept;
    [[nodiscard]] std::size_t chunk_cache_capacity() const noexcept;

  private:
    struct Chunk;
    PackedDiscSource(std::filesystem::path path,
                     PackedDiscInfo info,
                     std::vector<Chunk> chunks,
                     std::uint64_t file_size);
    [[nodiscard]] std::size_t track_index_for_lba(std::uint64_t absolute_lba) const;
    [[nodiscard]] std::vector<std::uint8_t> load_chunk(std::size_t chunk_index) const;
    [[nodiscard]] std::vector<std::uint8_t> read_data_sectors(std::uint64_t absolute_lba,
                                                              std::size_t count) const;
    std::filesystem::path path_;
    PackedDiscInfo info_;
    std::vector<Chunk> chunks_;
    std::unordered_map<std::uint32_t, std::size_t> track_number_index_;
    std::string identity_;
    std::uint64_t file_size_ = 0u;
    mutable std::ifstream stream_;
    mutable std::mutex stream_mutex_;
    std::size_t chunk_cache_capacity_ = 8u;
    mutable std::unordered_map<std::size_t, std::vector<std::uint8_t>> chunk_cache_;
    mutable std::vector<std::size_t> chunk_cache_order_;
    mutable std::mutex cache_mutex_;
};

[[nodiscard]] PackedDiscInfo write_packed_disc(const GdiDiscSource& source,
                                               const std::filesystem::path& destination,
                                               std::string job_generation,
                                               std::string_view expected_content_identity = {});
[[nodiscard]] std::string packed_disc_content_identity(const GdiDiscSource& source);
[[nodiscard]] std::string
format_packed_disc_manifest(const PackedDiscInfo& info,
                            std::string_view pack_sha256,
                            std::string_view executable_sha256 = {},
                            std::string_view executable_path = "../game.exe",
                            std::uint64_t executable_size = 0u);

} // namespace katana::runtime
