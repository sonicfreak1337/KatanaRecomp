#include "katana/platform/dreamcast.hpp"

#include "katana/platform/firmware_profile.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace katana::platform {
namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

bool fits_main_ram_alias(const std::uint32_t address, const std::uint64_t size) {
    if (size > runtime::dreamcast_main_ram_size) {
        return false;
    }
    for (const auto area : runtime::dreamcast_main_ram_area_bases) {
        for (std::size_t mirror = 0u; mirror < runtime::dreamcast_main_ram_mirrors_per_area;
             ++mirror) {
            const std::uint64_t base = area + mirror * runtime::dreamcast_main_ram_size;
            const std::uint64_t start = address;
            if (start >= base && start + size <= base + runtime::dreamcast_main_ram_size) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

DreamcastBootResult boot_homebrew(runtime::CpuState& cpu,
                                  const io::ExecutableImage& image,
                                  const DreamcastBootConfig& config) {
    require_alpha_firmware_profile(config.firmware_mode);
    if (image.segments().empty()) {
        throw std::invalid_argument("Homebrew-Abbild besitzt keine ladbaren Segmente.");
    }
    const auto entry = config.entry_point.has_value()
                           ? *config.entry_point
                           : (image.entry_points().empty() ? 0u : image.entry_points().front());
    if (!config.entry_point.has_value() && image.entry_points().empty()) {
        throw std::invalid_argument("Homebrew-Abbild besitzt keinen Einstiegspunkt.");
    }
    const auto* entry_segment = image.find_segment(entry);
    if (entry_segment == nullptr || !entry_segment->permissions.executable) {
        throw std::invalid_argument("Homebrew-Einstiegspunkt liegt in keinem Segment.");
    }
    if (cpu.memory.region_count() != 0u) {
        throw std::invalid_argument(
            "Dreamcast-Boot erwartet einen noch nicht abgebildeten Speicherbus.");
    }

    for (const auto& segment : image.segments()) {
        if (segment.memory_size < segment.bytes.size()) {
            throw std::invalid_argument(
                "Segmentdateien sind groesser als ihre Speicherausdehnung.");
        }
        if (!fits_main_ram_alias(segment.virtual_address, segment.memory_size)) {
            throw std::invalid_argument(
                "Homebrew-Segment liegt ausserhalb des Dreamcast-Hauptspeichers.");
        }
    }

    static_cast<void>(runtime::map_dreamcast_main_ram(cpu.memory));
    DreamcastBootResult result;
    result.entry_point = entry;
    result.log.push_back(config.firmware_mode == FirmwareMode::HleBiosAbi
                             ? "firmware=hle-bios-abi"
                             : "firmware=direct-homebrew");
    result.log.push_back("entry=" + hex32(entry));

    for (const auto& segment : image.segments()) {
        for (std::size_t offset = 0u; offset < static_cast<std::size_t>(segment.memory_size);
             ++offset) {
            const auto value =
                offset < segment.bytes.size() ? segment.bytes[offset] : std::uint8_t{0u};
            cpu.memory.write_u8(segment.virtual_address + static_cast<std::uint32_t>(offset),
                                value);
        }
        ++result.loaded_segments;
        result.loaded_bytes += static_cast<std::size_t>(segment.memory_size);
        result.log.push_back("segment=" + segment.name + "@" + hex32(segment.virtual_address));
    }

    runtime::reset_cpu(
        cpu,
        runtime::ResetState{
            entry, config.stack_pointer, config.vector_base, config.status_register, config.fpscr});
    result.log.push_back("boot=ready");
    return result;
}

DreamcastBootResult boot_raw_homebrew(runtime::CpuState& cpu,
                                      const std::filesystem::path& path,
                                      const io::RawBinaryLoadOptions& load_options,
                                      const DreamcastBootConfig& config) {
    return boot_homebrew(cpu, io::load_raw_binary(path, load_options), config);
}

DreamcastBootResult boot_elf_homebrew(runtime::CpuState& cpu,
                                      const std::filesystem::path& path,
                                      const DreamcastBootConfig& config) {
    return boot_homebrew(cpu, io::load_elf32_sh(path), config);
}

} // namespace katana::platform
