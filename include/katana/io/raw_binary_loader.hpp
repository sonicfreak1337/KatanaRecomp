#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace katana::io {

struct RawBinaryLoadOptions {
    std::uint32_t base_address = 0;
    std::string segment_name = ".raw";
    SegmentKind segment_kind = SegmentKind::Code;
    SegmentPermissions permissions{true, false, true};
    std::optional<std::uint32_t> entry_point;
};

[[nodiscard]] ExecutableImage load_raw_binary(const std::filesystem::path& path,
                                              const RawBinaryLoadOptions& options = {});

} // namespace katana::io
