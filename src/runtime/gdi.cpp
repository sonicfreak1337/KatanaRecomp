#include "katana/runtime/gdi.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
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

    const auto descriptor_provenance =
        katana::io::capture_input_provenance("gdi-descriptor", descriptor_path);
    GdiDescriptor result{descriptor_name,
                         descriptor_provenance.local_path,
                         descriptor_provenance.size,
                         descriptor_provenance.sha256,
                         {}};
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

std::shared_ptr<GdiDiscSource> GdiDiscSource::open(const std::filesystem::path& descriptor_path) {
    return std::shared_ptr<GdiDiscSource>(new GdiDiscSource(parse_gdi_descriptor(descriptor_path)));
}

GdiDiscSource::GdiDiscSource(GdiDescriptor descriptor) : descriptor_(std::move(descriptor)) {
    std::ostringstream identity_material;
    std::uint64_t maximum_end_lba = 0u;
    track_sources_.reserve(descriptor_.tracks.size());
    track_number_index_.reserve(descriptor_.tracks.size());
    for (std::size_t index = 0u; index < descriptor_.tracks.size(); ++index) {
        auto& track = descriptor_.tracks[index];
        auto source = std::make_shared<FileDiscSource>(track.resolved_path,
                                                       "gdi-track:" + std::to_string(track.number));
        track.sha256 = katana::io::capture_input_provenance(
                           "gdi-track-" + std::to_string(track.number), track.resolved_path)
                           .sha256;
        identity_material << track.number << ':' << track.lba << ':'
                          << static_cast<unsigned>(track.type) << ':' << track.sector_size << ':'
                          << track.file_offset << ':' << track.sector_count << ':' << track.sha256
                          << ';';
        const auto end_lba = static_cast<std::uint64_t>(track.lba) + track.sector_count;
        maximum_end_lba = std::max(maximum_end_lba, end_lba);
        track_number_index_.emplace(track.number, index);
        track_sources_.push_back(std::move(source));
    }
    if (maximum_end_lba > std::numeric_limits<std::uint64_t>::max() / 2048u) {
        throw std::out_of_range("GDI-logische Groesse laeuft ueber.");
    }
    logical_size_ = maximum_end_lba * 2048u;
    identity_ = "gdi-sha256:" + katana::io::sha256_bytes(identity_material.str());
    io_counters_.persistent_track_opens = track_sources_.size();
    sector_cache_.reserve(sector_cache_capacity_);
    sector_cache_order_.reserve(sector_cache_capacity_);
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
    return read_raw_sectors(track_number, sector_index, 1u);
}

std::vector<std::uint8_t> GdiDiscSource::read_raw_sectors(const std::uint32_t track_number,
                                                          const std::uint64_t first_sector,
                                                          const std::size_t count) const {
    const auto found = track_number_index_.find(track_number);
    if (found == track_number_index_.end()) {
        throw std::out_of_range("GDI-Track wurde nicht gefunden.");
    }
    const auto& track = descriptor_.tracks[found->second];
    if (count == 0u || first_sector > track.sector_count ||
        count > track.sector_count - first_sector ||
        count > std::numeric_limits<std::size_t>::max() / track.sector_size) {
        throw std::out_of_range("GDI-Sektorbatch liegt ausserhalb des Tracks.");
    }
    ++io_counters_.raw_read_operations;
    return track_sources_[found->second]->read(track.file_offset + first_sector * track.sector_size,
                                               count * track.sector_size);
}

std::size_t GdiDiscSource::track_index_for_lba(const std::uint64_t absolute_lba) const {
    ++io_counters_.track_lookups;
    const auto next =
        std::upper_bound(descriptor_.tracks.begin(),
                         descriptor_.tracks.end(),
                         absolute_lba,
                         [](const auto lba, const GdiTrack& track) { return lba < track.lba; });
    if (next == descriptor_.tracks.begin()) {
        throw std::out_of_range("GDI-LBA liegt in keinem Track.");
    }
    const auto index = static_cast<std::size_t>(std::prev(next) - descriptor_.tracks.begin());
    const auto& track = descriptor_.tracks[index];
    if (absolute_lba - track.lba >= track.sector_count) {
        throw std::out_of_range("GDI-LBA liegt in keinem Track.");
    }
    return index;
}

std::vector<std::uint8_t> GdiDiscSource::decode_track_sectors(const std::size_t track_index,
                                                              const std::uint64_t first_sector,
                                                              const std::size_t count) const {
    const auto& track = descriptor_.tracks[track_index];
    if (track.type != GdiTrackType::Data) {
        throw std::runtime_error("GDI-Audiotrack besitzt keine 2048-Byte-Datensicht.");
    }
    if (count == 0u || first_sector > track.sector_count ||
        count > track.sector_count - first_sector ||
        count > std::numeric_limits<std::size_t>::max() / track.sector_size) {
        throw std::out_of_range("GDI-Sektorbatch liegt ausserhalb des Tracks.");
    }
    std::vector<std::uint8_t> raw(count * track.sector_size);
    track_sources_[track_index]->read(track.file_offset + first_sector * track.sector_size, raw);
    ++io_counters_.raw_read_operations;
    if (track.sector_size == 2048u) {
        io_counters_.decoded_sectors += count;
        return raw;
    }
    std::vector<std::uint8_t> result(count * 2048u);
    for (std::size_t sector = 0u; sector < count; ++sector) {
        const auto raw_offset = sector * track.sector_size;
        std::size_t payload_offset = 0u;
        if (track.sector_size == 2336u) {
            payload_offset = 8u;
        } else if (track.sector_size == 2352u || track.sector_size == 2448u) {
            if (raw[raw_offset + 15u] != 1u && raw[raw_offset + 15u] != 2u) {
                throw std::runtime_error("GDI-Raw-Datensektor besitzt keinen unterstuetzten Mode.");
            }
            payload_offset = raw[raw_offset + 15u] == 1u ? 16u : 24u;
        }
        if (payload_offset + 2048u > track.sector_size) {
            throw std::runtime_error("GDI-Raw-Datensektor besitzt keinen unterstuetzten Mode.");
        }
        std::copy_n(raw.begin() + static_cast<std::ptrdiff_t>(raw_offset + payload_offset),
                    2048u,
                    result.begin() + static_cast<std::ptrdiff_t>(sector * 2048u));
    }
    io_counters_.decoded_sectors += count;
    return result;
}

std::vector<std::uint8_t> GdiDiscSource::read_data_sectors(const std::uint64_t absolute_lba,
                                                           const std::size_t count) const {
    if (count == 0u) return {};
    if (count > std::numeric_limits<std::size_t>::max() / 2048u) {
        throw std::out_of_range("GDI-Datensektorbatch ist fuer den Host zu gross.");
    }
    std::vector<std::uint8_t> result;
    result.reserve(count * 2048u);
    auto lba = absolute_lba;
    auto remaining = count;
    while (remaining != 0u) {
        const auto track_index = track_index_for_lba(lba);
        const auto& track = descriptor_.tracks[track_index];
        const auto within_track = lba - track.lba;
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, track.sector_count - within_track));
        bool all_cached = cache_mode_ == DiscCacheMode::Enabled;
        if (all_cached) {
            const std::lock_guard lock(cache_mutex_);
            for (std::size_t index = 0u; index < chunk; ++index) {
                if (!sector_cache_.contains(lba + index)) {
                    all_cached = false;
                    break;
                }
            }
            if (all_cached) {
                for (std::size_t index = 0u; index < chunk; ++index) {
                    const auto& sector = sector_cache_.at(lba + index);
                    result.insert(result.end(), sector.begin(), sector.end());
                    ++io_counters_.sector_cache_hits;
                }
            }
        }
        if (!all_cached) {
            auto decoded = decode_track_sectors(track_index, within_track, chunk);
            if (cache_mode_ == DiscCacheMode::Enabled) {
                const std::lock_guard lock(cache_mutex_);
                io_counters_.sector_cache_misses += chunk;
                for (std::size_t index = 0u; index < chunk; ++index) {
                    const auto sector_lba = lba + index;
                    if (!sector_cache_.contains(sector_lba)) {
                        if (sector_cache_.size() == sector_cache_capacity_) {
                            sector_cache_.erase(sector_cache_order_.front());
                            sector_cache_order_.erase(sector_cache_order_.begin());
                            ++io_counters_.sector_cache_evictions;
                        }
                        sector_cache_order_.push_back(sector_lba);
                        sector_cache_.emplace(
                            sector_lba,
                            std::vector<std::uint8_t>(
                                decoded.begin() + static_cast<std::ptrdiff_t>(index * 2048u),
                                decoded.begin() +
                                    static_cast<std::ptrdiff_t>((index + 1u) * 2048u)));
                    }
                }
            }
            result.insert(result.end(), decoded.begin(), decoded.end());
        }
        lba += chunk;
        remaining -= chunk;
    }
    return result;
}

std::vector<std::uint8_t> GdiDiscSource::read_data_sector(const std::uint64_t absolute_lba) const {
    return read_data_sectors(absolute_lba, 1u);
}

void GdiDiscSource::read(const std::uint64_t offset,
                         const std::span<std::uint8_t> destination) const {
    if (offset > logical_size_ ||
        static_cast<std::uint64_t>(destination.size()) > logical_size_ - offset) {
        throw std::out_of_range("GDI-Lesezugriff liegt ausserhalb der logischen Disc.");
    }
    if (destination.empty()) return;
    const auto first_sector = offset / 2048u;
    const auto in_sector = static_cast<std::size_t>(offset % 2048u);
    const auto full_sectors = destination.size() / 2048u;
    const auto remainder = destination.size() % 2048u;
    const auto sector_count = full_sectors + (in_sector + remainder + 2047u) / 2048u;
    const auto data = read_data_sectors(first_sector, sector_count);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(in_sector),
                destination.size(),
                destination.begin());
}

void GdiDiscSource::set_cache_mode(const DiscCacheMode mode) noexcept {
    cache_mode_ = mode;
    sector_cache_.clear();
    sector_cache_order_.clear();
}

DiscCacheMode GdiDiscSource::cache_mode() const noexcept {
    return cache_mode_;
}

const GdiIoCounters& GdiDiscSource::io_counters() const noexcept {
    return io_counters_;
}

void GdiDiscSource::reset_io_counters() const noexcept {
    io_counters_ = {};
    io_counters_.persistent_track_opens = track_sources_.size();
}

std::size_t GdiDiscSource::sector_cache_size() const noexcept {
    return sector_cache_.size();
}

std::size_t GdiDiscSource::sector_cache_capacity() const noexcept {
    return sector_cache_capacity_;
}

} // namespace katana::runtime
