#include "katana/codegen/source_map.hpp"

#include "katana/io/json_report.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::codegen {
namespace {

constexpr std::string_view marker = "// katana-guest 0x";

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

bool portable_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    return std::none_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

bool portable_label(const std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_' || character == '-';
    });
}

bool location_less(
    const AddressSourceLocation& left,
    const AddressSourceLocation& right
) {
    if (left.guest_address != right.guest_address) {
        return left.guest_address < right.guest_address;
    }
    if (left.generated_path != right.generated_path) {
        return left.generated_path < right.generated_path;
    }
    return left.generated_line < right.generated_line;
}

}

std::vector<AddressSourceLocation> build_address_source_map(
    const katana::io::ExecutableImage& image,
    const std::span<const ProjectArtifact> generated_units
) {
    std::vector<AddressSourceLocation> result;
    for (const auto& unit : generated_units) {
        if (unit.relative_path.extension() != ".cpp") continue;
        if (!portable_relative_path(unit.relative_path)) {
            throw std::invalid_argument("Source-Map-Unit besitzt keinen portablen relativen Pfad.");
        }
        std::size_t line_number = 0u;
        std::size_t offset = 0u;
        while (offset <= unit.content.size()) {
            ++line_number;
            const auto end = unit.content.find('\n', offset);
            const auto line = std::string_view(unit.content).substr(
                offset,
                end == std::string::npos ? unit.content.size() - offset : end - offset
            );
            const auto found = line.find(marker);
            if (found != std::string_view::npos) {
                const auto digits = line.substr(found + marker.size(), 8u);
                std::uint32_t address = 0u;
                const auto parsed = std::from_chars(
                    digits.data(), digits.data() + digits.size(), address, 16
                );
                if (digits.size() != 8u || parsed.ec != std::errc{}
                    || parsed.ptr != digits.data() + digits.size()) {
                    throw std::runtime_error("Generierter Source-Map-Marker ist ungueltig.");
                }
                const auto* segment = image.find_segment(address);
                const auto byte_offset = segment == nullptr
                    ? std::optional<std::size_t>{}
                    : segment->byte_offset(address);
                if (segment == nullptr || !byte_offset.has_value()) {
                    throw std::runtime_error("Source-Map-Adresse liegt ausserhalb des Images.");
                }
                if (!portable_label(segment->name)) {
                    throw std::runtime_error("Source-Map-Segmentname ist nicht portabel.");
                }
                if (*byte_offset > std::numeric_limits<std::uint64_t>::max()
                        - segment->file_offset) {
                    throw std::overflow_error("Source-Map-Dateioffset laeuft ueber.");
                }
                result.push_back({
                    address,
                    segment->name,
                    segment->file_offset + *byte_offset,
                    unit.relative_path.generic_string(),
                    line_number
                });
            }
            if (end == std::string::npos) break;
            offset = end + 1u;
        }
    }
    std::sort(result.begin(), result.end(), location_less);
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.guest_address == right.guest_address
            && left.generated_path == right.generated_path
            && left.generated_line == right.generated_line;
    }), result.end());
    return result;
}

std::span<const AddressSourceLocation> find_source_locations(
    const std::span<const AddressSourceLocation> locations,
    const std::uint32_t guest_address
) noexcept {
    const auto begin = std::lower_bound(
        locations.begin(), locations.end(), guest_address,
        [](const auto& location, const std::uint32_t address) {
            return location.guest_address < address;
        }
    );
    const auto end = std::upper_bound(
        begin, locations.end(), guest_address,
        [](const std::uint32_t address, const auto& location) {
            return address < location.guest_address;
        }
    );
    return {begin, end};
}

std::string serialize_address_source_map(
    const std::span<const AddressSourceLocation> locations
) {
    for (std::size_t index = 1u; index < locations.size(); ++index) {
        if (location_less(locations[index], locations[index - 1u])
            || (!location_less(locations[index - 1u], locations[index])
                && !location_less(locations[index], locations[index - 1u]))) {
            throw std::invalid_argument("Source Map ist nicht nach Gastadresse sortiert.");
        }
    }
    for (const auto& location : locations) {
        if (!portable_label(location.input_segment)
            || !portable_relative_path(std::filesystem::path(location.generated_path))
            || location.generated_line == 0u) {
            throw std::invalid_argument("Source Map enthaelt eine unportable Quellposition.");
        }
    }
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-address-source-map", "source-map");
    output << ",\"source_map_version\":" << source_map_schema_version
           << ",\"locations\":[";
    for (std::size_t index = 0u; index < locations.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& location = locations[index];
        output << "{\"guest_address\":" << katana::io::quote_json(hex32(location.guest_address))
               << ",\"input_segment\":" << katana::io::quote_json(location.input_segment)
               << ",\"input_byte_offset\":" << location.input_byte_offset
               << ",\"generated_path\":" << katana::io::quote_json(location.generated_path)
               << ",\"generated_line\":" << location.generated_line << '}';
    }
    output << "]}";
    return output.str();
}

}
