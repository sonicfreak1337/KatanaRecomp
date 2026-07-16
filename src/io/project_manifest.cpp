#include "katana/io/project_manifest.hpp"

#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/io/symbol_map.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

std::vector<std::string> split_list(const std::string_view text, const char delimiter) {
    std::vector<std::string> result;
    std::size_t start = 0u;
    for (;;) {
        const auto end = text.find(delimiter, start);
        const auto item = trim(text.substr(start, end == std::string_view::npos
            ? text.size() - start : end - start));
        if (item.empty()) {
            return {};
        }
        result.push_back(item);
        if (end == std::string_view::npos) {
            return result;
        }
        start = end + 1u;
    }
}

std::vector<ProjectAddressRange> parse_ranges(
    const ManifestValue& value,
    const std::filesystem::path& path,
    const char* field
) {
    std::vector<ProjectAddressRange> ranges;
    const auto items = split_list(value.text, ',');
    if (items.empty()) {
        fail(path, value.line, std::string(field) + " enthaelt eine leere Liste.");
    }
    for (const auto& item : items) {
        const auto fields = split_list(item, ':');
        if (fields.size() != 2u) {
            fail(path, value.line, std::string(field) + " braucht start:size-Eintraege.");
        }
        ProjectAddressRange range{
            parse_unsigned(fields[0], 16, path, value.line, field),
            parse_unsigned(fields[1], 16, path, value.line, field)
        };
        if (range.size == 0u || static_cast<std::uint64_t>(range.start) + range.size
                > std::numeric_limits<std::uint32_t>::max() + 1ull) {
            fail(path, value.line, std::string(field) + " enthaelt einen ungueltigen Bereich.");
        }
        ranges.push_back(range);
    }
    return ranges;
}

std::vector<ProjectAliasGroup> parse_aliases(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    std::vector<ProjectAliasGroup> aliases;
    const auto items = split_list(value.text, ',');
    if (items.empty()) {
        fail(path, value.line, "memory.alias_groups enthaelt eine leere Liste.");
    }
    for (const auto& item : items) {
        const auto fields = split_list(item, ':');
        if (fields.size() != 3u) {
            fail(path, value.line, "memory.alias_groups braucht virtual:physical:size-Eintraege.");
        }
        ProjectAliasGroup alias{
            parse_unsigned(fields[0], 16, path, value.line, "alias-virtual"),
            parse_unsigned(fields[1], 16, path, value.line, "alias-physical"),
            parse_unsigned(fields[2], 16, path, value.line, "alias-size")
        };
        const auto virtual_end = static_cast<std::uint64_t>(alias.virtual_start) + alias.size;
        const auto physical_end = static_cast<std::uint64_t>(alias.physical_start) + alias.size;
        if (alias.size == 0u || virtual_end > std::numeric_limits<std::uint32_t>::max() + 1ull
            || physical_end > std::numeric_limits<std::uint32_t>::max() + 1ull) {
            fail(path, value.line, "memory.alias_groups enthaelt einen ungueltigen Bereich.");
        }
        aliases.push_back(alias);
    }
    return aliases;
}

std::vector<std::uint32_t> parse_addresses(
    const ManifestValue& value,
    const std::filesystem::path& path,
    const char* field
) {
    std::vector<std::uint32_t> result;
    const auto items = split_list(value.text, ',');
    if (items.empty()) {
        fail(path, value.line, std::string(field) + " enthaelt eine leere Liste.");
    }
    for (const auto& item : items) {
        result.push_back(parse_unsigned(item, 16, path, value.line, field));
    }
    return result;
}

bool ranges_overlap(
    const std::uint32_t left_start,
    const std::uint32_t left_size,
    const std::uint32_t right_start,
    const std::uint32_t right_size
) noexcept {
    const auto left_end = static_cast<std::uint64_t>(left_start) + left_size;
    const auto right_end = static_cast<std::uint64_t>(right_start) + right_size;
    return static_cast<std::uint64_t>(left_start) < right_end
        && static_cast<std::uint64_t>(right_start) < left_end;
}

bool range_contains(
    const ProjectAddressRange& outer,
    const std::uint32_t start,
    const std::uint32_t size
) noexcept {
    return start >= outer.start
        && static_cast<std::uint64_t>(start) + size
            <= static_cast<std::uint64_t>(outer.start) + outer.size;
}

ProjectFirmwareMode parse_firmware_mode(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "direct") return ProjectFirmwareMode::Direct;
    if (value.text == "hle") return ProjectFirmwareMode::Hle;
    if (value.text == "lle") return ProjectFirmwareMode::Lle;
    fail(path, value.line, "execution.firmware muss direct, hle oder lle sein.");
}

ProjectFallbackPolicy parse_fallback_policy(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "abort") return ProjectFallbackPolicy::Abort;
    if (value.text == "interpreter") return ProjectFallbackPolicy::Interpreter;
    if (value.text == "diagnostic") return ProjectFallbackPolicy::Diagnostic;
    fail(path, value.line, "execution.fallback muss abort, interpreter oder diagnostic sein.");
}

ProjectMmuProfile parse_mmu_profile(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "disabled") return ProjectMmuProfile::Disabled;
    if (value.text == "sh4") return ProjectMmuProfile::Sh4;
    fail(path, value.line, "execution.mmu muss disabled oder sh4 sein.");
}

ProjectFastpathProfile parse_fastpath_profile(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    if (value.text == "conservative") return ProjectFastpathProfile::Conservative;
    if (value.text == "guarded") return ProjectFastpathProfile::Guarded;
    fail(path, value.line, "execution.fastpath muss conservative oder guarded sein.");
}

std::vector<std::string> parse_capabilities(
    const ManifestValue& value,
    const std::filesystem::path& path
) {
    static const std::set<std::string> known{
        "memory", "scheduler", "interrupts", "dma", "controlled-fallback",
        "mmu", "watchpoints", "executable-ram", "firmware-mode", "store-queues"
    };
    auto capabilities = split_list(value.text, ',');
    if (capabilities.empty()) {
        fail(path, value.line, "execution.required_capabilities enthaelt eine leere Liste.");
    }
    for (const auto& capability : capabilities) {
        if (!known.contains(capability)) {
            fail(path, value.line, "unbekannte Backend-Faehigkeit " + capability + ".");
        }
    }
    std::sort(capabilities.begin(), capabilities.end());
    if (std::adjacent_find(capabilities.begin(), capabilities.end()) != capabilities.end()) {
        fail(path, value.line, "doppelte Backend-Faehigkeit.");
    }
    return capabilities;
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
             "image.expected_entry_points", "image.dynamic_bios_vectors",
             "segment.name", "segment.kind", "segment.permissions",
             "execution.firmware", "execution.fallback", "execution.scheduler",
             "execution.mmu", "execution.fastpath", "execution.required_capabilities",
             "firmware.bios", "firmware.flash", "memory.alias_groups",
             "memory.canonical_ranges", "memory.writable_executable"},
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

    if (parsed_version == project_manifest_current_version) {
        if (const auto iterator = values.find("execution.firmware"); iterator != values.end()) {
            manifest.firmware_mode = parse_firmware_mode(iterator->second, path);
        }
        if (const auto iterator = values.find("execution.fallback"); iterator != values.end()) {
            manifest.fallback_policy = parse_fallback_policy(iterator->second, path);
        }
        if (const auto iterator = values.find("execution.scheduler"); iterator != values.end()) {
            if (iterator->second.text != "deterministic") {
                fail(path, iterator->second.line,
                    "execution.scheduler muss deterministic sein.");
            }
            manifest.scheduler_profile = iterator->second.text;
        }
        if (const auto iterator = values.find("execution.mmu"); iterator != values.end()) {
            manifest.mmu_profile = parse_mmu_profile(iterator->second, path);
        }
        if (const auto iterator = values.find("execution.fastpath"); iterator != values.end()) {
            manifest.fastpath_profile = parse_fastpath_profile(iterator->second, path);
        }
        if (const auto iterator = values.find("execution.required_capabilities");
            iterator != values.end()) {
            manifest.required_backend_capabilities = parse_capabilities(iterator->second, path);
        }
        if (const auto iterator = values.find("firmware.bios"); iterator != values.end()) {
            manifest.bios_path = resolve_path(path, iterator->second);
        }
        if (const auto iterator = values.find("firmware.flash"); iterator != values.end()) {
            manifest.flash_path = resolve_path(path, iterator->second);
        }
        if (const auto iterator = values.find("memory.alias_groups"); iterator != values.end()) {
            manifest.alias_groups = parse_aliases(iterator->second, path);
        }
        if (const auto iterator = values.find("memory.canonical_ranges");
            iterator != values.end()) {
            manifest.canonical_physical_ranges = parse_ranges(
                iterator->second, path, "memory.canonical_ranges"
            );
        }
        if (const auto iterator = values.find("memory.writable_executable");
            iterator != values.end()) {
            manifest.writable_executable_ranges = parse_ranges(
                iterator->second, path, "memory.writable_executable"
            );
        }
        if (const auto iterator = values.find("image.expected_entry_points");
            iterator != values.end()) {
            manifest.expected_entry_points = parse_addresses(
                iterator->second, path, "image.expected_entry_points"
            );
        }
        if (const auto iterator = values.find("image.dynamic_bios_vectors");
            iterator != values.end()) {
            manifest.dynamic_bios_vectors = parse_addresses(
                iterator->second, path, "image.dynamic_bios_vectors"
            );
        }
    }

    if (manifest.format == ProjectInputFormat::RawBinary && !manifest.base_address.has_value()) {
        fail(path, 0u, "Raw-Manifeste brauchen " + base_key + ".");
    }
    if (manifest.format == ProjectInputFormat::Elf32Sh
        && (manifest.base_address.has_value() || values.contains(segment_name_key)
            || values.contains(segment_kind_key) || values.contains(permissions_key))) {
        fail(path, 0u, "Raw-Adresslayoutfelder sind fuer elf32-sh nicht erlaubt.");
    }

    for (std::size_t left = 0u; left < manifest.alias_groups.size(); ++left) {
        const auto& alias = manifest.alias_groups[left];
        for (std::size_t right = left + 1u; right < manifest.alias_groups.size(); ++right) {
            const auto& candidate = manifest.alias_groups[right];
            if (ranges_overlap(
                    alias.virtual_start, alias.size, candidate.virtual_start, candidate.size
                )) {
                fail(path, values.at("memory.alias_groups").line,
                    "widerspruechliche ueberlappende Aliasgruppen.");
            }
        }
        if (!std::any_of(
                manifest.canonical_physical_ranges.begin(),
                manifest.canonical_physical_ranges.end(),
                [&alias](const auto& range) {
                    return range_contains(range, alias.physical_start, alias.size);
                }
            )) {
            fail(path, values.at("memory.alias_groups").line,
                "Aliasziel liegt ausserhalb der kanonischen physischen Bereiche.");
        }
    }

    const auto has_capability = [&manifest](const std::string_view capability) {
        return std::binary_search(
            manifest.required_backend_capabilities.begin(),
            manifest.required_backend_capabilities.end(),
            std::string(capability)
        );
    };
    if (manifest.mmu_profile == ProjectMmuProfile::Sh4 && !has_capability("mmu")) {
        fail(path, 0u, "execution.mmu=sh4 braucht die Backend-Faehigkeit mmu.");
    }
    if (!manifest.writable_executable_ranges.empty()
        && !has_capability("executable-ram")) {
        fail(path, 0u,
            "beschreibbare ausfuehrbare Bereiche brauchen executable-ram.");
    }
    for (const auto& range : manifest.writable_executable_ranges) {
        if (!manifest.canonical_physical_ranges.empty()
            && !std::any_of(
                manifest.canonical_physical_ranges.begin(),
                manifest.canonical_physical_ranges.end(),
                [&range](const auto& canonical) {
                    return range_contains(canonical, range.start, range.size);
                }
            )) {
            fail(path, values.at("memory.writable_executable").line,
                "beschreibbarer ausfuehrbarer Bereich ist nicht kanonisch.");
        }
    }
    if (manifest.firmware_mode == ProjectFirmwareMode::Lle) {
        if (!manifest.bios_path.has_value() || manifest.alias_groups.empty()
            || manifest.canonical_physical_ranges.empty()
            || !has_capability("firmware-mode") || !has_capability("memory")) {
            fail(path, 0u,
                "LLE braucht BIOS, Aliasgruppen, kanonische Bereiche sowie "
                "memory und firmware-mode.");
        }
    } else if (manifest.bios_path.has_value()) {
        fail(path, values.at("firmware.bios").line,
            "firmware.bios ist nur im LLE-Modus erlaubt.");
    }
    if (manifest.firmware_mode == ProjectFirmwareMode::Hle
        && !has_capability("firmware-mode")) {
        fail(path, 0u, "HLE braucht die Backend-Faehigkeit firmware-mode.");
    }
    if (manifest.fallback_policy != ProjectFallbackPolicy::Abort
        && !has_capability("controlled-fallback")) {
        fail(path, 0u,
            "ein aktiver Fallback braucht die Backend-Faehigkeit controlled-fallback.");
    }
    std::sort(manifest.expected_entry_points.begin(), manifest.expected_entry_points.end());
    manifest.expected_entry_points.erase(
        std::unique(
            manifest.expected_entry_points.begin(), manifest.expected_entry_points.end()
        ),
        manifest.expected_entry_points.end()
    );
    std::sort(manifest.dynamic_bios_vectors.begin(), manifest.dynamic_bios_vectors.end());
    manifest.dynamic_bios_vectors.erase(
        std::unique(manifest.dynamic_bios_vectors.begin(), manifest.dynamic_bios_vectors.end()),
        manifest.dynamic_bios_vectors.end()
    );
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
    for (const auto entry : manifest.expected_entry_points) {
        image.add_entry_point(entry);
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

const char* project_firmware_mode_name(const ProjectFirmwareMode mode) noexcept {
    switch (mode) {
        case ProjectFirmwareMode::Direct: return "direct";
        case ProjectFirmwareMode::Hle: return "hle";
        case ProjectFirmwareMode::Lle: return "lle";
    }
    return "unknown";
}

const char* project_fallback_policy_name(const ProjectFallbackPolicy policy) noexcept {
    switch (policy) {
        case ProjectFallbackPolicy::Abort: return "abort";
        case ProjectFallbackPolicy::Interpreter: return "interpreter";
        case ProjectFallbackPolicy::Diagnostic: return "diagnostic";
    }
    return "unknown";
}

bool project_manifest_version_supported(const std::uint32_t version) noexcept {
    return version == 1u || version == project_manifest_current_version;
}

}
