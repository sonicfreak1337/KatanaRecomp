#include "katana/codegen/probe.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/platform/phase6_gate.hpp"
#include "katana/runtime/cache_control.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 45000u;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

struct FixtureDirectory {
    std::filesystem::path path = std::filesystem::current_path() / "katana-phase6-gate-fixture";
    FixtureDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directory(path);
    }
    ~FixtureDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::size_t payload_offset(const std::size_t sector, const std::size_t byte = 0u) {
    return sector * raw_sector_size + 16u + byte;
}

std::vector<std::uint8_t> make_boot_track() {
    constexpr std::size_t sectors = 22u;
    std::vector<std::uint8_t> bytes(sectors * raw_sector_size);
    for (std::size_t sector = 0u; sector < sectors; ++sector) {
        bytes[sector * raw_sector_size + 15u] = 1u;
    }

    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(),
              hardware.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x00u)));
    std::copy(boot_file.begin(),
              boot_file.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x60u)));

    const auto pvd = payload_offset(16u);
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, data_lba + 20u, payload_size, std::string(1u, '\0'), true);

    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    record(bytes, directory, data_lba + 21u, 4u, "BOOT.BIN;1", false);

    bytes[payload_offset(21u, 0u)] = 0x09u;
    bytes[payload_offset(21u, 1u)] = 0x00u;
    bytes[payload_offset(21u, 2u)] = 0x09u;
    bytes[payload_offset(21u, 3u)] = 0x00u;
    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_fixture(const std::filesystem::path& directory) {
    std::vector<std::uint8_t> low_track(24u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 24u; ++sector)
        low_track[sector * raw_sector_size + 15u] = 1u;
    write_binary(directory / "low.bin", low_track);
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", make_boot_track());
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n"
               << "1 0 4 2352 low.bin 0\n"
               << "2 30 0 2352 audio.raw 0\n"
               << "3 45000 4 2352 high.bin 0\n";
}

void execute_synthetic_block(katana::runtime::CpuState& cpu) {
    cpu.memory.write_u32(katana::runtime::sh4_cache_control_address,
                         katana::runtime::Sh4CacheControl::instruction_invalidate);
    cpu.pc += 2u;
}

void execute_no_op(katana::runtime::CpuState&) {}

void execute_without_cache_invalidation(katana::runtime::CpuState& cpu) {
    cpu.pc += 2u;
}

} // namespace

int main() {
    using namespace katana::platform;

    katana::ir::BasicBlock entry_block;
    entry_block.instructions.push_back({});
    entry_block.instructions.front().operation = katana::ir::Operation::Nop;
    katana::ir::BasicBlock later_block;
    later_block.instructions.push_back({});
    later_block.instructions.front().operation = katana::ir::Operation::Call;
    require(!katana::codegen::block_requires_call_dispatch(entry_block) &&
                katana::codegen::block_requires_call_dispatch(later_block),
            "Phase-6-Probe verwechselt Calls spaeterer Bloecke mit dem kopierten Einstiegsblock.");

    FixtureDirectory fixture;
    write_fixture(fixture.path);

    const auto disc = load_dreamcast_gdi_boot(fixture.path / "disc.gdi");
    require(disc.data_track_lba == data_lba && disc.extent_lba_bias == 0u &&
                disc.validated_tracks == 3u && disc.repeated_reads_match &&
                disc.metadata.boot_file_name == "BOOT.BIN" &&
                disc.boot_file == std::vector<std::uint8_t>({0x09u, 0x00u, 0x09u, 0x00u}),
            "Dreamcast-GDI-Boot waehlt nicht den letzten Datentrack oder verliert absolute "
            "ISO-Extents.");
    const auto first = run_phase6_gate(fixture.path / "disc.gdi", execute_synthetic_block, 1u);
    const auto second = run_phase6_gate(fixture.path / "disc.gdi", execute_synthetic_block, 1u);
    const auto serialized = serialize_phase6_gate_report(first);
    require(first.checkpoint == "KR_PHASE6_MAIN_EXECUTION_STARTED" && first.executed_blocks == 1u &&
                first.guest_cycles == 1u && first.scheduler_events != 0u &&
                first.gdrom_completions == 1u && first.tmu_events != 0u && first.dma_events == 1u &&
                first.interrupts_delivered == 1u && first.cache_invalidations == 1u &&
                first.silent_failures == 0u,
            "Phase-6-Gate erreicht die messbaren Plattformkriterien nicht.");
    require(serialized == serialize_phase6_gate_report(second),
            "Zwei identische Phase-6-Laeufe liefern unterschiedliche Kernmetriken.");
    require(serialized.find(fixture.path.string()) == std::string::npos &&
                serialized.find("disc.gdi") == std::string::npos &&
                serialized.find("BOOT.BIN") == std::string::npos,
            "Phase-6-Bericht enthaelt lokale Pfade oder Disc-Metadaten.");

    std::vector<std::uint8_t> invalid(0x70u, static_cast<std::uint8_t>(' '));
    require(throws<std::runtime_error>([&] {
                static_cast<void>(run_phase6_gate(fixture.path / "disc.gdi", execute_no_op, 1u));
            }),
            "Phase-6-Gate akzeptiert einen Executor ohne beobachtbare Blockausfuehrung.");
    require(throws<std::runtime_error>([&] {
                static_cast<void>(run_phase6_gate(
                    fixture.path / "disc.gdi", execute_without_cache_invalidation, 1u));
            }),
            "Phase-6-Gate akzeptiert eine fehlende CCR-Invalidierung.");
    require(throws<std::invalid_argument>(
                [&] { static_cast<void>(parse_dreamcast_boot_metadata(invalid)); }),
            "Ungueltige Dreamcast-Bootmetadaten werden akzeptiert.");
    require(throws<std::invalid_argument>([&] {
                static_cast<void>(
                    run_phase6_gate(fixture.path / "disc.gdi", execute_synthetic_block, 65u, 64u));
            }),
            "Phase-6-Gate akzeptiert einen Block oberhalb des Gastzyklusbudgets.");

    std::cout << "v0.31.0 Phase-6-Gate-Infrastruktur erfolgreich.\n";
    return EXIT_SUCCESS;
}
