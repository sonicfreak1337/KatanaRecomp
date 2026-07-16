#include "katana/io/project_manifest.hpp"

#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/io/symbol_map.hpp"

#include <charconv>
#include <fstream>
#include <map>
#include <set>
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

void reject_unknown_keys(
    const std::map<std::string, ManifestValue>& values,
    const std::set<std::string>& known_keys,
    const std::filesystem::path& path
) {
    for (const auto& [key, value] : values) {
        if (!known_keys.contains(key)) {
            fail(path, value.line, "unbekanntes Feld " + key + ".");
        }
    }
}

ProjectInputFormat parse_input_format(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "raw") { return ProjectInputFormat::RawBinary; }
    if (value.text == "elf32-sh") { return ProjectInputFormat::Elf32Sh; }
    fail(path, value.line, "Eingabeformat muss raw oder elf32-sh sein.");
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
        if (!values.emplace(key, ManifestValue{value, line_number}).second) {
            fail(path, line_number, "doppeltes Feld " + key + ".");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Projektmanifest konnte nicht vollstaendig gelesen werden: " + path.string());
    }

    const auto& version = require_value(values, "version", path);
    const auto parsed_version = parse_unsigned(
        version.text, 10, path, version.line, "version"
    );
    if (!project_manifest_version_supported(parsed_version)) {
        fail(
            path,
            version.line,
            "Manifestversion " + std::to_string(parsed_version) +
                " wird nicht unterstuetzt; erwartet 1 oder 2."
        );
    }

    ProjectManifest manifest;
    manifest.version = parsed_version;
    std::string format_key;
    std::string input_key;
    std::string map_key;
    std::string base_key;
    std::string entry_key;
    std::string segment_name_key;
    std::string segment_kind_key;
    std::string permissions_key;

    if (parsed_version == 1u) {
        reject_unknown_keys(
            values,
            {"version", "format", "input", "map", "base_address",
             "entry_point", "segment_name", "segment_kind", "permissions"},
            path
        );
        manifest.schema = "katana-project-legacy-v1";
        manifest.project_name = path.stem().string();
        format_key = "format";
        input_key = "input";
        map_key = "map";
        base_key = "base_address";
        entry_key = "entry_point";
        segment_name_key = "segment_name";
        segment_kind_key = "segment_kind";
        permissions_key = "permissions";
    } else {
        reject_unknown_keys(
            values,
            {"schema", "version", "project.name", "input.format", "input.path",
             "input.map", "image.base_address", "image.entry_point",
             "segment.name", "segment.kind", "segment.permissions"},
            path
        );
        const auto& schema = require_value(values, "schema", path);
        if (schema.text != project_manifest_schema_name) {
            fail(path, schema.line, "schema muss katana-project sein.");
        }
        manifest.schema = schema.text;
        manifest.project_name = require_value(values, "project.name", path).text;
        if (manifest.project_name.empty()) {
            fail(path, values.at("project.name").line, "project.name darf nicht leer sein.");
        }
        format_key = "input.format";
        input_key = "input.path";
        map_key = "input.map";
        base_key = "image.base_address";
        entry_key = "image.entry_point";
        segment_name_key = "segment.name";
        segment_kind_key = "segment.kind";
        permissions_key = "segment.permissions";
    }

    manifest.format = parse_input_format(require_value(values, format_key, path), path);
    manifest.input_path = resolve_path(path, require_value(values, input_key, path));

    if (const auto iterator = values.find(map_key); iterator != values.end()) {
        manifest.map_path = resolve_path(path, iterator->second);
    }
    if (const auto iterator = values.find(entry_key); iterator != values.end()) {
        manifest.entry_point = parse_unsigned(iterator->second.text, 16, path, iterator->second.line, "entry_point");
    }
    if (const auto iterator = values.find(base_key); iterator != values.end()) {
        manifest.base_address = parse_unsigned(iterator->second.text, 16, path, iterator->second.line, "base_address");
    }
    if (const auto iterator = values.find(segment_name_key); iterator != values.end()) {
        manifest.segment_name = iterator->second.text;
    }
    if (const auto iterator = values.find(segment_kind_key); iterator != values.end()) {
        manifest.segment_kind = parse_segment_kind(iterator->second, path);
    }
    if (const auto iterator = values.find(permissions_key); iterator != values.end()) {
        manifest.permissions = parse_permissions(iterator->second, path);
    }

    if (manifest.format == ProjectInputFormat::RawBinary && !manifest.base_address.has_value()) {
        fail(path, 0u, "Raw-Manifeste brauchen " + base_key + ".");
    }
    if (manifest.format == ProjectInputFormat::Elf32Sh
        && (manifest.base_address.has_value() || values.contains(segment_name_key)
            || values.contains(segment_kind_key) || values.contains(permissions_key))) {
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

bool project_manifest_version_supported(const std::uint32_t version) noexcept {
    return version == 1u || version == project_manifest_current_version;
}

}
