#include "katana/runtime/dreamcast_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace katana::runtime {
namespace {

std::uint32_t alias_base(
    const std::uint32_t area_base,
    const std::size_t mirror_index
) {
    return area_base + static_cast<std::uint32_t>(
        mirror_index * dreamcast_main_ram_size
    );
}

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output
        << "0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << address;
    return output.str();
}

bool overlaps(
    const MemoryRegionInfo& existing,
    const std::uint32_t candidate_base
) {
    const std::uint64_t existing_start = existing.base_address;
    const std::uint64_t existing_end =
        existing_start + existing.size;
    const std::uint64_t candidate_start = candidate_base;
    const std::uint64_t candidate_end =
        candidate_start + dreamcast_main_ram_size;

    return
        candidate_start < existing_end &&
        existing_start < candidate_end;
}

void require_free_aliases(const Memory& memory) {
    for (const auto area_base : dreamcast_main_ram_area_bases) {
        for (
            std::size_t mirror_index = 0u;
            mirror_index < dreamcast_main_ram_mirrors_per_area;
            ++mirror_index
        ) {
            const auto candidate_base =
                alias_base(area_base, mirror_index);

            for (
                std::size_t region_index = 0u;
                region_index < memory.region_count();
                ++region_index
            ) {
                const auto& existing = memory.region(region_index);
                if (overlaps(existing, candidate_base)) {
                    throw std::invalid_argument(
                        "Dreamcast-Haupt-RAM-Alias " +
                        hex_address(candidate_base) +
                        " kollidiert mit Region '" +
                        existing.name + "'."
                    );
                }
            }
        }
    }
}

} // namespace

std::shared_ptr<LinearMemoryDevice>
map_dreamcast_main_ram(Memory& memory) {
    require_free_aliases(memory);

    auto main_ram =
        std::make_shared<LinearMemoryDevice>(dreamcast_main_ram_size);

    for (const auto area_base : dreamcast_main_ram_area_bases) {
        for (
            std::size_t mirror_index = 0u;
            mirror_index < dreamcast_main_ram_mirrors_per_area;
            ++mirror_index
        ) {
            const auto base = alias_base(area_base, mirror_index);
            memory.map_region(
                "dreamcast-main-ram-" + hex_address(base),
                base,
                main_ram
            );
        }
    }

    return main_ram;
}

} // namespace katana::runtime
