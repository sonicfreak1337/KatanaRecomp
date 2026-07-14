#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace katana::io {

enum class ProjectInputFormat {
    RawBinary,
    Elf32Sh
};

struct ProjectManifest {
    std::uint32_t version = 1u;
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

}
