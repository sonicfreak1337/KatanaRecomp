#pragma once

#include "katana/runtime/disc.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

struct Iso9660Entry {
    std::string name;
    std::uint32_t lba = 0u;
    std::uint32_t size = 0u;
    bool directory = false;
};

class Iso9660Filesystem final {
public:
    explicit Iso9660Filesystem(
        std::shared_ptr<const DiscSource> source,
        std::uint32_t sector_size = 2048u,
        std::uint32_t volume_start_lba = 0u,
        std::optional<std::uint32_t> extent_lba_bias = std::nullopt
    );
    [[nodiscard]] std::vector<Iso9660Entry> list_directory(std::string_view path = "/") const;
    [[nodiscard]] std::vector<std::uint8_t> read_file(std::string_view path) const;
private:
    [[nodiscard]] Iso9660Entry resolve(std::string_view path) const;
    [[nodiscard]] std::vector<Iso9660Entry> read_directory(const Iso9660Entry& directory) const;
    [[nodiscard]] static std::vector<std::string> split_path(std::string_view path);
    std::shared_ptr<const DiscSource> source_;
    std::uint32_t sector_size_ = 2048u;
    std::uint32_t volume_start_lba_ = 0u;
    std::uint32_t extent_lba_bias_ = 0u;
    Iso9660Entry root_;
};

} // namespace katana::runtime
