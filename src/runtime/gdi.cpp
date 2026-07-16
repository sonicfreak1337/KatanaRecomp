#include "katana/runtime/gdi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {
[[noreturn]] void
fail(const std::string& descriptor, const std::size_t line, const std::string& message) {
    throw std::runtime_error("GDI-Descriptor '" + descriptor + "', Zeile " + std::to_string(line) +
                             ": " + message);
}

std::vector<std::string> tokenize(const std::string& descriptor,
                                  const std::size_t line_number,
                                  const std::string_view line) {
    std::vector<std::string> result;
    std::size_t position = 0u;
    while (position < line.size()) {
        while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
            ++position;
        }
        if (position == line.size()) {
            break;
        }
        if (line[position] == '"') {
            const auto end = line.find('"', position + 1u);
            if (end == std::string_view::npos) {
                fail(descriptor, line_number, "nicht abgeschlossenes Dateinamen-Zitat.");
            }
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

template <typename Integer>
Integer parse_integer(const std::string& descriptor,
                      const std::size_t line,
                      const std::string& field,
                      const std::string& value) {
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
} // namespace

GdiDescriptor parse_gdi_descriptor(const std::filesystem::path& descriptor_path) {
    const auto descriptor_name = descriptor_path.filename().string();
    std::ifstream input(descriptor_path);
    if (!input) {
        throw std::invalid_argument("GDI-Descriptor '" + descriptor_name + "' ist nicht lesbar.");
    }
    std::string line;
    if (!std::getline(input, line)) {
        fail(descriptor_name, 1u, "Trackanzahl fehlt.");
    }
    const auto count_tokens = tokenize(descriptor_name, 1u, line);
    if (count_tokens.size() != 1u) {
        fail(descriptor_name, 1u, "Trackanzahlzeile besitzt Zusatzfelder.");
    }
    const auto expected_count =
        parse_integer<std::uint32_t>(descriptor_name, 1u, "Trackanzahl", count_tokens[0]);
    if (expected_count == 0u) {
        fail(descriptor_name, 1u, "Trackanzahl darf nicht null sein.");
    }

    std::error_code canonical_error;
    const auto descriptor_directory =
        std::filesystem::weakly_canonical(descriptor_path.parent_path(), canonical_error);
    if (canonical_error) {
        throw std::invalid_argument("GDI-Descriptorverzeichnis ist nicht aufloesbar.");
    }

    GdiDescriptor result{descriptor_name, {}};
    result.tracks.reserve(expected_count);
    std::size_t line_number = 1u;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        const auto fields = tokenize(descriptor_name, line_number, line);
        if (fields.size() != 6u) {
            fail(descriptor_name, line_number, "Trackzeile braucht exakt sechs Felder.");
        }
        GdiTrack track;
        track.number =
            parse_integer<std::uint32_t>(descriptor_name, line_number, "Track", fields[0]);
        track.lba = parse_integer<std::uint32_t>(descriptor_name, line_number, "LBA", fields[1]);
        const auto type =
            parse_integer<std::uint32_t>(descriptor_name, line_number, "Typ", fields[2]);
        track.sector_size =
            parse_integer<std::uint32_t>(descriptor_name, line_number, "Sektorgroesse", fields[3]);
        track.file_name = fields[4];
        track.file_offset =
            parse_integer<std::uint64_t>(descriptor_name, line_number, "Dateioffset", fields[5]);
        track.descriptor_line = line_number;
        if (type != 0u && type != 4u) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) + " besitzt einen ungueltigen Typ.");
        }
        track.type = static_cast<GdiTrackType>(type);
        if (!supported_sector_size(track.sector_size)) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) +
                     " besitzt ein unplausibles Sektorformat.");
        }
        const std::filesystem::path relative(track.file_name);
        if (relative.empty() || relative.is_absolute()) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) + " braucht einen relativen Dateipfad.");
        }
        track.resolved_path =
            std::filesystem::weakly_canonical(descriptor_directory / relative, canonical_error);
        if (canonical_error) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) +
                     " besitzt einen nicht aufloesbaren Dateipfad.");
        }
        const auto contained_path = track.resolved_path.lexically_relative(descriptor_directory);
        if (contained_path.empty() || contained_path.is_absolute() ||
            *contained_path.begin() == "..") {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) +
                     " verlaesst das GDI-Descriptorverzeichnis.");
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(track.resolved_path, error) || error) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) + " verweist auf eine fehlende Datei.");
        }
        const auto file_size = std::filesystem::file_size(track.resolved_path, error);
        if (error || track.file_offset > file_size ||
            (file_size - track.file_offset) % track.sector_size != 0u) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) +
                     " widerspricht Dateigroesse oder Offset.");
        }
        track.sector_count = (file_size - track.file_offset) / track.sector_size;
        if (track.sector_count == 0u) {
            fail(descriptor_name,
                 line_number,
                 "Track " + std::to_string(track.number) + " ist leer.");
        }
        if (!result.tracks.empty() && (track.number <= result.tracks.back().number ||
                                       track.lba <= result.tracks.back().lba)) {
            fail(descriptor_name,
                 line_number,
                 "Tracknummern und LBAs muessen streng aufsteigend sein.");
        }
        result.tracks.push_back(std::move(track));
    }
    if (result.tracks.size() != expected_count) {
        fail(descriptor_name,
             line_number + 1u,
             "deklarierte und gelesene Trackanzahl stimmen nicht ueberein.");
    }
    for (std::size_t index = 0u; index < result.tracks.size(); ++index) {
        if (result.tracks[index].number != index + 1u) {
            fail(descriptor_name,
                 result.tracks[index].descriptor_line,
                 "Tracknummern muessen bei 1 beginnen und lueckenlos sein.");
        }
        if (index != 0u) {
            const auto& previous = result.tracks[index - 1u];
            const auto previous_end =
                static_cast<std::uint64_t>(previous.lba) + previous.sector_count;
            if (previous_end > result.tracks[index].lba) {
                fail(descriptor_name,
                     result.tracks[index].descriptor_line,
                     "Track-LBA-Bereiche ueberlappen sich.");
            }
        }
    }
    return result;
}

namespace {
void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ull;
}

template <typename Integer> void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    for (std::size_t index = 0u; index < sizeof(Integer); ++index) {
        hash_byte(hash, static_cast<std::uint8_t>(value));
        value >>= 8u;
    }
}

std::string format_identity(const std::uint64_t hash) {
    std::ostringstream output;
    output << "gdi-fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}
} // namespace

std::shared_ptr<GdiDiscSource> GdiDiscSource::open(const std::filesystem::path& descriptor_path) {
    return std::shared_ptr<GdiDiscSource>(new GdiDiscSource(parse_gdi_descriptor(descriptor_path)));
}

GdiDiscSource::GdiDiscSource(GdiDescriptor descriptor) : descriptor_(std::move(descriptor)) {
    std::uint64_t hash = 14695981039346656037ull;
    std::uint64_t maximum_end_lba = 0u;
    track_sources_.reserve(descriptor_.tracks.size());
    for (const auto& track : descriptor_.tracks) {
        auto source = std::make_shared<FileDiscSource>(track.resolved_path,
                                                       "gdi-track:" + std::to_string(track.number));
        hash_integer(hash, track.number);
        hash_integer(hash, track.lba);
        hash_integer(hash, static_cast<std::uint8_t>(track.type));
        hash_integer(hash, track.sector_size);
        hash_integer(hash, track.file_offset);
        hash_integer(hash, track.sector_count);
        const auto active_size = track.sector_count * track.sector_size;
        if (track.file_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::out_of_range("GDI-Trackoffset ist fuer den Hoststream zu gross.");
        }
        std::ifstream hash_input(track.resolved_path, std::ios::binary);
        if (!hash_input) {
            throw std::runtime_error("GDI-Track konnte nicht read-only geoeffnet werden.");
        }
        hash_input.seekg(static_cast<std::streamoff>(track.file_offset));
        std::vector<std::uint8_t> buffer(65536u);
        std::uint64_t consumed = 0u;
        while (consumed < active_size) {
            const auto length = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), active_size - consumed));
            hash_input.read(reinterpret_cast<char*>(buffer.data()),
                            static_cast<std::streamsize>(length));
            if (hash_input.gcount() != static_cast<std::streamsize>(length)) {
                throw std::runtime_error("GDI-Trackhash erhielt einen unvollstaendigen Read.");
            }
            for (std::size_t index = 0u; index < length; ++index) {
                hash_byte(hash, buffer[index]);
            }
            consumed += length;
        }
        const auto end_lba = static_cast<std::uint64_t>(track.lba) + track.sector_count;
        maximum_end_lba = std::max(maximum_end_lba, end_lba);
        track_sources_.push_back(std::move(source));
    }
    if (maximum_end_lba > std::numeric_limits<std::uint64_t>::max() / 2048u) {
        throw std::out_of_range("GDI-logische Groesse laeuft ueber.");
    }
    logical_size_ = maximum_end_lba * 2048u;
    identity_ = format_identity(hash);
}

std::uint64_t GdiDiscSource::size() const noexcept {
    return logical_size_;
}
const std::string& GdiDiscSource::identity() const noexcept {
    return identity_;
}
const GdiDescriptor& GdiDiscSource::descriptor() const noexcept {
    return descriptor_;
}

std::uint32_t GdiDiscSource::primary_data_lba() const {
    const auto track =
        std::find_if(descriptor_.tracks.rbegin(),
                     descriptor_.tracks.rend(),
                     [](const GdiTrack& value) { return value.type == GdiTrackType::Data; });
    if (track == descriptor_.tracks.rend()) {
        throw std::runtime_error("GDI-Quelle besitzt keinen Datentrack.");
    }
    return track->lba;
}

std::vector<std::uint8_t> GdiDiscSource::read_raw_sector(const std::uint32_t track_number,
                                                         const std::uint64_t sector_index) const {
    const auto track =
        std::find_if(descriptor_.tracks.begin(),
                     descriptor_.tracks.end(),
                     [&](const GdiTrack& value) { return value.number == track_number; });
    if (track == descriptor_.tracks.end()) {
        throw std::out_of_range("GDI-Track wurde nicht gefunden.");
    }
    if (sector_index >= track->sector_count) {
        throw std::out_of_range("GDI-Sektor liegt ausserhalb des Tracks.");
    }
    const auto index = static_cast<std::size_t>(track - descriptor_.tracks.begin());
    return track_sources_[index]->read(track->file_offset + sector_index * track->sector_size,
                                       track->sector_size);
}

std::vector<std::uint8_t> GdiDiscSource::read_data_sector(const std::uint64_t absolute_lba) const {
    const auto track = std::find_if(
        descriptor_.tracks.begin(), descriptor_.tracks.end(), [&](const GdiTrack& value) {
            return absolute_lba >= value.lba && absolute_lba - value.lba < value.sector_count;
        });
    if (track == descriptor_.tracks.end()) {
        throw std::out_of_range("GDI-LBA liegt in keinem Track.");
    }
    if (track->type != GdiTrackType::Data) {
        throw std::runtime_error("GDI-Audiotrack besitzt keine 2048-Byte-Datensicht.");
    }
    const auto raw = read_raw_sector(track->number, absolute_lba - track->lba);
    std::size_t payload_offset = 0u;
    if (track->sector_size == 2336u) {
        payload_offset = 8u;
    } else if (track->sector_size == 2352u || track->sector_size == 2448u) {
        if (raw.size() < 24u || (raw[15u] != 1u && raw[15u] != 2u)) {
            throw std::runtime_error("GDI-Raw-Datensektor besitzt keinen unterstuetzten Mode.");
        }
        payload_offset = raw[15u] == 1u ? 16u : 24u;
    }
    if (payload_offset + 2048u > raw.size()) {
        throw std::runtime_error("GDI-Datensektor ist zu kurz fuer 2048 Nutzbytes.");
    }
    return {raw.begin() + static_cast<std::ptrdiff_t>(payload_offset),
            raw.begin() + static_cast<std::ptrdiff_t>(payload_offset + 2048u)};
}

void GdiDiscSource::read(const std::uint64_t offset,
                         const std::span<std::uint8_t> destination) const {
    if (offset > logical_size_ ||
        static_cast<std::uint64_t>(destination.size()) > logical_size_ - offset) {
        throw std::out_of_range("GDI-Lesezugriff liegt ausserhalb der logischen Disc.");
    }
    std::size_t written = 0u;
    while (written < destination.size()) {
        const auto logical_offset = offset + written;
        const auto sector = logical_offset / 2048u;
        const auto in_sector = static_cast<std::size_t>(logical_offset % 2048u);
        const auto data = read_data_sector(sector);
        const auto length = std::min(destination.size() - written, data.size() - in_sector);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(in_sector),
                    length,
                    destination.begin() + static_cast<std::ptrdiff_t>(written));
        written += length;
    }
}

} // namespace katana::runtime
