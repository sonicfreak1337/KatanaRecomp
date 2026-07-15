#include "katana/runtime/gdi.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {
[[noreturn]] void fail(
    const std::string& descriptor,
    const std::size_t line,
    const std::string& message
) {
    throw std::runtime_error(
        "GDI-Descriptor '" + descriptor + "', Zeile " + std::to_string(line) + ": " + message
    );
}

std::vector<std::string> tokenize(
    const std::string& descriptor,
    const std::size_t line_number,
    const std::string_view line
) {
    std::vector<std::string> result;
    std::size_t position = 0u;
    while (position < line.size()) {
        while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) { ++position; }
        if (position == line.size()) { break; }
        if (line[position] == '"') {
            const auto end = line.find('"', position + 1u);
            if (end == std::string_view::npos) { fail(descriptor, line_number, "nicht abgeschlossenes Dateinamen-Zitat."); }
            result.emplace_back(line.substr(position + 1u, end - position - 1u));
            position = end + 1u;
            if (position < line.size() && line[position] != ' ' && line[position] != '\t') {
                fail(descriptor, line_number, "unerwartete Zeichen nach zitiertem Dateinamen.");
            }
        } else {
            const auto end = line.find_first_of(" \t", position);
            result.emplace_back(line.substr(position, end - position));
            position = end == std::string_view::npos ? line.size() : end;
        }
    }
    return result;
}

template<typename Integer>
Integer parse_integer(
    const std::string& descriptor,
    const std::size_t line,
    const std::string& field,
    const std::string& value
) {
    Integer result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        fail(descriptor, line, "ungueltiges Ganzzahlfeld '" + field + "'.");
    }
    return result;
}

bool supported_sector_size(const std::uint32_t value) {
    return value == 2048u || value == 2336u || value == 2352u || value == 2448u;
}
}

GdiDescriptor parse_gdi_descriptor(const std::filesystem::path& descriptor_path) {
    const auto descriptor_name = descriptor_path.filename().string();
    std::ifstream input(descriptor_path);
    if (!input) { throw std::invalid_argument("GDI-Descriptor '" + descriptor_name + "' ist nicht lesbar."); }
    std::string line;
    if (!std::getline(input, line)) { fail(descriptor_name, 1u, "Trackanzahl fehlt."); }
    const auto count_tokens = tokenize(descriptor_name, 1u, line);
    if (count_tokens.size() != 1u) { fail(descriptor_name, 1u, "Trackanzahlzeile besitzt Zusatzfelder."); }
    const auto expected_count = parse_integer<std::uint32_t>(descriptor_name, 1u, "Trackanzahl", count_tokens[0]);
    if (expected_count == 0u) { fail(descriptor_name, 1u, "Trackanzahl darf nicht null sein."); }

    GdiDescriptor result{descriptor_name, {}};
    result.tracks.reserve(expected_count);
    std::size_t line_number = 1u;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) { continue; }
        const auto fields = tokenize(descriptor_name, line_number, line);
        if (fields.size() != 6u) { fail(descriptor_name, line_number, "Trackzeile braucht exakt sechs Felder."); }
        GdiTrack track;
        track.number = parse_integer<std::uint32_t>(descriptor_name, line_number, "Track", fields[0]);
        track.lba = parse_integer<std::uint32_t>(descriptor_name, line_number, "LBA", fields[1]);
        const auto type = parse_integer<std::uint32_t>(descriptor_name, line_number, "Typ", fields[2]);
        track.sector_size = parse_integer<std::uint32_t>(descriptor_name, line_number, "Sektorgroesse", fields[3]);
        track.file_name = fields[4];
        track.file_offset = parse_integer<std::uint64_t>(descriptor_name, line_number, "Dateioffset", fields[5]);
        track.descriptor_line = line_number;
        if (type != 0u && type != 4u) { fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " besitzt einen ungueltigen Typ."); }
        track.type = static_cast<GdiTrackType>(type);
        if (!supported_sector_size(track.sector_size)) {
            fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " besitzt ein unplausibles Sektorformat.");
        }
        const std::filesystem::path relative(track.file_name);
        if (relative.empty() || relative.is_absolute()) {
            fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " braucht einen relativen Dateipfad.");
        }
        track.resolved_path = (descriptor_path.parent_path() / relative).lexically_normal();
        std::error_code error;
        if (!std::filesystem::is_regular_file(track.resolved_path, error) || error) {
            fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " verweist auf eine fehlende Datei.");
        }
        const auto file_size = std::filesystem::file_size(track.resolved_path, error);
        if (error || track.file_offset > file_size ||
            (file_size - track.file_offset) % track.sector_size != 0u) {
            fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " widerspricht Dateigroesse oder Offset.");
        }
        track.sector_count = (file_size - track.file_offset) / track.sector_size;
        if (track.sector_count == 0u) { fail(descriptor_name, line_number, "Track " + std::to_string(track.number) + " ist leer."); }
        if (!result.tracks.empty() &&
            (track.number <= result.tracks.back().number || track.lba <= result.tracks.back().lba)) {
            fail(descriptor_name, line_number, "Tracknummern und LBAs muessen streng aufsteigend sein.");
        }
        result.tracks.push_back(std::move(track));
    }
    if (result.tracks.size() != expected_count) {
        fail(descriptor_name, line_number + 1u, "deklarierte und gelesene Trackanzahl stimmen nicht ueberein.");
    }
    for (std::size_t index = 0u; index < result.tracks.size(); ++index) {
        if (result.tracks[index].number != index + 1u) {
            fail(descriptor_name, result.tracks[index].descriptor_line, "Tracknummern muessen bei 1 beginnen und lueckenlos sein.");
        }
    }
    return result;
}

} // namespace katana::runtime
