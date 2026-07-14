#include "katana/io/project_manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void save_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
}

template <typename Function>
std::string require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        return error.what();
    }
    require(false, message);
    return {};
}

}

int main() {
    using namespace katana::io;
    const auto directory = std::filesystem::current_path() / "katana-manifest-fixture";
    std::filesystem::create_directory(directory);
    const auto binary = directory / "program.bin";
    const auto map = directory / "program.map";
    const auto manifest_path = directory / "program.katana";
    {
        std::ofstream output(binary, std::ios::binary);
        const char bytes[]{'\x09', '\x00', '\x0B', '\x00'};
        output.write(bytes, 4);
    }
    save_text(map, "8C010000 FUNC start 4\n");
    save_text(
        manifest_path,
        "# KatanaRecomp project manifest\n"
        "version = 1\n"
        "format = raw\n"
        "input = program.bin\n"
        "base_address = 0x8C010000\n"
        "entry_point = 8C010000\n"
        "segment_name = .text\n"
        "segment_kind = code\n"
        "permissions = r-x\n"
        "map = program.map\n"
    );

    const auto manifest = parse_project_manifest(manifest_path);
    require(manifest.version == 1u && manifest.format == ProjectInputFormat::RawBinary, "Manifestkopf ist falsch.");
    require(manifest.input_path == binary && manifest.map_path == map, "Relative Manifestpfade wurden falsch aufgeloest.");
    require(manifest.base_address == 0x8C010000u && manifest.entry_point == 0x8C010000u, "Adresslayout ist falsch.");
    require(std::string(project_input_format_name(manifest.format)) == "raw", "Formatname ist instabil.");

    const auto image = load_project_manifest(manifest_path);
    require(image.source_path() == binary, "Manifestloader verlor die eigentliche Eingabedatei.");
    require(image.segments().size() == 1u && image.segments()[0].name == ".text", "Raw-Segmentlayout wurde nicht angewendet.");
    require(image.entry_points().size() == 1u && image.entry_points()[0] == 0x8C010000u, "Manifest-Einstiegspunkt fehlt.");
    require(image.find_symbol("start") != nullptr, "Optionale Map-Datei wurde nicht geladen.");

    save_text(manifest_path, "version = 2\nformat = raw\ninput = program.bin\nbase_address = 0\n");
    auto error = require_failure(
        [&] { static_cast<void>(parse_project_manifest(manifest_path)); },
        "Eine unbekannte Manifestversion wurde akzeptiert."
    );
    require(error.find(manifest_path.string()) != std::string::npos && error.find("Zeile 1") != std::string::npos, "Versionsfehler nennt Datei und Zeile nicht.");

    save_text(manifest_path, "version = 1\nformat = raw\ninput = program.bin\n");
    error = require_failure(
        [&] { static_cast<void>(parse_project_manifest(manifest_path)); },
        "Ein Raw-Manifest ohne Basisadresse wurde akzeptiert."
    );
    require(error.find("base_address") != std::string::npos, "Fehlende Raw-Basisadresse ist nicht diagnostisch.");

    save_text(manifest_path, "version = 1\nformat = raw\ninput = program.bin\nbase_address = 0\nmystery = value\n");
    error = require_failure(
        [&] { static_cast<void>(parse_project_manifest(manifest_path)); },
        "Ein unbekanntes Manifestfeld wurde akzeptiert."
    );
    require(error.find("Zeile 5") != std::string::npos && error.find("mystery") != std::string::npos, "Unbekanntes Feld ist nicht diagnostisch.");

    std::filesystem::remove_all(directory);
    std::cout << "KR-1606 Projektmanifest Version 1 erfolgreich.\n";
    return EXIT_SUCCESS;
}
