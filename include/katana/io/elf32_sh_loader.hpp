#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace katana::io {

[[nodiscard]] ExecutableImage load_elf32_sh(
    const std::filesystem::path& path
);

[[nodiscard]] ExecutableImage load_elf32_sh(
    std::span<const std::uint8_t> bytes,
    const std::filesystem::path& synthetic_source
);

}
