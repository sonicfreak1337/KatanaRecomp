#include "katana/io/symbol_map.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace katana::io {
namespace {

std::uint32_t parse_hex(
    std::string text,
    const std::filesystem::path& path,
    const std::size_t line,
    const char* field
) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0u, 2u);
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error(
            "Map-Dateifehler in " + path.string() + " in Zeile " +
            std::to_string(line) + ": ungueltiges " + field + "."
        );
    }
    return value;
}

SymbolKind parse_kind(
    const std::string& text,
    const std::filesystem::path& path,
    const std::size_t line
) {
    if (text == "F" || text == "FUNC" || text == "FUNCTION") {
        return SymbolKind::Function;
    }
    if (text == "O" || text == "OBJECT" || text == "DATA") {
        return SymbolKind::Object;
    }
    if (text == "U" || text == "UNKNOWN") {
        return SymbolKind::Unknown;
    }
    throw std::runtime_error(
        "Map-Dateifehler in " + path.string() + " in Zeile " +
        std::to_string(line) + ": unbekannte Symbolart."
    );
}

}

void load_symbol_map(
    const std::filesystem::path& path,
    ExecutableImage& image
) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Map-Datei konnte nicht geoeffnet werden: " + path.string());
    }

    std::string line_text;
    std::size_t line_number = 0;
    while (std::getline(input, line_text)) {
        ++line_number;
        const auto first = line_text.find_first_not_of(" \t\r");
        if (first == std::string::npos || line_text[first] == '#') {
            continue;
        }

        std::istringstream line(line_text);
        std::string address_text;
        std::string kind_text;
        std::string name;
        std::string size_text;
        std::string trailing;
        if (!(line >> address_text >> kind_text >> name) || (line >> size_text && line >> trailing)) {
            throw std::runtime_error(
                "Map-Dateifehler in " + path.string() + " in Zeile " +
                std::to_string(line_number) + ": erwartet ADDRESS KIND NAME [SIZE]."
            );
        }

        ImageSymbol symbol;
        symbol.address = parse_hex(address_text, path, line_number, "Adresse");
        symbol.kind = parse_kind(kind_text, path, line_number);
        symbol.name = name;
        symbol.binding = SymbolBinding::Global;
        if (!size_text.empty()) {
            symbol.size = parse_hex(size_text, path, line_number, "Groesse");
        }
        try {
            image.add_symbol(std::move(symbol));
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Map-Dateifehler in " + path.string() + " in Zeile " +
                std::to_string(line_number) + ": " + error.what()
            );
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Map-Datei konnte nicht vollstaendig gelesen werden: " + path.string());
    }
}

}
