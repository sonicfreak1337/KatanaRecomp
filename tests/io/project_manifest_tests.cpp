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

} // namespace

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
    save_text(manifest_path,
              "# KatanaRecomp project manifest\n"
              "version = 1\n"
              "format = raw\n"
              "input = program.bin\n"
              "base_address = 0x8C010000\n"
              "entry_point = 8C010000\n"
              "segment_name = .text\n"
              "segment_kind = code\n"
              "permissions = r-x\n"
              "map = program.map\n");

    const auto manifest = parse_project_manifest(manifest_path);
    require(manifest.version == 1u && manifest.format == ProjectInputFormat::RawBinary,
            "Manifestkopf ist falsch.");
    require(manifest.input_path == binary && manifest.map_path == map,
            "Relative Manifestpfade wurden falsch aufgeloest.");
    require(manifest.base_address == 0x8C010000u && manifest.entry_point == 0x8C010000u,
            "Adresslayout ist falsch.");
    require(std::string(project_input_format_name(manifest.format)) == "raw",
            "Formatname ist instabil.");

    const auto image = load_project_manifest(manifest_path);
    require(image.source_path() == binary, "Manifestloader verlor die eigentliche Eingabedatei.");
    require(image.segments().size() == 1u && image.segments()[0].name == ".text",
            "Raw-Segmentlayout wurde nicht angewendet.");
    require(image.entry_points().size() == 1u && image.entry_points()[0] == 0x8C010000u,
            "Manifest-Einstiegspunkt fehlt.");
    require(image.find_symbol("start") != nullptr, "Optionale Map-Datei wurde nicht geladen.");

    save_text(manifest_path,
              "schema = katana-project\n"
              "version = 2\n"
              "project.name = phase8\n"
              "input.format = raw\n"
              "input.path = program.bin\n"
              "image.base_address = 0x8C010000\n"
              "image.entry_point = 0x8C010000\n"
              "image.dynamic_bios_vectors = 0x8C0000B0,0x8C0000B4\n"
              "segment.name = phase8\n"
              "segment.kind = code\n"
              "segment.permissions = r-x\n"
              "execution.firmware = direct\n"
              "execution.fallback = abort\n"
              "execution.scheduler = deterministic\n"
              "execution.mmu = disabled\n"
              "execution.fastpath = conservative\n");
    const auto version_two = parse_project_manifest(manifest_path);
    require(version_two.version == 2u && version_two.schema == project_manifest_schema_name &&
                version_two.project_name == "phase8" &&
                version_two.firmware_mode == ProjectFirmwareMode::Direct &&
                version_two.fallback_policy == ProjectFallbackPolicy::Abort &&
                version_two.dynamic_bios_vectors ==
                    std::vector<std::uint32_t>{0x8C0000B0u, 0x8C0000B4u},
            "Manifestversion 2 oder ihr sicherer Ausfuehrungsvertrag wurde falsch gelesen.");
    const auto loaded_version_two = load_project(manifest_path);
    require(loaded_version_two.image.find_symbol("bios-vector") == nullptr,
            "Dynamischer BIOS-Vektor wurde als statisches ROM-Symbol serialisiert.");
    require(loaded_version_two.execution_profile.fallback_policy == ProjectFallbackPolicy::Abort &&
                loaded_version_two.execution_profile.scheduler_profile == "deterministic" &&
                loaded_version_two.execution_profile.mmu_profile == ProjectMmuProfile::Disabled &&
                loaded_version_two.execution_profile.fastpath_profile ==
                    ProjectFastpathProfile::Conservative,
            "Geladenes Projekt verlor sein validiertes Ausfuehrungsprofil.");

    save_text(manifest_path, "schema = katana-project\nversion = 3\n");
    auto error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                                 "Eine unbekannte Manifestversion wurde akzeptiert.");
    require(error.find(manifest_path.string()) != std::string::npos &&
                error.find("Zeile 2") != std::string::npos,
            "Versionsfehler nennt Datei und Zeile nicht.");

    save_text(manifest_path,
              "schema = katana-project\nversion = 2\nproject.name = mixed\n"
              "input.format = raw\ninput.path = program.bin\nbase_address = 0x8C010000\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Ein namespaced v2-Manifest mit v1-Feld wurde akzeptiert.");
    require(error.find("base_address") != std::string::npos,
            "Versionsfremdes v1-Feld wird nicht benannt.");

    save_text(manifest_path,
              "schema = wrong-project\nversion = 2\nproject.name = mixed\n"
              "input.format = raw\ninput.path = program.bin\nimage.base_address = 0x8C010000\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Ein unbekannter Manifest-Schema-Identifier wurde akzeptiert.");
    require(error.find("schema") != std::string::npos, "Schemafehler benennt das Feld nicht.");

    save_text(manifest_path,
              "schema = katana-project\nversion = 2\nproject.name = lle\n"
              "input.format = raw\ninput.path = program.bin\nimage.base_address = 0x8C010000\n"
              "execution.firmware = lle\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Unvollstaendiges LLE-Profil wurde akzeptiert.");
    require(error.find("LLE braucht BIOS") != std::string::npos,
            "LLE-Pflichtsegmente und -Faehigkeiten werden nicht diagnostiziert.");

    save_text(manifest_path,
              "schema = katana-project\nversion = 2\nproject.name = profile\n"
              "input.format = raw\ninput.path = program.bin\nimage.base_address = 0x8C010000\n"
              "execution.scheduler = realtime\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Unbekanntes Schedulerprofil wurde akzeptiert.");
    require(error.find("deterministic") != std::string::npos,
            "Unbekanntes Schedulerprofil ist nicht diagnostisch.");

    require_failure(
        [&] {
            const std::vector<ProjectAliasGroup> cycle{{0x1000u, 0x2000u, 0x100u},
                                                       {0x2000u, 0x3000u, 0x100u}};
            const std::vector<ProjectAddressRange> canonical{{0x2000u, 0x2000u}};
            require_valid_project_alias_groups(cycle, canonical);
        },
        "Aliasziel auf eine weitere virtuelle Aliasgruppe wurde akzeptiert.");

    const std::vector<ProjectAddressRange> shared_canonical{{0x0C000000u, 0x2000u}};
    require_failure(
        [&] {
            const std::vector<ProjectAliasGroup> self{{0x0C000000u, 0x0C000000u, 0x100u}};
            require_valid_project_alias_groups(self, shared_canonical);
        },
        "Selbstalias wurde akzeptiert.");
    require_failure(
        [&] {
            const std::vector<ProjectAliasGroup> overlap{{0x8C000000u, 0x0C000000u, 0x1000u},
                                                         {0xAC000000u, 0x0C000800u, 0x1000u}};
            require_valid_project_alias_groups(overlap, shared_canonical);
        },
        "Teilweise ueberlappende physische Aliasziele wurden akzeptiert.");
    const std::vector<ProjectAliasGroup> shared_target{{0x8C000000u, 0x0C000000u, 0x1000u},
                                                       {0xAC000000u, 0x0C000000u, 0x1000u}};
    require_valid_project_alias_groups(shared_target, shared_canonical);

    save_text(manifest_path,
              "schema = katana-project\nversion = 2\nproject.name = alias-error\n"
              "input.format = raw\ninput.path = program.bin\nimage.base_address = 0x8C010000\n"
              "memory.canonical_ranges = 0x0C000000:0x2000\n"
              "memory.alias_groups = 0x0C000000:0x0C000000:0x100\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Manifest-Selbstalias wurde akzeptiert.");
    require(error.find(manifest_path.string()) != std::string::npos &&
                error.find("Zeile 8") != std::string::npos,
            "Aliasdiagnose nennt Datei und Zeile nicht.");

    save_text(manifest_path,
              "\xEF\xBB\xBFschema = katana-project\n"
              "version = 2\nproject.name = bom\ninput.format = raw\n"
              "input.path = program.bin\nimage.base_address = 0x8C010000\n");
    require(parse_project_manifest(manifest_path).project_name == "bom",
            "UTF-8-BOM eines GUI-gespeicherten Manifests wurde nicht akzeptiert.");

    save_text(manifest_path, "version = 1\nformat = raw\ninput = program.bin\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Ein Raw-Manifest ohne Basisadresse wurde akzeptiert.");
    require(error.find("base_address") != std::string::npos,
            "Fehlende Raw-Basisadresse ist nicht diagnostisch.");

    save_text(
        manifest_path,
        "version = 1\nformat = raw\ninput = program.bin\nbase_address = 0\nmystery = value\n");
    error = require_failure([&] { static_cast<void>(parse_project_manifest(manifest_path)); },
                            "Ein unbekanntes Manifestfeld wurde akzeptiert.");
    require(error.find("Zeile 5") != std::string::npos &&
                error.find("mystery") != std::string::npos,
            "Unbekanntes Feld ist nicht diagnostisch.");

    std::filesystem::remove_all(directory);
    std::cout << "KR-3501 Projektmanifest Version 1 und 2 erfolgreich.\n";
    return EXIT_SUCCESS;
}
