#include "katana/codegen/project.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) { continue; }
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

struct Fixture {
    std::filesystem::path root = std::filesystem::current_path() / "katana-project-writer-fixture";
    Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

} // namespace

int main() {
    using namespace katana::codegen;
    Fixture fixture;
    CodegenCache cache(fixture.root / "cache");
    const auto key = make_codegen_cache_key({"input", "ir", "opt", "cpp", 1u, 8u});
    const std::vector<ProjectArtifact> artifacts = {
        {"code/unit-00001.cpp", "int unit_1() { return 1; }\n"},
        {"include/constants.hpp", "#pragma once\ninline constexpr int value = 7;\n"},
        {"code/unit-00000.cpp", "int unit_0() { return 0; }\n"}
    };
    const auto serial = write_codegen_project(
        fixture.root / "serial", artifacts, {1u, &cache, key}
    );
    const auto parallel = write_codegen_project(
        fixture.root / "parallel", artifacts, {4u, &cache, key}
    );
    const auto serial_snapshot = snapshot(fixture.root / "serial");
    const auto parallel_snapshot = snapshot(fixture.root / "parallel");
    require(
        serial.cache_hits == 0u && serial.cache_misses == artifacts.size() &&
            parallel.cache_hits == artifacts.size() && parallel.cache_misses == 0u &&
            serial.written_files == parallel.written_files &&
            serial_snapshot == parallel_snapshot,
        "Serielle und parallele Ausgabe oder Cachetreffer sind nicht deterministisch."
    );
    require(
        serial_snapshot.at("CMakeLists.txt").find("code/unit-00000.cpp") != std::string::npos &&
            serial_snapshot.at("build.ninja").find("libkatana_generated.a") != std::string::npos &&
            serial_snapshot.at("compile_commands.json").find("\"directory\":\".\"") !=
                std::string::npos,
        "CMake-, Ninja- oder Compile-Commands-Integration fehlt."
    );

    std::cout << "KR-3304 parallele Ausgabe und Buildintegration erfolgreich.\n";
    return 0;
}
