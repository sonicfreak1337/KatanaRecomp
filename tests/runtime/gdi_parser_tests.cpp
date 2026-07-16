#include "katana/runtime/gdi.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
struct FixtureDirectory {
    std::filesystem::path path = std::filesystem::current_path() / "katana-gdi-parser-fixture";
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
void write_bytes(const std::filesystem::path& path, const std::size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::vector<char> bytes(size, 0);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::trunc);
    output << text;
}
} // namespace

int main() {
    using namespace katana::runtime;
    FixtureDirectory fixture;
    write_bytes(fixture.path / "track 01.bin", 2u * 2352u);
    write_bytes(fixture.path / "track02.bin", 3u * 2048u);
    const auto descriptor = fixture.path / "synthetic.gdi";
    write_text(descriptor, "2\n1 0 0 2352 \"track 01.bin\" 0\n2 450 4 2048 track02.bin 0\n");
    const auto parsed = parse_gdi_descriptor(descriptor);
    require(parsed.descriptor_name == "synthetic.gdi" && parsed.tracks.size() == 2u &&
                parsed.tracks[0].type == GdiTrackType::Audio &&
                parsed.tracks[0].sector_count == 2u && parsed.tracks[0].descriptor_line == 2u &&
                parsed.tracks[1].lba == 450u && parsed.tracks[1].sector_count == 3u &&
                parsed.tracks[1].resolved_path.parent_path() == fixture.path,
            "GDI-Parser verliert quoted Pfad, Provenienz, Tracktyp, LBA oder Sektorzahl.");
    require(std::filesystem::file_size(fixture.path / "track 01.bin") == 2u * 2352u,
            "GDI-Parser veraendert Trackdateien.");

    write_text(descriptor, "2\n1 0 0 2352 \"track 01.bin\" 0\n");
    require(
        throws<std::runtime_error>([&] { static_cast<void>(parse_gdi_descriptor(descriptor)); }),
        "GDI-Parser akzeptiert eine falsche Trackanzahl.");
    write_text(descriptor, "1\n1 0 4 2048 missing.bin 0\n");
    require(
        throws<std::runtime_error>([&] { static_cast<void>(parse_gdi_descriptor(descriptor)); }),
        "GDI-Parser akzeptiert eine fehlende Trackdatei.");
    write_bytes(fixture.path / "bad.bin", 2049u);
    write_text(descriptor, "1\n1 0 4 2048 bad.bin 0\n");
    require(
        throws<std::runtime_error>([&] { static_cast<void>(parse_gdi_descriptor(descriptor)); }),
        "GDI-Parser akzeptiert einen Dateigroessenkonflikt.");
    write_text(descriptor, "2\n1 0 0 2352 \"track 01.bin\" 0\n1 450 4 2048 track02.bin 0\n");
    try {
        static_cast<void>(parse_gdi_descriptor(descriptor));
        require(false, "GDI-Parser akzeptiert doppelte Tracks.");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        require(message.find("synthetic.gdi") != std::string::npos &&
                    message.find("Zeile 3") != std::string::npos,
                "GDI-Diagnose verliert Descriptorname oder Zeilenprovenienz.");
    }
    write_text(descriptor, "2\n1 0 0 2352 \"track 01.bin\" 0\n2 1 4 2048 track02.bin 0\n");
    require(
        throws<std::runtime_error>([&] { static_cast<void>(parse_gdi_descriptor(descriptor)); }),
        "GDI-Parser akzeptiert ueberlappende Track-LBA-Bereiche.");

    const auto escaped_track = fixture.path.parent_path() / "katana-gdi-escaped-track.bin";
    write_bytes(escaped_track, 2048u);
    write_text(descriptor, "1\n1 0 4 2048 ../katana-gdi-escaped-track.bin 0\n");
    require(
        throws<std::runtime_error>([&] { static_cast<void>(parse_gdi_descriptor(descriptor)); }),
        "GDI-Parser akzeptiert einen Track ausserhalb des Descriptorverzeichnisses.");
    std::filesystem::remove(escaped_track);

    std::cout << "KR-3005 GDI-Deskriptoren und Trackmodell erfolgreich.\n";
}
