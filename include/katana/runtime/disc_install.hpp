#pragma once

#include "katana/runtime/gdi.hpp"
#include "katana/runtime/packed_disc.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t disc_install_recipe_version = 1u;

struct DiscInstallTrack {
    std::uint32_t number = 0u;
    std::uint32_t lba = 0u;
    GdiTrackType type = GdiTrackType::Data;
    std::uint32_t sector_size = 0u;
    std::uint64_t file_offset = 0u;
    std::uint64_t sector_count = 0u;
    std::string sha256;
};

struct DiscInstallRecipe {
    std::uint32_t version = disc_install_recipe_version;
    std::string job_generation;
    std::string descriptor_sha256;
    std::string content_identity;
    std::string boot_sha256;
    std::vector<DiscInstallTrack> tracks;
};

[[nodiscard]] DiscInstallRecipe make_disc_install_recipe(const GdiDiscSource& source,
                                                         std::string job_generation,
                                                         std::string boot_sha256);
[[nodiscard]] std::string format_disc_install_recipe(const DiscInstallRecipe& recipe);
[[nodiscard]] DiscInstallRecipe parse_disc_install_recipe(const std::filesystem::path& path);
void verify_disc_install_source(const DiscInstallRecipe& recipe, const GdiDiscSource& source);
[[nodiscard]] PackedDiscInfo install_disc_content(const DiscInstallRecipe& recipe,
                                                  const std::filesystem::path& gdi_path,
                                                  const std::filesystem::path& destination);

} // namespace katana::runtime
