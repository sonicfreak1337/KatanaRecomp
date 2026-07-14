#include "katana/analysis/analysis_overrides.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::analysis {
namespace {

[[noreturn]] void fail(
    const std::filesystem::path& path,
    const std::size_t line,
    const std::string& cause
) {
    throw std::runtime_error(
        "Override-Fehler in " + path.string() + " in Zeile "
        + std::to_string(line) + ": " + cause
    );
}

std::string trim(const std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1u));
}

std::uint32_t parse_number(
    std::string text,
    const int base,
    const std::filesystem::path& path,
    const std::size_t line,
    const char* field
) {
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) {
        text.erase(0u, 2u);
    }
    std::uint32_t value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        fail(path, line, std::string("ungueltiges Feld ") + field + ".");
    }
    return value;
}

std::vector<std::string> words(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::string> result;
    std::string word;
    while (input >> word) {
        result.push_back(std::move(word));
    }
    return result;
}

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << address;
    return output.str();
}

}

AnalysisOverrides parse_analysis_overrides(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Override-Datei konnte nicht geoeffnet werden: " + path.string());
    }
    AnalysisOverrides overrides;
    overrides.source_path = path;
    bool saw_version = false;
    std::string line_text;
    std::size_t line_number = 0u;
    while (std::getline(input, line_text)) {
        ++line_number;
        const auto content = trim(line_text);
        if (content.empty() || content[0] == '#') {
            continue;
        }
        const auto separator = content.find('=');
        if (separator == std::string::npos) {
            fail(path, line_number, "erwartet KEY = VALUE.");
        }
        const auto key = trim(std::string_view(content).substr(0u, separator));
        const auto value = trim(std::string_view(content).substr(separator + 1u));
        const auto fields = words(value);
        if (key == "version") {
            if (saw_version || fields.size() != 1u) {
                fail(path, line_number, "version muss genau einmal gesetzt werden.");
            }
            overrides.version = parse_number(fields[0], 10, path, line_number, "version");
            saw_version = true;
        } else if (key == "function" && fields.size() == 1u) {
            overrides.functions.push_back({
                parse_number(fields[0], 16, path, line_number, "function"), line_number
            });
        } else if (key == "jump" && fields.size() == 2u) {
            overrides.jumps.push_back({
                parse_number(fields[0], 16, path, line_number, "jump-address"),
                parse_number(fields[1], 16, path, line_number, "jump-target"),
                line_number
            });
        } else if (key == "jump_table" && fields.size() == 3u) {
            overrides.jump_tables.push_back({
                parse_number(fields[0], 16, path, line_number, "dispatch-address"),
                parse_number(fields[1], 16, path, line_number, "table-address"),
                parse_number(fields[2], 10, path, line_number, "entry-count"),
                line_number
            });
        } else {
            fail(path, line_number, "unbekanntes Feld oder falsche Feldanzahl: " + key + ".");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Override-Datei konnte nicht vollstaendig gelesen werden: " + path.string());
    }
    if (!saw_version) {
        fail(path, 0u, "Pflichtfeld version fehlt.");
    }
    if (overrides.version != 1u) {
        fail(path, 0u, "nur Override-Version 1 wird unterstuetzt.");
    }

    std::sort(overrides.functions.begin(), overrides.functions.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    std::sort(overrides.jumps.begin(), overrides.jumps.end(), [](const auto& left, const auto& right) {
        return left.instruction_address < right.instruction_address;
    });
    std::sort(overrides.jump_tables.begin(), overrides.jump_tables.end(), [](const auto& left, const auto& right) {
        return left.dispatch_address < right.dispatch_address;
    });
    if (std::adjacent_find(overrides.functions.begin(), overrides.functions.end(), [](const auto& left, const auto& right) {
            return left.address == right.address;
        }) != overrides.functions.end()) {
        fail(path, 0u, "doppelter function-Eintrag.");
    }
    if (std::adjacent_find(overrides.jumps.begin(), overrides.jumps.end(), [](const auto& left, const auto& right) {
            return left.instruction_address == right.instruction_address;
        }) != overrides.jumps.end()) {
        fail(path, 0u, "doppelter jump-Eintrag.");
    }
    if (std::adjacent_find(overrides.jump_tables.begin(), overrides.jump_tables.end(), [](const auto& left, const auto& right) {
            return left.dispatch_address == right.dispatch_address;
        }) != overrides.jump_tables.end()) {
        fail(path, 0u, "doppelter jump_table-Eintrag.");
    }
    auto jump = overrides.jumps.begin();
    auto table = overrides.jump_tables.begin();
    while (jump != overrides.jumps.end() && table != overrides.jump_tables.end()) {
        if (jump->instruction_address < table->dispatch_address) {
            ++jump;
            continue;
        }
        if (table->dispatch_address < jump->instruction_address) {
            ++table;
            continue;
        }
        const auto first_line = std::min(jump->line, table->line);
        const auto second_line = std::max(jump->line, table->line);
        throw std::runtime_error(
            "Override-Fehler in " + path.string() + " in Zeilen "
            + std::to_string(first_line) + " und " + std::to_string(second_line)
            + ": jump und jump_table verwenden dieselbe Dispatch-Adresse "
            + hex_address(jump->instruction_address) + "."
        );
    }
    return overrides;
}

}
