#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace katana::io {

inline constexpr std::uint32_t project_manifest_current_version = 2u;
inline constexpr std::string_view project_manifest_schema_name = "katana-project";

enum class ProjectInputFormat {
    RawBinary,
    Elf32Sh
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
};

[[nodiscard]] ProjectManifest parse_project_manifest(
    const std::filesystem::path& path
);

[[nodiscard]] ExecutableImage load_project_manifest(
    const std::filesystem::path& path
);

[[nodiscard]] const char* project_input_format_name(ProjectInputFormat format) noexcept;
[[nodiscard]] bool project_manifest_version_supported(std::uint32_t version) noexcept;

}
