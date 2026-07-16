#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::io {

inline constexpr std::uint32_t project_manifest_current_version = 2u;
inline constexpr std::string_view project_manifest_schema_name = "katana-project";

enum class ProjectInputFormat {
    RawBinary,
    Elf32Sh
};

enum class ProjectFirmwareMode {
    Direct,
    Hle,
    Lle
};

enum class ProjectFallbackPolicy {
    Abort,
    Interpreter,
    Diagnostic
};

enum class ProjectMmuProfile {
    Disabled,
    Sh4
};

enum class ProjectFastpathProfile {
    Conservative,
    Guarded
};

struct ProjectAddressRange {
    std::uint32_t start = 0u;
    std::uint32_t size = 0u;
};

struct ProjectAliasGroup {
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_start = 0u;
    std::uint32_t size = 0u;
};

struct ProjectManifest {
    std::uint32_t version = project_manifest_current_version;
    std::string schema = std::string(project_manifest_schema_name);
    std::string project_name;
    ProjectInputFormat format = ProjectInputFormat::RawBinary;
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> map_path;
    std::optional<std::uint32_t> base_address;
    std::optional<std::uint32_t> entry_point;
    std::string segment_name = ".raw";
    SegmentKind segment_kind = SegmentKind::Code;
    SegmentPermissions permissions{true, false, true};
    ProjectFirmwareMode firmware_mode = ProjectFirmwareMode::Direct;
    ProjectFallbackPolicy fallback_policy = ProjectFallbackPolicy::Abort;
    std::string scheduler_profile = "deterministic";
    ProjectMmuProfile mmu_profile = ProjectMmuProfile::Disabled;
    ProjectFastpathProfile fastpath_profile = ProjectFastpathProfile::Conservative;
    std::optional<std::filesystem::path> bios_path;
    std::optional<std::filesystem::path> flash_path;
    std::vector<std::string> required_backend_capabilities;
    std::vector<ProjectAliasGroup> alias_groups;
    std::vector<ProjectAddressRange> canonical_physical_ranges;
    std::vector<ProjectAddressRange> writable_executable_ranges;
    std::vector<std::uint32_t> expected_entry_points;
    std::vector<std::uint32_t> dynamic_bios_vectors;
};

[[nodiscard]] ProjectManifest parse_project_manifest(
    const std::filesystem::path& path
);

[[nodiscard]] ExecutableImage load_project_manifest(
    const std::filesystem::path& path
);

[[nodiscard]] const char* project_input_format_name(ProjectInputFormat format) noexcept;
[[nodiscard]] const char* project_firmware_mode_name(ProjectFirmwareMode mode) noexcept;
[[nodiscard]] const char* project_fallback_policy_name(ProjectFallbackPolicy policy) noexcept;
[[nodiscard]] bool project_manifest_version_supported(std::uint32_t version) noexcept;

void require_valid_project_alias_groups(
    std::span<const ProjectAliasGroup> aliases,
    std::span<const ProjectAddressRange> canonical_ranges
);

}
