#include "katana/runtime/gdi.hpp"
#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr std::size_t sector_size = 2048u;
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
struct FixtureDirectory {
    std::filesystem::path path = std::filesystem::current_path() / "katana-gdi-integration-fixture";
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
    for (std::size_t i = 0u; i < 4u; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
        bytes[offset + 4u + i] = static_cast<std::uint8_t>(value >> ((3u - i) * 8u));
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
std::vector<std::uint8_t> make_data_track() {
    std::vector<std::uint8_t> bytes(24u * sector_size);
    const auto pvd = 16u * sector_size;
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, 20u, sector_size, std::string(1u, '\0'), true);
    auto offset = 20u * sector_size;
    offset += record(bytes, offset, 20u, sector_size, std::string(1u, '\0'), true);
    offset += record(bytes, offset, 20u, sector_size, std::string(1u, '\1'), true);
    record(bytes, offset, 21u, 4u, "BOOT.BIN;1", false);
    bytes[21u * sector_size] = 0x11u;
    bytes[21u * sector_size + 1u] = 0x22u;
    bytes[21u * sector_size + 2u] = 0x33u;
    bytes[21u * sector_size + 3u] = 0x44u;
    return bytes;
}
void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}
void write_fixture(const std::filesystem::path& directory) {
    std::filesystem::create_directory(directory);
    write_binary(directory / "audio.bin", std::vector<std::uint8_t>(2u * 2352u, 0x5Au));
    write_binary(directory / "data.bin", make_data_track());
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "2\n1 0 0 2352 audio.bin 0\n2 10 4 2048 data.bin 0\n";
}

void write_multidata_fixture(const std::filesystem::path& directory) {
    std::filesystem::create_directory(directory);
    write_binary(directory / "boot.bin", make_data_track());
    write_binary(directory / "content.bin", std::vector<std::uint8_t>(2u * sector_size));
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n1 0 4 2048 boot.bin 0\n"
                  "2 45000 4 2048 boot.bin 0\n"
                  "3 45024 4 2048 content.bin 0\n";
}
} // namespace

int main(const int argc, const char* const* argv) {
    using namespace katana::runtime;
    FixtureDirectory fixture;
    const auto first_directory = fixture.path / "first";
    const auto second_directory = fixture.path / "second";
    write_fixture(first_directory);
    write_fixture(second_directory);
    const auto first = GdiDiscSource::open(first_directory / "disc.gdi");
    const auto second = GdiDiscSource::open(second_directory / "disc.gdi");
    require(first->identity().starts_with("gdi-sha256:") && first->identity().size() == 75u &&
                first->identity() == second->identity(),
            "GDI-Identitaet ist nicht inhaltsstabil oder haengt vom Hostpfad ab.");
    require(first->primary_data_lba() == 10u && first->descriptor().tracks.size() == 2u,
            "GDI-Datentrack oder Trackzuordnung ist falsch.");
    const auto multidata_directory = fixture.path / "multidata";
    write_multidata_fixture(multidata_directory);
    const auto multidata = GdiDiscSource::open(multidata_directory / "disc.gdi");
    require(multidata->primary_data_lba() == 45000u,
            "Ein spaeterer Datentrack wurde faelschlich als Dreamcast-Boottrack gewaehlt.");
    require(first->descriptor().sha256.size() == 64u &&
                std::all_of(first->descriptor().tracks.begin(),
                            first->descriptor().tracks.end(),
                            [](const auto& track) { return track.sha256.size() == 64u; }) &&
                first->io_counters().persistent_track_opens == 2u,
            "GDI-Provenienzhashes werden nicht zwischen I/O und Export wiederverwendbar gehalten.");
    const auto audio = first->read_raw_sector(1u, 1u);
    require(audio.size() == 2352u && audio.front() == 0x5Au && audio.back() == 0x5Au,
            "GDI-Audiotrack ist nicht als stabiler Raw-Sektor lesbar.");
    GdRomDrive drive(first);
    const auto sector = drive.execute({GdRomCommand::ReadSectors, 10u, 1u});
    require(sector.status == GdRomStatus::Good && sector.data.size() == 2048u,
            "GDI-Datentrack ist nicht ueber den GD-ROM-Pfad lesbar.");
    first->set_cache_mode(DiscCacheMode::DisabledReference);
    first->reset_io_counters();
    const auto reference_batch = first->read(10u * sector_size, 8u * sector_size);
    require(first->io_counters().raw_read_operations == 1u,
            "Sequenzieller GDI-Batch wird in einzelne Hostreads zerlegt.");
    first->set_cache_mode(DiscCacheMode::Enabled);
    first->reset_io_counters();
    const auto cached_batch = first->read(10u * sector_size, 8u * sector_size);
    const auto reads_after_fill = first->io_counters().raw_read_operations;
    const auto cached_repeat = first->read(10u * sector_size, 8u * sector_size);
    require(reference_batch == cached_batch && cached_batch == cached_repeat &&
                first->io_counters().raw_read_operations == reads_after_fill &&
                first->io_counters().sector_cache_hits == 8u &&
                first->sector_cache_size() <= first->sector_cache_capacity(),
            "GDI-Sektorcache veraendert Bytes, liest Hits erneut oder waechst ungebremst.");
    Iso9660Filesystem filesystem(first, 2048u, first->primary_data_lba());
    require(filesystem.read_file("/BOOT.BIN") ==
                std::vector<std::uint8_t>({0x11u, 0x22u, 0x33u, 0x44u}),
            "GDI-Datentrack erreicht den ISO9660-Pfad nicht.");
    require(std::filesystem::file_size(first_directory / "data.bin") == 24u * sector_size,
            "GDI-Integration veraendert eine Trackdatei.");

    if (argc == 2) {
        const auto local = GdiDiscSource::open(argv[1]);
        require(!local->descriptor().tracks.empty(), "Lokale GDI-Quelle besitzt keine Tracks.");
        Iso9660Filesystem local_filesystem(local, 2048u, local->primary_data_lba(), 0u);
        static_cast<void>(local_filesystem.list_directory());
        std::cout << "Lokaler read-only GDI-Smoke-Test erfolgreich.\n";
    }
    std::cout << "KR-3006 GDI-Quellenintegration erfolgreich.\n";
}
