#pragma once

#include "katana/runtime/disc.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

struct Iso9660Entry {
    std::string name;
    std::uint32_t lba = 0u;
    std::uint32_t size = 0u;
    bool directory = false;
};

enum class Iso9660CacheMode : std::uint8_t { Enabled, DisabledReference };

struct Iso9660IoCounters {
    std::uint64_t directory_cache_hits = 0u;
    std::uint64_t directory_cache_misses = 0u;
    std::uint64_t extent_cache_hits = 0u;
    std::uint64_t extent_cache_misses = 0u;
    std::uint64_t cache_evictions = 0u;
};

class Iso9660Filesystem final {
  public:
    explicit Iso9660Filesystem(std::shared_ptr<const DiscSource> source,
                               std::uint32_t sector_size = 2048u,
                               std::uint32_t volume_start_lba = 0u,
                               std::optional<std::uint32_t> extent_lba_bias = std::nullopt);
    [[nodiscard]] std::vector<Iso9660Entry> list_directory(std::string_view path = "/") const;
    [[nodiscard]] std::vector<std::uint8_t> read_file(std::string_view path) const;
    void set_cache_mode(Iso9660CacheMode mode) noexcept;
    [[nodiscard]] Iso9660CacheMode cache_mode() const noexcept;
    [[nodiscard]] const Iso9660IoCounters& io_counters() const noexcept;
    void reset_io_counters() const noexcept;
    [[nodiscard]] std::size_t directory_cache_size() const noexcept;
    [[nodiscard]] std::size_t extent_cache_size() const noexcept;

  private:
    [[nodiscard]] Iso9660Entry resolve(std::string_view path) const;
    [[nodiscard]] std::vector<Iso9660Entry> read_directory(const Iso9660Entry& directory) const;
    [[nodiscard]] static std::vector<std::string> split_path(std::string_view path);
    std::shared_ptr<const DiscSource> source_;
    std::uint32_t sector_size_ = 2048u;
    std::uint32_t volume_start_lba_ = 0u;
    std::uint32_t extent_lba_bias_ = 0u;
    Iso9660Entry root_;
    Iso9660CacheMode cache_mode_ = Iso9660CacheMode::Enabled;
    mutable std::unordered_map<std::uint64_t, std::vector<Iso9660Entry>> directory_cache_;
    mutable std::unordered_map<std::string, Iso9660Entry> extent_cache_;
    mutable std::vector<std::uint64_t> directory_cache_order_;
    mutable std::vector<std::string> extent_cache_order_{"/"};
    std::size_t directory_cache_capacity_ = 256u;
    std::size_t extent_cache_capacity_ = 4096u;
    mutable std::mutex cache_mutex_;
    mutable Iso9660IoCounters io_counters_;
};

} // namespace katana::runtime
