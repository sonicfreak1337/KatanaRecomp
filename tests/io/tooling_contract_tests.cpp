#include "katana/analysis/symbol_names.hpp"
#include "katana/cli/exit_code.hpp"
#include "katana/codegen/source_map.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-phase8-tooling-fixture";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "a");
        std::filesystem::create_directories(root / "b");
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

katana::io::BuildProvenance provenance(const std::vector<katana::io::InputProvenance>& inputs) {
    const auto hash = katana::io::sha256_bytes("manifest");
    return {
        "0.34.0-dev", 2u, hash, katana::io::sha256_bytes("directives"), 2u, 8u, "cpp", 1u, inputs};
}

} // namespace

int main() {
    using katana::cli::exit_code_name;
    using katana::cli::exit_status;
    using katana::cli::ExitCode;

    const std::array exit_codes{ExitCode::Success,
                                ExitCode::Usage,
                                ExitCode::InvalidInput,
                                ExitCode::InputOutput,
                                ExitCode::ProcessingFailure,
                                ExitCode::CodeGenerationFailure,
                                ExitCode::BuildFailure,
                                ExitCode::InternalError};
    const std::array expected_status{0, 2, 3, 4, 5, 6, 7, 70};
    const std::array expected_names{"success",
                                    "usage",
                                    "invalid-input",
                                    "input-output",
                                    "processing-failure",
                                    "codegen-failure",
                                    "build-failure",
                                    "internal-error"};
    for (std::size_t index = 0u; index < exit_codes.size(); ++index) {
        require(exit_status(exit_codes[index]) == expected_status[index] &&
                    exit_code_name(exit_codes[index]) == expected_names[index],
                "Oeffentlicher CLI-Exitcode oder Maschinenname ist nicht stabil.");
    }
    const katana::cli::Error cli_error(ExitCode::CodeGenerationFailure, "codegen");
    require(cli_error.code() == ExitCode::CodeGenerationFailure,
            "CLI-Fehler verliert seine Klasse.");
    require_failure<std::invalid_argument>(
        [] { static_cast<void>(katana::cli::Error(ExitCode::Success, "invalid")); },
        "Ein CLI-Fehler darf Erfolg nicht als Fehlerklasse tragen.");

    const auto escaped = katana::io::quote_json("quote\" slash\\\n\t\x01");
    require(escaped == "\"quote\\\" slash\\\\\\n\\t\\u0001\"",
            "Zentraler JSON-Escaper behandelt Anfuehrungszeichen oder Kontrollzeichen falsch.");

    Fixture fixture;
    write_bytes(fixture.root / "a" / "input.bin", "synthetic-input");
    write_bytes(fixture.root / "b" / "input.bin", "synthetic-input");
    const auto input_a =
        katana::io::capture_input_provenance("main-image", fixture.root / "a" / "input.bin");
    const auto input_b =
        katana::io::capture_input_provenance("main-image", fixture.root / "b" / "input.bin");
    const auto first = provenance({input_a});
    const auto second = provenance({input_b});
    require(input_a.local_path != input_b.local_path &&
                katana::io::make_portable_build_identity(first) ==
                    katana::io::make_portable_build_identity(second),
            "Gleiche Inhalte an verschiedenen lokalen Pfaden erhalten verschiedene portable IDs.");
    const auto portable_json = katana::io::format_build_provenance_json(first);
    require(portable_json.find(fixture.root.string()) == std::string::npos &&
                portable_json.find("synthetic-input") == std::string::npos &&
                portable_json.find("\"schema\":\"katana-build-provenance\"") != std::string::npos,
            "Buildprovenienz enthaelt lokale Pfade/Rohdaten oder keinen gemeinsamen Berichtkopf.");
    write_bytes(fixture.root / "b" / "input.bin", "synthetic-inpuu");
    const auto changed = provenance(
        {katana::io::capture_input_provenance("main-image", fixture.root / "b" / "input.bin")});
    require(katana::io::make_portable_build_identity(first) !=
                katana::io::make_portable_build_identity(changed),
            "Ein geaendertes Eingabebyte invalidiert die portable Buildidentitaet nicht.");

    using namespace katana::io;
    const std::vector<ImageSymbol> symbols{
        {"local", 0x1000u, 16u, SymbolKind::Object, SymbolBinding::Local},
        {"winner\"name", 0x1000u, 8u, SymbolKind::Function, SymbolBinding::Global},
        {"zero", 0x2000u, 0u, SymbolKind::Function, SymbolBinding::Global}};
    const katana::analysis::SymbolNameIndex names(symbols);
    const auto exact = names.resolve(0x1000u);
    const auto offset = names.resolve(0x1004u);
    require(exact.has_value() && exact->name == "winner\"name" && exact->exact &&
                offset.has_value() && offset->name == "winner\"name" &&
                katana::analysis::format_symbolic_address(*offset) == "winner\"name+0x4",
            "Symbolprioritaet, exakter Treffer oder begrenzte Offsetdarstellung ist falsch.");
    require(
        names.resolve(0x2000u).has_value() && !names.resolve(0x2001u).has_value() &&
            !names.resolve(0x3000u).has_value(),
        "Groessenloses oder unbekanntes Symbol wird ueber seine exakte Adresse hinaus verwendet.");

    ExecutableImage image;
    image.add_segment({"text",
                       0x1000u,
                       32u,
                       8u,
                       SegmentKind::Code,
                       {true, false, true},
                       std::vector<std::uint8_t>(8u, 0u)});
    const std::vector<katana::codegen::ProjectArtifact> units{
        {"code/b.cpp", "// katana-guest 0x00001002\n// katana-guest 0x00001000\n"},
        {"code/a.cpp", "line\n// katana-guest 0x00001000\n"}};
    const auto locations = katana::codegen::build_address_source_map(image, units);
    const auto same_address = katana::codegen::find_source_locations(locations, 0x1000u);
    require(locations.size() == 3u && same_address.size() == 2u &&
                same_address[0].generated_path == "code/a.cpp" &&
                same_address[0].input_byte_offset == 32u,
            "Source Map verliert Mehrfachpositionen, Sortierung oder Eingabeoffset.");
    const auto source_json = katana::codegen::serialize_address_source_map(locations);
    require(source_json.find("\"source_map_version\":1") != std::string::npos &&
                source_json.find("0x00001002") != std::string::npos,
            "Source-Map-JSON besitzt keinen versionierten Marker-Roundtrip.");
    auto unsorted = locations;
    std::swap(unsorted.front(), unsorted.back());
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(katana::codegen::serialize_address_source_map(unsorted)); },
        "Unsortierte manuelle Source Map wurde akzeptiert.");
    auto unportable = locations;
    unportable.front().generated_path = "../host.cpp";
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(katana::codegen::serialize_address_source_map(unportable)); },
        "Unportabler Source-Map-Pfad wurde akzeptiert.");

    std::cout << "KR-3502/3503/3506 und KR-3601/3602 Werkzeugvertraege erfolgreich.\n";
    return EXIT_SUCCESS;
}
