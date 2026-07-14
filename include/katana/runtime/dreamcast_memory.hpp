#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace katana::runtime {

inline constexpr std::size_t dreamcast_main_ram_size = 0x01000000u;
inline constexpr std::size_t dreamcast_main_ram_mirrors_per_area = 4u;
inline constexpr std::size_t dreamcast_vram_size = 0x00800000u;
inline constexpr std::size_t dreamcast_aica_ram_size = 0x00200000u;
inline constexpr std::size_t dreamcast_bios_size = 0x00200000u;
inline constexpr std::size_t dreamcast_flash_size = 0x00020000u;
inline constexpr std::uint32_t dreamcast_vram_bank_size = 0x00400000u;
inline constexpr std::uint32_t dreamcast_bios_physical_base = 0x00000000u;
inline constexpr std::uint32_t dreamcast_flash_physical_base = 0x00200000u;

[[nodiscard]] constexpr std::uint32_t
dreamcast_vram_32bit_to_linear_offset(
    const std::uint32_t offset
) noexcept {
    constexpr std::uint32_t bytes_per_word = 4u;

    const auto bank = offset / dreamcast_vram_bank_size;
    const auto offset_in_bank = offset % dreamcast_vram_bank_size;
    const auto word_in_bank = offset_in_bank / bytes_per_word;
    const auto byte_in_word = offset_in_bank % bytes_per_word;

    return
        ((word_in_bank * 2u + bank) * bytes_per_word) +
        byte_in_word;
}

inline constexpr std::array<std::uint32_t, 7>
    dreamcast_direct_segment_bases = {
        0x00000000u,
        0x20000000u,
        0x40000000u,
        0x60000000u,
        0x80000000u,
        0xA0000000u,
        0xC0000000u
    };

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

inline constexpr std::array<std::uint32_t, 4>
    dreamcast_vram_64bit_physical_bases = {
        0x04000000u,
        0x04800000u,
        0x06000000u,
        0x06800000u
    };

inline constexpr std::array<std::uint32_t, 4>
    dreamcast_vram_32bit_physical_bases = {
        0x05000000u,
        0x05800000u,
        0x07000000u,
        0x07800000u
    };

inline constexpr std::array<std::uint32_t, 4>
    dreamcast_aica_ram_physical_bases = {
        0x00800000u,
        0x00A00000u,
        0x00C00000u,
        0x00E00000u
    };

inline constexpr std::size_t dreamcast_main_ram_alias_count =
    dreamcast_main_ram_area_bases.size() *
    dreamcast_main_ram_mirrors_per_area;
inline constexpr std::size_t dreamcast_vram_64bit_alias_count =
    dreamcast_direct_segment_bases.size() *
    dreamcast_vram_64bit_physical_bases.size();
inline constexpr std::size_t dreamcast_vram_32bit_alias_count =
    dreamcast_direct_segment_bases.size() *
    dreamcast_vram_32bit_physical_bases.size();
inline constexpr std::size_t dreamcast_vram_alias_count =
    dreamcast_vram_64bit_alias_count +
    dreamcast_vram_32bit_alias_count;
inline constexpr std::size_t dreamcast_aica_ram_alias_count =
    dreamcast_direct_segment_bases.size() *
    dreamcast_aica_ram_physical_bases.size();
inline constexpr std::size_t dreamcast_bios_alias_count =
    dreamcast_direct_segment_bases.size();
inline constexpr std::size_t dreamcast_flash_alias_count =
    dreamcast_direct_segment_bases.size();

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_main_ram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_vram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_aica_ram(Memory& memory);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_bios(
    Memory& memory,
    std::span<const std::uint8_t> image = {}
);

[[nodiscard]] std::shared_ptr<LinearMemoryDevice>
map_dreamcast_flash(
    Memory& memory,
    std::span<const std::uint8_t> image = {}
);

} // namespace katana::runtime