#pragma once

#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace katana::platform {

enum class FirmwareMode {
    DirectHomebrew,
    HleBiosAbi,
    LleFirmware
};

struct DreamcastBootConfig {
    FirmwareMode firmware_mode = FirmwareMode::DirectHomebrew;
    std::uint32_t stack_pointer = 0x8D000000u;
    std::uint32_t vector_base = 0u;
    std::uint32_t status_register = 0u;
    std::uint32_t fpscr = 0u;
    std::optional<std::uint32_t> entry_point;
};

struct DreamcastBootResult {
    std::uint32_t entry_point = 0u;
    std::size_t loaded_segments = 0u;
    std::size_t loaded_bytes = 0u;
    std::vector<std::string> log;
};

[[nodiscard]] DreamcastBootResult boot_homebrew(
    runtime::CpuState& cpu,
    const io::ExecutableImage& image,
    const DreamcastBootConfig& config = {}
);

[[nodiscard]] DreamcastBootResult boot_raw_homebrew(
    runtime::CpuState& cpu,
    const std::filesystem::path& path,
    const io::RawBinaryLoadOptions& load_options,
    const DreamcastBootConfig& config = {}
);

[[nodiscard]] DreamcastBootResult boot_elf_homebrew(
    runtime::CpuState& cpu,
    const std::filesystem::path& path,
    const DreamcastBootConfig& config = {}
);

} // namespace katana::platform
