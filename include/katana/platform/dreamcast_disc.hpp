#pragma once

#include "katana/io/executable_image.hpp"
#include "katana/runtime/disc_install.hpp"
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
inline constexpr std::uint32_t dreamcast_system_bootstrap_address = 0xAC008000u;
inline constexpr std::uint32_t dreamcast_system_bootstrap_entry_address = 0xAC008300u;
inline constexpr std::uint32_t dreamcast_boot_executable_artifact_version = 1u;
inline constexpr std::string_view dreamcast_boot_executable_manifest_name =
    "boot.katana-executable";
inline constexpr std::string_view dreamcast_boot_executable_file_name = "boot.bin";
inline constexpr std::string_view dreamcast_boot_executable_recipe_name =
    "disc.katana-install";

enum class DreamcastDiscExecutionPath : std::uint8_t {
    DirectBootFile,
    NativeSystemBootstrap
};

struct DreamcastBootMetadata {
    std::string hardware_id;
    std::string boot_file_name;
};

struct DreamcastDiscBoot {
    std::shared_ptr<runtime::GdiDiscSource> source;
    DreamcastBootMetadata metadata;
    std::vector<std::uint8_t> system_bootstrap;
    std::vector<std::uint8_t> boot_file;
    std::uint32_t data_track_lba = 0u;
    std::uint32_t extent_lba_bias = 0u;
    std::size_t validated_tracks = 0u;
    bool repeated_bootstrap_reads_match = false;
    bool repeated_reads_match = false;
};

// Private, local-only bring-up input. boot_file contains retail bytes and must
// never be copied into a generated port or source repository. The accompanying
// install recipe contains hashes/layout only and remains the final user-disc
// identity contract.
struct DreamcastBootExecutableArtifact {
    std::uint32_t version = dreamcast_boot_executable_artifact_version;
    std::filesystem::path manifest_path;
    std::filesystem::path executable_path;
    std::filesystem::path install_recipe_path;
    DreamcastBootMetadata metadata;
    std::vector<std::uint8_t> boot_file;
    runtime::DiscInstallRecipe install_recipe;
    std::string project_identity;
    std::string boot_sha256;
    std::uint32_t entry_address = dreamcast_disc_boot_address;
};

[[nodiscard]] DreamcastBootMetadata
parse_dreamcast_boot_metadata(std::span<const std::uint8_t> bytes);

[[nodiscard]] DreamcastDiscBoot
load_dreamcast_gdi_boot(const std::filesystem::path& descriptor_path);

[[nodiscard]] io::ExecutableImage make_dreamcast_disc_executable(const DreamcastDiscBoot& disc);

[[nodiscard]] io::ExecutableImage
make_dreamcast_disc_executable(const DreamcastDiscBoot& disc,
                               DreamcastDiscExecutionPath execution_path);

[[nodiscard]] std::string
dreamcast_disc_project_identity(const DreamcastDiscBoot& disc);

// Extraction is immutable: an existing differing artifact is rejected rather
// than overwritten. The manifest is written last and reloaded before success is
// reported, so a completed artifact is always byte/hash/recipe verified.
[[nodiscard]] DreamcastBootExecutableArtifact
extract_dreamcast_boot_executable_artifact(
    const std::filesystem::path& descriptor_path,
    const std::filesystem::path& artifact_root);

[[nodiscard]] DreamcastBootExecutableArtifact
load_dreamcast_boot_executable_artifact(
    const std::filesystem::path& manifest_path);

[[nodiscard]] io::ExecutableImage make_dreamcast_boot_executable(
    const DreamcastBootExecutableArtifact& artifact);

} // namespace katana::platform
