#pragma once

#include "katana/io/executable_image.hpp"

#include <filesystem>

namespace katana::io {

[[nodiscard]] ExecutableImage load_elf32_sh(
    const std::filesystem::path& path
);

}
