#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::size_t dreamcast_main_ram_size = 0x01000000u;
inline constexpr std::size_t dreamcast_main_ram_mirrors_per_area = 4u;

inline constexpr std::array<std::uint32_t, 7>
    dreamcast_main_ram_area_bases = {
        0x0C000000u,
        0x2C000000u,
        0x4C000000u,
        0x6C000000u,
        0x8C000000u,
        0xAC000000u,
        0xCC000000u
    };

inline constexpr std::size_t dreamcast_main_ram_alias_count =
    dreamcast_main_ram_area_bases.size() *
    dreamcast_main_ram_mirrors_per_area;

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_main_ram(Memory& memory);

} // namespace katana::runtime
