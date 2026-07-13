#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace katana::io {

[[nodiscard]] std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path
);

[[nodiscard]] std::uint16_t read_u16_le(
    std::span<const std::uint8_t> bytes,
    std::size_t offset
);

}
