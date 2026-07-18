#include "katana/io/raw_binary_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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

} // namespace

int main() {
    using namespace katana::io;
    const auto fixture =
        std::filesystem::path(KATANA_SOURCE_DIR) / "tests" / "fixtures" / "codegen_execution.bin";

    RawBinaryLoadOptions options;
    options.base_address = 0x8C010000u;
    options.segment_name = ".text.raw";
    options.entry_point = 0x8C010004u;
    const auto image = load_raw_binary(fixture, options);

    require(image.source_path() == fixture, "Der Raw-Loader verlor den Quellpfad.");
    require(image.segments().size() == 1u, "Raw-Binary erzeugt nicht genau ein Segment.");
    const auto& segment = image.segments()[0];
    require(segment.name == ".text.raw", "Der konfigurierte Segmentname ging verloren.");
    require(segment.virtual_address == 0x8C010000u, "Die Raw-Basisadresse ist falsch.");
    require(segment.file_offset == 0u, "Ein Raw-Segment beginnt nicht bei Dateioffset null.");
    require(segment.memory_size == segment.bytes.size() && !segment.bytes.empty(),
            "Raw-Datei- und Speichergroesse stimmen nicht ueberein.");
    require(segment.kind == SegmentKind::Code, "Raw-Code wurde falsch klassifiziert.");
    require(segment.source_kind == ImageSourceKind::RawBinary &&
                segment.load_phase == ImageLoadPhase::Initial &&
                segment.local_source_name == fixture.filename().string(),
            "Raw-Loader verliert Quelle oder Ladephase.");
    require(segment.permissions.readable && segment.permissions.executable &&
                !segment.permissions.writable,
            "Raw-Standardberechtigungen sind falsch.");
    require(image.entry_points().size() == 1u && image.entry_points()[0] == 0x8C010004u,
            "Der konfigurierte Einstiegspunkt fehlt.");

    const auto missing =
        std::filesystem::path(KATANA_SOURCE_DIR) / "tests" / "fixtures" / "does-not-exist.bin";
    const auto missing_error = require_failure([&] { static_cast<void>(load_raw_binary(missing)); },
                                               "Eine fehlende Raw-Datei wurde akzeptiert.");
    require(missing_error.find(missing.string()) != std::string::npos,
            "Der Loaderfehler nennt die Quelldatei nicht.");

    const auto empty = std::filesystem::current_path() / "katana-empty-raw-fixture.bin";
    {
        std::ofstream output(empty, std::ios::binary);
    }
    const auto empty_error = require_failure([&] { static_cast<void>(load_raw_binary(empty)); },
                                             "Eine leere Raw-Datei wurde akzeptiert.");
    std::filesystem::remove(empty);
    require(empty_error.find("Offset 0") != std::string::npos,
            "Der Leerdateifehler nennt den Dateioffset nicht.");

    RawBinaryLoadOptions wrap_options;
    wrap_options.base_address = 0xFFFFFFFFu;
    const auto wrap_error =
        require_failure([&] { static_cast<void>(load_raw_binary(fixture, wrap_options)); },
                        "Ein Raw-Image ausserhalb des Adressraums wurde akzeptiert.");
    require(wrap_error.find("32-Bit-Adressraum") != std::string::npos,
            "Der Adressraumfehler nennt seine Ursache nicht.");

    std::cout << "KR-1602 Raw-Binary-Loader erfolgreich.\n";
    return EXIT_SUCCESS;
}
