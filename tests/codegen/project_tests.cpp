#include "katana/codegen/project.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

#ifndef KATANA_SOURCE_DIR
#error "KATANA_SOURCE_DIR muss fuer den frischen Ninja-Build gesetzt sein."
#endif
#ifndef KATANA_NINJA_EXECUTABLE
#error "KATANA_NINJA_EXECUTABLE muss fuer den frischen Ninja-Build gesetzt sein."
#endif

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

struct Fixture {
    std::filesystem::path root = std::filesystem::current_path() / "katana-project-writer-fixture";
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::string shell_quote(const std::filesystem::path& path) {
    const auto text = path.string();
#ifdef _WIN32
    return '"' + text + '"';
#else
    std::string quoted = "'";
    for (const auto character : text)
        character == '\'' ? quoted += "'\\''" : quoted += character;
    return quoted + "'";
#endif
}

} // namespace

int main() {
    using namespace katana::codegen;
    Fixture fixture;
    CodegenCache cache(fixture.root / "cache");
    const auto key = make_codegen_cache_key(
        {"input", "ir", "opt", "cpp", 1u, 8u, "manifest", "overrides", 2u, 1u, "0.34.0-dev"});
    const std::vector<ProjectArtifact> artifacts = {
        {"code/unit-00001.cpp",
         "#include <katana/build_contract.hpp>\n"
         "int unit_1() { return katana::build_contract::runtime_abi_version; }\n"},
        {"include/constants.hpp", "#pragma once\ninline constexpr int value = 7;\n"},
        {"code/unit-00000.cpp",
         "#include <katana/build_contract.hpp>\n"
         "int unit_0() { return katana::build_contract::block_abi_version; }\n"}};
    const auto serial =
        write_codegen_project(fixture.root / "serial", artifacts, {1u, &cache, key});
    const auto parallel =
        write_codegen_project(fixture.root / "parallel", artifacts, {4u, &cache, key});
    const auto serial_snapshot = snapshot(fixture.root / "serial");
    const auto parallel_snapshot = snapshot(fixture.root / "parallel");
    require(serial.cache_hits == 0u && serial.cache_misses == artifacts.size() &&
                parallel.cache_hits == artifacts.size() && parallel.cache_misses == 0u &&
                serial.written_files == parallel.written_files &&
                serial_snapshot == parallel_snapshot,
            "Serielle und parallele Ausgabe oder Cachetreffer sind nicht deterministisch.");
    require(serial_snapshot.at("CMakeLists.txt").find("code/unit-00000.cpp") != std::string::npos &&
                serial_snapshot.at("build.ninja").find("libkatana_generated.a") !=
                    std::string::npos &&
                serial_snapshot.at("compile_commands.json").find("\"directory\":\".\"") !=
                    std::string::npos,
            "CMake-, Ninja- oder Compile-Commands-Integration fehlt.");

#ifdef _WIN32
    require(_putenv_s("KATANA_RUNTIME_ROOT", KATANA_SOURCE_DIR) == 0,
            "KATANA_RUNTIME_ROOT konnte fuer Ninja nicht gesetzt werden.");
#else
    require(setenv("KATANA_RUNTIME_ROOT", KATANA_SOURCE_DIR, 1) == 0,
            "KATANA_RUNTIME_ROOT konnte fuer Ninja nicht gesetzt werden.");
#endif
    const auto ninja_command =
        shell_quote(KATANA_NINJA_EXECUTABLE) + " -C " + shell_quote(fixture.root / "serial");
    require(std::system(ninja_command.c_str()) == 0 &&
                std::filesystem::is_regular_file(fixture.root / "serial" / "libkatana_generated.a"),
            "Ein frisches erzeugtes Ninja-Projekt baut Runtime-Includes, Buildvertrag oder "
            "Archiv nicht eigenstaendig.");

    const auto reused = fixture.root / "reused";
    static_cast<void>(write_codegen_project(reused,
                                            {{"code/unit-00000.cpp", "first-0\n"},
                                             {"code/unit-00001.cpp", "first-1\n"},
                                             {"metadata/blocks.json", "blocks\n"},
                                             {"symbols/names.json", "symbols\n"}}));
    {
        std::ofstream user_file(reused / "user-notes.txt", std::ios::binary);
        user_file << "keep me\n";
    }
    const auto shrunk = write_codegen_project(reused, {{"code/renamed-unit.cpp", "second\n"}});
    const auto shrunk_snapshot = snapshot(reused);
    require(!shrunk_snapshot.contains("code/unit-00000.cpp") &&
                !shrunk_snapshot.contains("code/unit-00001.cpp") &&
                !shrunk_snapshot.contains("metadata/blocks.json") &&
                !shrunk_snapshot.contains("symbols/names.json") &&
                shrunk_snapshot.contains("code/renamed-unit.cpp") &&
                !std::filesystem::exists(reused / "metadata") &&
                !std::filesystem::exists(reused / "symbols") &&
                shrunk_snapshot.at("user-notes.txt") == "keep me\n" &&
                shrunk.removed_files.size() == 4u,
            "Zweiter Lauf entfernt alte Units/Metadaten/Symbole nicht selektiv oder loescht "
            "Nutzerdateien.");

    const auto failing = fixture.root / "cleanup-failure";
    static_cast<void>(write_codegen_project(failing, {{"code/stale.cpp", "stale\n"}}));
    std::filesystem::remove(failing / "code/stale.cpp");
    std::filesystem::create_directories(failing / "code/stale.cpp/child");
    bool cleanup_reported = false;
    try {
        static_cast<void>(write_codegen_project(failing, {{"code/current.cpp", "current\n"}}));
    } catch (const std::runtime_error& error) {
        cleanup_reported = std::string(error.what()).find("code/stale.cpp") != std::string::npos;
    }
    require(cleanup_reported,
            "Fehlgeschlagene Bereinigung wird nicht mit dem betroffenen Pfad gemeldet.");

    const auto outside = fixture.root / "outside-symlink-target";
    std::filesystem::create_directories(outside);
    const auto symlink_write = fixture.root / "symlink-write";
    std::filesystem::create_directories(symlink_write);
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(outside, symlink_write / "code", symlink_error);
    if (!symlink_error) {
        bool write_rejected = false;
        try {
            static_cast<void>(
                write_codegen_project(symlink_write, {{"code/unit.cpp", "outside write\n"}}));
        } catch (const std::runtime_error& error) {
            write_rejected =
                std::string(error.what()).find("symbolischen Link") != std::string::npos;
        }
        require(write_rejected && !std::filesystem::exists(outside / "unit.cpp"),
                "Artefaktschreiben folgt einem Symlink aus dem Ausgabeziel.");

        const auto symlink_cleanup = fixture.root / "symlink-cleanup";
        static_cast<void>(
            write_codegen_project(symlink_cleanup, {{"code/stale.cpp", "owned stale\n"}}));
        const auto outside_cleanup = fixture.root / "outside-cleanup-target";
        std::filesystem::create_directories(outside_cleanup);
        {
            std::ofstream external(outside_cleanup / "stale.cpp", std::ios::binary);
            external << "must survive\n";
        }
        std::filesystem::remove_all(symlink_cleanup / "code");
        std::filesystem::create_directory_symlink(outside_cleanup, symlink_cleanup / "code");
        bool cleanup_symlink_rejected = false;
        try {
            static_cast<void>(
                write_codegen_project(symlink_cleanup, {{"safe/current.cpp", "current\n"}}));
        } catch (const std::runtime_error& error) {
            cleanup_symlink_rejected =
                std::string(error.what()).find("symbolischen Link") != std::string::npos;
        }
        std::ifstream external(outside_cleanup / "stale.cpp", std::ios::binary);
        std::ostringstream external_content;
        external_content << external.rdbuf();
        require(cleanup_symlink_rejected && external_content.str() == "must survive\n",
                "Artefaktbereinigung folgt einem Symlink aus dem Ausgabeziel.");
    } else {
        std::cout << "Symlink-Regression lokal nicht verfuegbar: " << symlink_error.message()
                  << '\n';
    }

    std::cout << "KR-3304 parallele Ausgabe und Buildintegration erfolgreich.\n";
    return 0;
}
