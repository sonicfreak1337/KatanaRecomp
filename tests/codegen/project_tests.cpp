#include "katana/codegen/project.hpp"
#include "katana/io/input_provenance.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#endif

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

#ifndef _WIN32
std::string shell_quote(const std::filesystem::path& path) {
    const auto text = path.string();
    std::string quoted = "'";
    for (const auto character : text)
        character == '\'' ? quoted += "'\\''" : quoted += character;
    return quoted + "'";
}
#endif

int run_ninja(const std::filesystem::path& root) {
    const auto executable = std::filesystem::path(KATANA_NINJA_EXECUTABLE);
#ifdef _WIN32
    return static_cast<int>(_wspawnl(_P_WAIT,
                                     executable.c_str(),
                                     executable.filename().c_str(),
                                     L"-C",
                                     root.c_str(),
                                     static_cast<const wchar_t*>(nullptr)));
#else
    const auto command = shell_quote(executable) + " -C " + shell_quote(root);
    return std::system(command.c_str());
#endif
}

} // namespace

int main() {
    using namespace katana::codegen;
    Fixture fixture;
    CodegenCache cache(fixture.root / "cache");
    const auto key = make_codegen_cache_key(
        {"input",
         "ir",
         "opt",
         "cpp",
         1u,
         8u,
         "manifest",
         "overrides",
         2u,
         1u,
         "0.34.0-dev",
         "project-exporter-a"});
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
    auto serial_artifacts = serial_snapshot;
    auto parallel_artifacts = parallel_snapshot;
    serial_artifacts.erase(".katana-generated-artifacts");
    parallel_artifacts.erase(".katana-generated-artifacts");
    require(serial.cache_hits == 0u && serial.cache_misses == artifacts.size() &&
                parallel.cache_hits == artifacts.size() && parallel.cache_misses == 0u &&
                serial.written_files == parallel.written_files &&
                serial_artifacts == parallel_artifacts,
            "Serielle und parallele Ausgabe oder Cachetreffer sind nicht deterministisch.");
    require(serial_snapshot.at(".katana-generated-artifacts").starts_with(
                "katana-codegen-artifacts-v2\ngeneration\tsha256:"),
            "Der Artefaktmanifest v2 fehlt die gebundene Generation.");

    const auto manifest_reuse_root = fixture.root / "manifest-cache-reuse";
    const auto manifest_cache_root = fixture.root / "manifest-cache";
    CodegenCache manifest_cache(manifest_cache_root);
    static_cast<void>(write_codegen_project(
        manifest_reuse_root, artifacts, {2u, &manifest_cache, key}));
    {
        std::error_code remove_error;
        std::filesystem::remove_all(manifest_cache_root, remove_error);
        require(!remove_error && !std::filesystem::exists(manifest_cache_root),
                "Das Manifest-Reuse-Fixture konnte den optionalen Cache nicht entfernen.");
    }
    const auto manifest_reuse = write_codegen_project(
        manifest_reuse_root, artifacts, {2u, &manifest_cache, key});
    require(!std::filesystem::exists(manifest_cache_root) &&
                manifest_reuse.cache_hits == 0u &&
                manifest_reuse.cache_misses == artifacts.size(),
            "Ein exakt gebundenes v2-Manifest liest oder repariert den optionalen Cache.");
    {
        std::ofstream mutate_same_size(
            manifest_reuse_root / "code/unit-00000.cpp",
            std::ios::binary | std::ios::trunc);
        auto changed = artifacts[2].content;
        changed[0] = changed[0] == '#' ? '!' : '#';
        mutate_same_size << changed;
    }
    static_cast<void>(write_codegen_project(
        manifest_reuse_root, artifacts, {2u, &manifest_cache, key}));
    require(std::filesystem::exists(manifest_cache_root) &&
                snapshot(manifest_reuse_root).at("code/unit-00000.cpp") ==
                    artifacts[2].content,
            "Eine mutierte Manifest-Bindung faellt nicht in Cache-/Atomic-Recovery zurueck.");
    require(serial_snapshot.at("CMakeLists.txt").find("code/unit-00000.cpp") != std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("/bigobj") != std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_EXCEPTION_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("^code/unit-v[^/]+\\\\.cpp$") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("code/native-port-dispatch.cpp") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_DECLARATIVE_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_COLD_AOT_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_CENTRAL_DISPATCH_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("${KATANA_GENERATED_CENTRAL_DISPATCH_SOURCES}") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("NOT KATANA_GENERATED_SOURCE MATCHES \"^code/unit-v8[Cc]\"") ==
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_HOT_SOURCE_INDEX EQUAL -1") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("^code/native-port-(dispatch|loaded-aot|runtime-image)-"
                              "shard-[0-9]+\\\\.cpp$") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_GENERATED_NON_LTO_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("PROPERTY COMPILE_OPTIONS /clang:-fno-lto") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("PROPERTY COMPILE_OPTIONS /EHsc") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("KATANA_AOT_COMPILE_JOBS") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("JOB_POOL_COMPILE") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("/O1 /Ob0") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_AOT_HOT_SOURCES") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("/O2 /Ob2") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("SKIP_PRECOMPILE_HEADERS") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_PERSISTENT_COMPILER_CACHE_ACTIVE") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("KATANA_PERSISTENT_COMPILER_CACHE_USE_PCH") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("MSVC_DEBUG_INFORMATION_FORMAT") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("/MP${KATANA_AOT_COMPILE_JOBS}") !=
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt").find("PRIVATE /bigobj /FS") ==
                    std::string::npos &&
                serial_snapshot.at("CMakeLists.txt")
                        .find("CMAKE_GENERATOR MATCHES \"^Visual Studio\"") !=
                    std::string::npos &&
                serial_snapshot.at("build.ninja").find("libkatana_generated.a") !=
                    std::string::npos &&
                serial_snapshot.at("compile_commands.json").find("\"directory\":\".\"") !=
                    std::string::npos,
            "CMake-, Ninja- oder Compile-Commands-Integration fehlt.");

    const auto refresh_root = fixture.root / "refresh";
    static_cast<void>(write_codegen_project(
        refresh_root,
        {{"code/native-port-dispatch.cpp", "provider-before\n"},
         {"code/aot-partition.cpp", "aot-unchanged\n"},
         {"tools/native-port-link-audit.cpp", "marker-before\n"}}));
    const auto refresh_before = snapshot(refresh_root);
    const auto refreshed = rewrite_codegen_project_artifacts(
        refresh_root,
        {{"code/native-port-dispatch.cpp",
          "sha256:" + katana::io::sha256_bytes("provider-before\n"),
          "provider-after\n"},
         {"tools/native-port-link-audit.cpp",
          "sha256:" + katana::io::sha256_bytes("marker-before\n"),
          "marker-after\n"}});
    const auto refresh_after = snapshot(refresh_root);
    require(
        refresh_after.at("code/native-port-dispatch.cpp") ==
                "provider-after\n" &&
            refresh_after.at("tools/native-port-link-audit.cpp") ==
                "marker-after\n" &&
            refresh_after.at("code/aot-partition.cpp") ==
                refresh_before.at("code/aot-partition.cpp") &&
            refresh_after.at("CMakeLists.txt") ==
                refresh_before.at("CMakeLists.txt") &&
            refresh_after.at("build.ninja") ==
                refresh_before.at("build.ninja") &&
            refresh_after.at("compile_commands.json") ==
                refresh_before.at("compile_commands.json") &&
            refresh_after.at(".katana-generated-artifacts") !=
                refresh_before.at(".katana-generated-artifacts") &&
            refreshed.written_files.size() == 3u &&
            refreshed.removed_files.empty(),
        "Der gebundene Consumer-Refresh veraendert AOT/Buildgraph oder "
        "publiziert sein Manifest nicht.");
    bool stale_refresh_rejected = false;
    try {
        static_cast<void>(rewrite_codegen_project_artifacts(
            refresh_root,
            {{"code/native-port-dispatch.cpp",
              "sha256:" + katana::io::sha256_bytes("provider-before\n"),
              "provider-stale\n"}}));
    } catch (const std::runtime_error&) {
        stale_refresh_rejected = true;
    }
    require(stale_refresh_rejected &&
                snapshot(refresh_root).at("code/native-port-dispatch.cpp") ==
                    "provider-after\n",
            "Der Consumer-Refresh akzeptiert eine stale Quellbindung.");
    {
        std::ofstream mutate_unselected(
            refresh_root / "code/aot-partition.cpp",
            std::ios::binary | std::ios::trunc);
        mutate_unselected << "aot-tampered!\n";
    }
    bool generation_refresh_rejected = false;
    try {
        static_cast<void>(rewrite_codegen_project_artifacts(
            refresh_root,
            {{"code/native-port-dispatch.cpp",
              "sha256:" + katana::io::sha256_bytes("provider-after\n"),
              "provider-next\n"}}));
    } catch (const std::runtime_error&) {
        generation_refresh_rejected = true;
    }
    require(generation_refresh_rejected &&
                snapshot(refresh_root).at("code/native-port-dispatch.cpp") ==
                    "provider-after\n",
            "Der Consumer-Refresh akzeptiert eine fremd veraenderte AOT-Generation.");

    const auto same_size_tamper_root = fixture.root / "same-size-tamper";
    static_cast<void>(write_codegen_project(
        same_size_tamper_root,
        {{"code/native-port-dispatch.cpp", "dispatch-before\n"},
         {"code/aot-partition.cpp", "aot-unchanged\n"}}));
    {
        std::ofstream mutate_same_size(
            same_size_tamper_root / "code/aot-partition.cpp",
            std::ios::binary | std::ios::trunc);
        mutate_same_size << "aot-corrupted\n";
    }
    bool same_size_generation_rejected = false;
    try {
        static_cast<void>(rewrite_codegen_project_artifacts(
            same_size_tamper_root,
            {{"code/native-port-dispatch.cpp",
              "sha256:" + katana::io::sha256_bytes("dispatch-before\n"),
              "dispatch-after\n"}}));
    } catch (const std::runtime_error&) {
        same_size_generation_rejected = true;
    }
    require(
        same_size_generation_rejected,
        "Der Consumer-Refresh akzeptiert eine gleich grosse AOT-Manipulation.");

    const auto manifest_mismatch_root = fixture.root / "manifest-mismatch";
    static_cast<void>(write_codegen_project(
        manifest_mismatch_root,
        {{"code/native-port-dispatch.cpp", "manifest-before\n"},
         {"code/aot-partition.cpp", "aot-stable\n"}}));
    {
        const auto manifest_path =
            manifest_mismatch_root / ".katana-generated-artifacts";
        std::ifstream input(manifest_path, std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        auto document = content.str();
        const auto marker = document.find("katana-codegen-artifacts-v2");
        require(marker != std::string::npos,
                "Das Manifest-Mismatch-Fixture besitzt keinen Header.");
        document[marker] = document[marker] == 'k' ? 'l' : 'k';
        std::ofstream output(manifest_path,
                             std::ios::binary | std::ios::trunc);
        output.write(document.data(),
                     static_cast<std::streamsize>(document.size()));
    }
    bool manifest_mismatch_rejected = false;
    try {
        static_cast<void>(rewrite_codegen_project_artifacts(
            manifest_mismatch_root,
            {{"code/native-port-dispatch.cpp",
              "sha256:" + katana::io::sha256_bytes("manifest-before\n"),
              "manifest-after\n"}}));
    } catch (const std::runtime_error&) {
        manifest_mismatch_rejected = true;
    }
    require(manifest_mismatch_rejected,
            "Der Consumer-Refresh akzeptiert ein inkonsistentes Manifest.");

    const auto structural_mismatch_root = fixture.root / "structural-mismatch";
    static_cast<void>(write_codegen_project(
        structural_mismatch_root,
        {{"code/native-port-dispatch.cpp", "structure-before\n"}}));
    bool buildgraph_replacement_rejected = false;
    try {
        static_cast<void>(rewrite_codegen_project_artifacts(
            structural_mismatch_root,
            {{"build.ninja",
              "sha256:" + katana::io::sha256_bytes(
                                snapshot(structural_mismatch_root).at(
                                    "build.ninja")),
              "do-not-replace-buildgraph\n"}}));
    } catch (const std::invalid_argument&) {
        buildgraph_replacement_rejected = true;
    }
    require(buildgraph_replacement_rejected,
            "Der Consumer-Refresh ersetzt unberechtigt den Buildgraphen.");

    const auto idempotent_root = fixture.root / "idempotent";
    static_cast<void>(write_codegen_project(
        idempotent_root,
        {{"code/native-port-dispatch.cpp", "idempotent-before\n"},
         {"code/aot-partition.cpp", "aot-stable\n"}}));
    static_cast<void>(rewrite_codegen_project_artifacts(
        idempotent_root,
        {{"code/native-port-dispatch.cpp",
          "sha256:" + katana::io::sha256_bytes("idempotent-before\n"),
          "idempotent-after\n"}}));
    const auto idempotent_before = snapshot(idempotent_root);
    const auto idempotent_result = rewrite_codegen_project_artifacts(
        idempotent_root,
        {{"code/native-port-dispatch.cpp",
          "sha256:" + katana::io::sha256_bytes("idempotent-after\n"),
          "idempotent-after\n"}});
    require(idempotent_result.written_files.empty() &&
                idempotent_result.removed_files.empty() &&
                snapshot(idempotent_root) == idempotent_before,
            "Ein idempotenter Consumer-Refresh schreibt erneut Dateien.");

#ifdef _WIN32
    require(_putenv_s("KATANA_RUNTIME_ROOT", KATANA_SOURCE_DIR) == 0,
            "KATANA_RUNTIME_ROOT konnte fuer Ninja nicht gesetzt werden.");
#else
    require(setenv("KATANA_RUNTIME_ROOT", KATANA_SOURCE_DIR, 1) == 0,
            "KATANA_RUNTIME_ROOT konnte fuer Ninja nicht gesetzt werden.");
#endif
    require(run_ninja(fixture.root / "serial") == 0 &&
                std::filesystem::is_regular_file(fixture.root / "serial" / "libkatana_generated.a"),
            "Ein frisches erzeugtes Ninja-Projekt baut Runtime-Includes, Buildvertrag oder "
            "Archiv nicht eigenstaendig.");

    const auto reused = fixture.root / "reused";
    const std::vector<ProjectArtifact> reused_artifacts = {
        {"code/unit-00000.cpp", "first-0\n"},
        {"code/unit-00001.cpp", "first-1\n"},
        {"metadata/blocks.json", "blocks\n"},
        {"symbols/names.json", "symbols\n"}};
    static_cast<void>(write_codegen_project(reused, reused_artifacts));
    {
        std::ofstream externally_mutated(reused / "code/unit-00000.cpp", std::ios::binary);
        externally_mutated << "alter-0\n";
    }
    static_cast<void>(write_codegen_project(reused, reused_artifacts));
    {
        std::ifstream repaired(reused / "code/unit-00000.cpp", std::ios::binary);
        std::ostringstream repaired_content;
        repaired_content << repaired.rdbuf();
        require(repaired_content.str() == "first-0\n",
                "Eine externe Same-Size-Mutation wird vom Manifest-Skip nicht repariert.");
    }
    {
        std::ofstream v1_user_file(reused / "v1-user-owned.txt", std::ios::binary);
        v1_user_file << "must survive v1 recovery\n";
        std::ofstream legacy_manifest(reused / ".katana-generated-artifacts",
                                       std::ios::binary | std::ios::trunc);
        legacy_manifest << "katana-codegen-artifacts-v1\n"
                        << "v1-user-owned.txt\n"
                        << "code/unit-00000.cpp\n"
                        << "code/unit-00001.cpp\n"
                        << "metadata/blocks.json\n"
                        << "symbols/names.json\n"
                        << "CMakeLists.txt\n"
                        << "build.ninja\n"
                        << "compile_commands.json\n";
    }
    {
        std::ofstream externally_mutated(reused / "code/unit-00000.cpp", std::ios::binary);
        externally_mutated << "legacy-0\n";
    }
    static_cast<void>(write_codegen_project(reused, reused_artifacts));
    {
        std::ifstream recovered_manifest(reused / ".katana-generated-artifacts",
                                         std::ios::binary);
        std::string header;
        std::getline(recovered_manifest, header);
        require(header == "katana-codegen-artifacts-v2",
                "Ein v1-Manifest wird nicht sauber in einen v2-Cold-Recovery-Lauf ueberfuehrt.");
        std::ifstream v1_user_file(reused / "v1-user-owned.txt", std::ios::binary);
        std::ostringstream v1_user_content;
        v1_user_content << v1_user_file.rdbuf();
        require(v1_user_content.str() == "must survive v1 recovery\n",
                "Ein ungebundenes v1-Manifest autorisiert das Loeschen einer Nutzerdatei.");
    }
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
    static_cast<void>(
        write_codegen_project(failing, {{"code/current.cpp", "current\n"}}));
    require(std::filesystem::is_directory(failing / "code/stale.cpp") &&
                std::filesystem::is_directory(failing / "code/stale.cpp/child"),
            "Eine extern ersetzte stale Artefaktdatei wird trotz abweichender Bindung geloescht.");

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
