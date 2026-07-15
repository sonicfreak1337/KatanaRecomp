#include "katana/platform/dreamcast.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

int main() {
    using namespace katana;
    io::ExecutableImage image;
    io::ImageSegment segment;
    segment.name = ".text";
    segment.virtual_address = 0x8C010000u;
    segment.memory_size = 8u;
    segment.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    segment.permissions = {true, false, true};
    image.add_segment(std::move(segment));
    image.add_entry_point(0x8C010000u);

    runtime::CpuState cpu;
    cpu.memory = runtime::Memory(0u);
    const auto result = platform::boot_homebrew(cpu, image);
    require(cpu.pc == 0x8C010000u && cpu.r[15] == 0x8D000000u,
        "Direktboot setzt PC oder Stack nicht deterministisch.");
    require(cpu.memory.read_u32(0x8C010000u) == 0x000B0009u &&
        cpu.memory.read_u32(0x8C010004u) == 0u,
        "Direktboot kopiert Datei- oder BSS-Anteil falsch.");
    require(result.loaded_segments == 1u && result.loaded_bytes == 8u &&
        result.log.front() == "firmware=direct-homebrew" && result.log.back() == "boot=ready",
        "Minimales Plattformlogging ist unvollstaendig.");

    runtime::CpuState unsupported;
    unsupported.memory = runtime::Memory(0u);
    platform::DreamcastBootConfig lle;
    lle.firmware_mode = platform::FirmwareMode::LleFirmware;
    require(throws<std::invalid_argument>([&] { static_cast<void>(platform::boot_homebrew(unsupported, image, lle)); }),
        "Nicht unterstuetzter LLE-Modus scheitert nicht sichtbar.");

    io::ExecutableImage empty;
    runtime::CpuState missing;
    missing.memory = runtime::Memory(0u);
    require(throws<std::invalid_argument>([&] { static_cast<void>(platform::boot_homebrew(missing, empty)); }),
        "Fehlende Segmente oder Einstiegspunkte werden nicht abgewiesen.");

    io::ExecutableImage invalid;
    io::ImageSegment valid_segment;
    valid_segment.name = ".text";
    valid_segment.virtual_address = 0x8C030000u;
    valid_segment.memory_size = 2u;
    valid_segment.bytes = {0x09u, 0u};
    valid_segment.permissions = {true, false, true};
    invalid.add_segment(std::move(valid_segment));
    io::ImageSegment invalid_segment;
    invalid_segment.name = ".bad";
    invalid_segment.virtual_address = 0x01000000u;
    invalid_segment.memory_size = 1u;
    invalid_segment.bytes = {0u};
    invalid.add_segment(std::move(invalid_segment));
    invalid.add_entry_point(0x8C030000u);
    runtime::CpuState atomic;
    atomic.memory = runtime::Memory(0u);
    require(
        throws<std::invalid_argument>([&] { static_cast<void>(platform::boot_homebrew(atomic, invalid)); }) &&
        atomic.memory.region_count() == 0u,
        "Fehlerhafte spaetere Segmente hinterlassen einen halben Plattformboot."
    );

    const auto raw_path = std::filesystem::temp_directory_path() / "katana-v026-homebrew.bin";
    { std::ofstream out(raw_path, std::ios::binary | std::ios::trunc); const char bytes[2] = {'\x09', '\x00'}; out.write(bytes, 2); }
    runtime::CpuState raw_cpu;
    raw_cpu.memory = runtime::Memory(0u);
    io::RawBinaryLoadOptions raw_options;
    raw_options.base_address = 0x8C020000u;
    raw_options.entry_point = 0x8C020000u;
    const auto raw_result = platform::boot_raw_homebrew(raw_cpu, raw_path, raw_options);
    std::filesystem::remove(raw_path);
    require(raw_result.entry_point == 0x8C020000u && raw_cpu.memory.read_u16(0x8C020000u) == 0x0009u,
        "Raw-Homebrew erreicht den Plattformboot nicht.");

    std::cout << "BIOS-freier Dreamcast-Homebrew-Boot erfolgreich.\n";
}
