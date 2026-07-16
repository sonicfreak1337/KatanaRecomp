#pragma once

#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/gdi.hpp"
#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t dreamcast_disc_boot_address = 0x8C010000u;
inline constexpr std::uint32_t dreamcast_direct_boot_stack = 0x8D000000u;

struct DreamcastRuntimeBootImage {
    std::shared_ptr<GdiDiscSource> source;
    std::string hardware_id;
    std::string boot_file_name;
    std::vector<std::uint8_t> boot_file;
    std::uint32_t data_track_lba = 0u;
    std::uint32_t extent_lba_bias = 0u;
    std::size_t validated_tracks = 0u;
    bool repeated_reads_match = false;
};

struct DreamcastRuntimeState {
    std::shared_ptr<LinearMemoryDevice> main_ram;
    std::shared_ptr<LinearMemoryDevice> vram;
    std::shared_ptr<LinearMemoryDevice> aica_ram;
    std::shared_ptr<FlashMemoryDevice> flash;
    std::size_t loaded_boot_bytes = 0u;
};

[[nodiscard]] DreamcastRuntimeBootImage
load_dreamcast_runtime_boot(const std::filesystem::path& descriptor_path);

[[nodiscard]] DreamcastRuntimeState
initialize_dreamcast_runtime(CpuState& cpu, const DreamcastRuntimeBootImage& boot);

} // namespace katana::runtime
