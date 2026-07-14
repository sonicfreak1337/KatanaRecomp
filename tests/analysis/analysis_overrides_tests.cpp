#include "katana/analysis/analysis_overrides.hpp"

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

void save(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
}

}

int main() {
    const auto path = std::filesystem::current_path() / "analysis-overrides-test.txt";
    save(path,
        "# Reihenfolge ist absichtlich unsortiert\n"
        "version = 1\n"
        "function = 0x1020\n"
        "jump = 0x1010 0x1030\n"
        "function = 0x1000\n"
        "jump_table = 0x1040 0x2000 3\n");
    const auto overrides = katana::analysis::parse_analysis_overrides(path);
    require(overrides.functions.size() == 2u, "Funktionshinweise fehlen.");
    require(
        overrides.functions[0].address == 0x1000u && overrides.functions[1].address == 0x1020u
            && overrides.functions[0].line == 5u,
        "Funktionshinweise wurden nicht deterministisch sortiert."
    );
    require(
        overrides.source_path == path
            && overrides.jumps.size() == 1u
            && overrides.jumps[0].target == 0x1030u
            && overrides.jumps[0].line == 4u,
        "Sprungoverride wurde falsch gelesen."
    );
    require(
        overrides.jump_tables.size() == 1u
            && overrides.jump_tables[0].entry_count == 3u
            && overrides.jump_tables[0].line == 6u,
        "Jump-Table-Hinweis wurde falsch gelesen."
    );

    save(path, "version = 2\n");
    try {
        static_cast<void>(katana::analysis::parse_analysis_overrides(path));
        require(false, "Unbekannte Override-Version wurde akzeptiert.");
    } catch (const std::exception& error) {
        require(std::string(error.what()).find("Version 1") != std::string::npos, "Versionsdiagnose fehlt.");
    }
    std::filesystem::remove(path);
    std::cout << "KR-1805 Override-Datei erfolgreich.\n";
    return EXIT_SUCCESS;
}
