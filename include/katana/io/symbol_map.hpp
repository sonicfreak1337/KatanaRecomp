#pragma once

#include "katana/io/executable_image.hpp"

#include <filesystem>

namespace katana::io {

void load_symbol_map(
    const std::filesystem::path& path,
    ExecutableImage& image
);

}
