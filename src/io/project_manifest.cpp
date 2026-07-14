#include "katana/io/project_manifest.hpp"

#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/io/symbol_map.hpp"

#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::io {
namespace {

struct ManifestValue {
    std::string text;
    std::size_t line = 0u;
};

[[noreturn]] void fail(
    const std::filesystem::path& path,
    const std::size_t line,
    const std::string& cause
) {
    throw std::runtime_error(
        "Projektmanifestfehler in " + path.string() + " in Zeile "
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

std::uint32_t parse_unsigned(
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
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        fail(path, line, std::string("ungueltiges Feld ") + field + ".");
    }
    return value;
}

const ManifestValue& require_value(
    const std::map<std::string, ManifestValue>& values,
    const std::string& key,
    const std::filesystem::path& path
) {
    const auto iterator = values.find(key);
    if (iterator == values.end()) {
        fail(path, 0u, "Pflichtfeld " + key + " fehlt.");
    }
    return iterator->second;
}

std::filesystem::path resolve_path(
    const std::filesystem::path& manifest_path,
    const ManifestValue& value
) {
    auto path = std::filesystem::path(value.text);
    if (path.is_relative()) {
        path = manifest_path.parent_path() / path;
    }
    return path.lexically_normal();
}

SegmentKind parse_segment_kind(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "code") {
        return SegmentKind::Code;
    }
    if (value.text == "data") {
        return SegmentKind::Data;
    }
    if (value.text == "unknown") {
        return SegmentKind::Unknown;
    }
    fail(path, value.line, "segment_kind muss code, data oder unknown sein.");
}

SegmentPermissions parse_permissions(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text.size() != 3u
        || (value.text[0] != 'r' && value.text[0] != '-')
        || (value.text[1] != 'w' && value.text[1] != '-')
        || (value.text[2] != 'x' && value.text[2] != '-')) {
        fail(path, value.line, "permissions muss die Form rwx mit '-' fuer fehlende Rechte besitzen.");
    }
    return {value.text[0] == 'r', value.text[1] == 'w', value.text[2] == 'x'};
}

}

ProjectManifest parse_project_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Projektmanifest konnte nicht geoeffnet werden: " + path.string());
    }

    const std::map<std::string, bool> known_keys{
        {"version", true}, {"format", true}, {"input", true}, {"map", true},
        {"base_address", true}, {"entry_point", true}, {"segment_name", true},
        {"segment_kind", true}, {"permissions", true}
    };
    std::map<std::string, ManifestValue> values;
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
        if (key.empty() || value.empty()) {
            fail(path, line_number, "Schluessel und Wert duerfen nicht leer sein.");
        }
        if (!known_keys.contains(key)) {
            fail(path, line_number, "unbekanntes Feld " + key + ".");
        }
        if (!values.emplace(key, ManifestValue{value, line_number}).second) {
            fail(path, line_number, "doppeltes Feld " + key + ".");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Projektmanifest konnte nicht vollstaendig gelesen werden: " + path.string());
    }

    ProjectManifest manifest;
    const auto& version = require_value(values, "version", path);
    manifest.version = parse_unsigned(version.text, 10, path, version.line, "version");
    if (manifest.version != 1u) {
        fail(path, version.line, "nur Manifestversion 1 wird unterstuetzt.");
    }

    const auto& format = require_value(values, "format", path);
    if (format.text == "raw") {
        manifest.format = ProjectInputFormat::RawBinary;
    } else if (format.text == "elf32-sh") {
        manifest.format = ProjectInputFormat::Elf32Sh;
    } else {
        fail(path, format.line, "format muss raw oder elf32-sh sein.");
    }
    manifest.input_path = resolve_path(path, require_value(values, "input", path));

    if (const auto iterator = values.find("map"); iterator != values.end()) {
        manifest.map_path = resolve_path(path, iterator->second);
    }
    if (const auto iterator = values.find("entry_point"); iterator != values.end()) {
        manifest.entry_point = parse_unsigned(iterator->second.text, 16, path, iterator->second.line, "entry_point");
    }
    if (const auto iterator = values.find("base_address"); iterator != values.end()) {
        manifest.base_address = parse_unsigned(iterator->second.text, 16, path, iterator->second.line, "base_address");
    }
    if (const auto iterator = values.find("segment_name"); iterator != values.end()) {
        manifest.segment_name = iterator->second.text;
    }
    if (const auto iterator = values.find("segment_kind"); iterator != values.end()) {
        manifest.segment_kind = parse_segment_kind(iterator->second, path);
    }
    if (const auto iterator = values.find("permissions"); iterator != values.end()) {
        manifest.permissions = parse_permissions(iterator->second, path);
    }

    if (manifest.format == ProjectInputFormat::RawBinary && !manifest.base_address.has_value()) {
        fail(path, 0u, "Raw-Manifeste brauchen base_address.");
    }
    if (manifest.format == ProjectInputFormat::Elf32Sh
        && (manifest.base_address.has_value() || values.contains("segment_name")
            || values.contains("segment_kind") || values.contains("permissions"))) {
        fail(path, 0u, "Raw-Adresslayoutfelder sind fuer elf32-sh nicht erlaubt.");
    }
    return manifest;
}

ExecutableImage load_project_manifest(const std::filesystem::path& path) {
    const auto manifest = parse_project_manifest(path);
    ExecutableImage image;
    if (manifest.format == ProjectInputFormat::RawBinary) {
        RawBinaryLoadOptions options;
        options.base_address = *manifest.base_address;
        options.segment_name = manifest.segment_name;
        options.segment_kind = manifest.segment_kind;
        options.permissions = manifest.permissions;
        options.entry_point = manifest.entry_point;
        image = load_raw_binary(manifest.input_path, options);
    } else {
        image = load_elf32_sh(manifest.input_path);
        if (manifest.entry_point.has_value()) {
            image.add_entry_point(*manifest.entry_point);
        }
    }
    if (manifest.map_path.has_value()) {
        load_symbol_map(*manifest.map_path, image);
    }
    return image;
}

const char* project_input_format_name(const ProjectInputFormat format) noexcept {
    switch (format) {
        case ProjectInputFormat::RawBinary: return "raw";
        case ProjectInputFormat::Elf32Sh: return "elf32-sh";
    }
    return "unknown";
}

}
