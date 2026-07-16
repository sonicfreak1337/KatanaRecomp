#include "katana/io/symbol_map.hpp"

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

std::string failure(const std::filesystem::path& path, katana::io::ExecutableImage& image) {
    try {
        katana::io::load_symbol_map(path, image);
    } catch (const std::exception& error) {
        return error.what();
    }
    require(false, "Eine ungueltige Map-Datei wurde akzeptiert.");
    return {};
}

} // namespace

int main() {
    using namespace katana::io;
    const auto path = std::filesystem::current_path() / "katana-symbols.map";
    save(path,
         "# Katana Symbol Map v1\n"
         "8C020000 OBJECT global_data 20\n"
         "0x8C010000 FUNC start 4\n"
         "8C010010 U helper\n");

    ExecutableImage image;
    load_symbol_map(path, image);
    require(image.symbols().size() == 3u, "Map-Symbole wurden nicht vollstaendig geladen.");
    require(image.symbols()[0].name == "start", "Symbole sind nicht deterministisch sortiert.");
    const auto* start = image.find_symbol("start");
    if (start == nullptr) {
        throw std::runtime_error("Funktionssymbol fehlt.");
    }
    require(start->address == 0x8C010000u && start->size == 4u, "Funktionssymbol ist falsch.");
    require(start->kind == SymbolKind::Function && start->binding == SymbolBinding::Global,
            "Symbolart oder Bindung ist falsch.");
    const auto* global_data = image.find_symbol("global_data");
    if (global_data == nullptr) {
        throw std::runtime_error("Datensymbol fehlt.");
    }
    require(global_data->kind == SymbolKind::Object, "Datensymbol ist falsch klassifiziert.");

    save(path, "8C010000 WRONG bad\n");
    ExecutableImage invalid;
    auto error = failure(path, invalid);
    require(error.find(path.string()) != std::string::npos &&
                error.find("Zeile 1") != std::string::npos,
            "Map-Fehler nennt Datei und Zeile nicht.");

    save(path, "8C010000 FUNC same\n8C010004 FUNC same\n");
    ExecutableImage duplicate;
    error = failure(path, duplicate);
    require(error.find("Doppelter Symbolname") != std::string::npos &&
                error.find("Zeile 2") != std::string::npos,
            "Doppeltes Symbol ist nicht diagnostisch.");

    std::filesystem::remove(path);
    std::cout << "KR-1604 Symbol- und Map-Dateimodell erfolgreich.\n";
    return EXIT_SUCCESS;
}
