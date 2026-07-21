#include "katana/runtime/packed_disc.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace katana::runtime {
namespace {

constexpr std::array<std::uint8_t, 8u> pack_magic{'K', 'A', 'T', 'D', 'I', 'S', 'C', 0u};
constexpr std::size_t header_size = 192u;
constexpr std::size_t track_entry_size = 112u;
constexpr std::size_t chunk_entry_size = 72u;
constexpr std::size_t metadata_hash_offset = 144u;
constexpr std::uint32_t feature_raw_sectors = 1u << 0u;
constexpr std::uint32_t feature_chunk_sha256 = 1u << 1u;

void require_range(const std::size_t offset, const std::size_t length, const std::size_t size) {
    if (offset > size || length > size - offset)
        throw std::runtime_error("Katana-Disc-Pack-Metadaten sind abgeschnitten.");
}

void put_u32(std::vector<std::uint8_t>& bytes,
             const std::size_t offset,
             const std::uint32_t value) {
    require_range(offset, 4u, bytes.size());
    for (std::size_t index = 0u; index < 4u; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
}

void put_u64(std::vector<std::uint8_t>& bytes,
             const std::size_t offset,
             const std::uint64_t value) {
    require_range(offset, 8u, bytes.size());
    for (std::size_t index = 0u; index < 8u; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
}

std::uint32_t get_u32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || 4u > bytes.size() - offset)
        throw std::runtime_error("Katana-Disc-Pack-Metadaten sind abgeschnitten.");
    std::uint32_t result = 0u;
    for (std::size_t index = 0u; index < 4u; ++index)
        result |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8u);
    return result;
}

std::uint64_t get_u64(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || 8u > bytes.size() - offset)
        throw std::runtime_error("Katana-Disc-Pack-Metadaten sind abgeschnitten.");
    std::uint64_t result = 0u;
    for (std::size_t index = 0u; index < 8u; ++index)
        result |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8u);
    return result;
}

std::uint8_t hex_digit(const char value) {
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
    throw std::invalid_argument("SHA-256-Text ist ungueltig.");
}

std::array<std::uint8_t, 32u> decode_sha256(const std::string_view value) {
    if (value.size() != 64u) throw std::invalid_argument("SHA-256-Text ist ungueltig.");
    std::array<std::uint8_t, 32u> result{};
    for (std::size_t index = 0u; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>((hex_digit(value[index * 2u]) << 4u) |
                                                  hex_digit(value[index * 2u + 1u]));
    return result;
}

std::string encode_sha256(const std::span<const std::uint8_t> value) {
    if (value.size() != 32u) throw std::invalid_argument("SHA-256-Bytes sind ungueltig.");
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64u, '0');
    for (std::size_t index = 0u; index < value.size(); ++index) {
        result[index * 2u] = digits[value[index] >> 4u];
        result[index * 2u + 1u] = digits[value[index] & 0x0Fu];
    }
    return result;
}

void put_sha256(std::vector<std::uint8_t>& bytes,
                const std::size_t offset,
                const std::string_view value) {
    require_range(offset, 32u, bytes.size());
    const auto decoded = decode_sha256(value);
    std::copy(decoded.begin(), decoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::string hash_bytes(const std::span<const std::uint8_t> bytes) {
    return katana::io::sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

PackedDiscPayloadKind payload_kind(const GdiTrack& track) {
    if (track.type == GdiTrackType::Audio) return PackedDiscPayloadKind::NotData;
    return track.sector_size == 2352u || track.sector_size == 2448u
               ? PackedDiscPayloadKind::SectorMode
               : PackedDiscPayloadKind::Fixed;
}

bool supported_sector_size(const std::uint32_t value) noexcept {
    return value == 2048u || value == 2336u || value == 2352u || value == 2448u;
}

bool valid_track_payload(const GdiTrackType type,
                         const std::uint32_t sector_size,
                         const PackedDiscPayloadKind kind,
                         const std::uint32_t offset) noexcept {
    if (!supported_sector_size(sector_size)) return false;
    if (type == GdiTrackType::Audio) return kind == PackedDiscPayloadKind::NotData && offset == 0u;
    if (type != GdiTrackType::Data) return false;
    if (sector_size == 2048u) return kind == PackedDiscPayloadKind::Fixed && offset == 0u;
    if (sector_size == 2336u) return kind == PackedDiscPayloadKind::Fixed && offset == 8u;
    return (sector_size == 2352u || sector_size == 2448u) &&
           kind == PackedDiscPayloadKind::SectorMode && offset == 0u;
}

std::uint32_t payload_offset(const GdiTrack& track) {
    if (track.type == GdiTrackType::Audio) return 0u;
    return track.sector_size == 2336u ? 8u : 0u;
}

void atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::filesystem::filesystem_error(
            "Katana-Disc-Pack konnte nicht atomar veroeffentlicht werden.",
            source,
            destination,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
#else
    std::filesystem::rename(source, destination);
#endif
}

} // namespace

struct PackedDiscSource::Chunk {
    std::uint32_t track_index = 0u;
    std::uint32_t sector_count = 0u;
    std::uint64_t first_sector = 0u;
    std::uint64_t data_offset = 0u;
    std::uint64_t stored_size = 0u;
    std::uint64_t raw_size = 0u;
    std::string sha256;
};

PackedDiscSource::~PackedDiscSource() = default;

std::shared_ptr<PackedDiscSource> PackedDiscSource::open(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        throw std::invalid_argument("Katana-Disc-Pack ist keine lesbare regulaere Datei.");
    const auto file_size = std::filesystem::file_size(path, error);
    if (error || file_size < header_size)
        throw std::runtime_error("Katana-Disc-Pack ist abgeschnitten.");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Katana-Disc-Pack konnte nicht read-only geoeffnet werden.");
    std::vector<std::uint8_t> header(header_size);
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input || !std::equal(pack_magic.begin(), pack_magic.end(), header.begin()))
        throw std::runtime_error("Katana-Disc-Pack besitzt keine gueltige Kennung.");
    const auto version = get_u32(header, 8u);
    if (version != packed_disc_format_version || get_u32(header, 12u) != header_size)
        throw std::runtime_error("Katana-Disc-Pack-Formatversion wird nicht unterstuetzt.");
    const auto features = get_u32(header, 16u);
    if ((features & (feature_raw_sectors | feature_chunk_sha256)) !=
        (feature_raw_sectors | feature_chunk_sha256))
        throw std::runtime_error(
            "Katana-Disc-Pack verliert erforderliche Raw- oder Integritaetsdaten.");
    const auto track_count = get_u32(header, 20u);
    const auto chunk_count = get_u64(header, 24u);
    const auto track_offset = get_u64(header, 32u);
    const auto chunk_offset = get_u64(header, 40u);
    const auto data_offset = get_u64(header, 48u);
    if (track_count == 0u || track_offset != header_size ||
        track_count > (std::numeric_limits<std::size_t>::max() - header_size) / track_entry_size ||
        chunk_count > std::numeric_limits<std::size_t>::max() / chunk_entry_size)
        throw std::runtime_error("Katana-Disc-Pack besitzt unplausible Tabellen.");
    const auto expected_chunk_offset = header_size + track_count * track_entry_size;
    const auto expected_data_offset = expected_chunk_offset + chunk_count * chunk_entry_size;
    if (chunk_offset != expected_chunk_offset || data_offset != expected_data_offset ||
        data_offset > file_size || data_offset > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Katana-Disc-Pack-Tabellen sind inkonsistent.");
    std::vector<std::uint8_t> metadata(static_cast<std::size_t>(data_offset));
    input.clear();
    input.seekg(0);
    input.read(reinterpret_cast<char*>(metadata.data()),
               static_cast<std::streamsize>(metadata.size()));
    if (!input) throw std::runtime_error("Katana-Disc-Pack-Metadaten sind abgeschnitten.");
    const auto stored_metadata_hash =
        encode_sha256(std::span<const std::uint8_t>(metadata).subspan(metadata_hash_offset, 32u));
    std::fill_n(metadata.begin() + static_cast<std::ptrdiff_t>(metadata_hash_offset),
                32u,
                std::uint8_t{0u});
    if (hash_bytes(metadata) != stored_metadata_hash)
        throw std::runtime_error("Katana-Disc-Pack-Metadatenintegritaet ist verletzt.");

    PackedDiscInfo info;
    info.format_version = version;
    info.logical_size = get_u64(header, 56u);
    info.packed_sectors = get_u64(header, 64u);
    info.uncompressed_size = get_u64(header, 72u);
    info.pack_size = file_size;
    info.chunk_count = chunk_count;
    info.content_identity = encode_sha256(std::span<const std::uint8_t>(header).subspan(80u, 32u));
    info.job_generation = encode_sha256(std::span<const std::uint8_t>(header).subspan(112u, 32u));
    info.tracks.reserve(track_count);
    std::uint64_t maximum_end_lba = 0u;
    std::uint64_t counted_sectors = 0u;
    for (std::size_t index = 0u; index < track_count; ++index) {
        const auto offset = header_size + index * track_entry_size;
        PackedDiscTrack track;
        track.number = get_u32(metadata, offset);
        track.lba = get_u32(metadata, offset + 4u);
        const auto type = get_u32(metadata, offset + 8u);
        track.sector_size = get_u32(metadata, offset + 12u);
        const auto encoded_payload_kind = get_u32(metadata, offset + 16u);
        if (encoded_payload_kind > static_cast<std::uint32_t>(PackedDiscPayloadKind::SectorMode))
            throw std::runtime_error("Katana-Disc-Pack-Payloadtyp ist unbekannt.");
        track.payload_kind = static_cast<PackedDiscPayloadKind>(encoded_payload_kind);
        track.payload_offset = get_u32(metadata, offset + 20u);
        track.session = get_u32(metadata, offset + 24u);
        track.sector_count = get_u64(metadata, offset + 32u);
        track.first_chunk = get_u64(metadata, offset + 40u);
        track.chunk_count = get_u64(metadata, offset + 48u);
        track.integrity_sha256 =
            encode_sha256(std::span<const std::uint8_t>(metadata).subspan(offset + 64u, 32u));
        if ((type != 0u && type != 4u) || track.number != index + 1u || track.sector_count == 0u ||
            track.session == 0u || track.first_chunk > chunk_count ||
            track.chunk_count > chunk_count - track.first_chunk)
            throw std::runtime_error("Katana-Disc-Pack-Tracktabelle ist ungueltig.");
        track.type = static_cast<GdiTrackType>(type);
        if (!valid_track_payload(
                track.type, track.sector_size, track.payload_kind, track.payload_offset))
            throw std::runtime_error(
                "Katana-Disc-Pack-Track besitzt ungueltige Sektor-/Payloadmetadaten.");
        if (index != 0u && track.lba <= info.tracks.back().lba)
            throw std::runtime_error("Katana-Disc-Pack-LBAs sind nicht streng aufsteigend.");
        const auto end_lba = static_cast<std::uint64_t>(track.lba) + track.sector_count;
        if (end_lba < track.lba ||
            (index != 0u &&
             static_cast<std::uint64_t>(info.tracks.back().lba) + info.tracks.back().sector_count >
                 track.lba))
            throw std::runtime_error("Katana-Disc-Pack-Trackbereiche ueberlappen sich.");
        maximum_end_lba = std::max(maximum_end_lba, end_lba);
        counted_sectors += track.sector_count;
        info.tracks.push_back(std::move(track));
    }
    if (maximum_end_lba > std::numeric_limits<std::uint64_t>::max() / 2048u ||
        info.logical_size != maximum_end_lba * 2048u || counted_sectors != info.packed_sectors)
        throw std::runtime_error("Katana-Disc-Pack besitzt eine inkonsistente logische Groesse.");

    std::vector<Chunk> chunks;
    chunks.reserve(static_cast<std::size_t>(chunk_count));
    for (std::size_t index = 0u; index < chunk_count; ++index) {
        const auto offset = static_cast<std::size_t>(chunk_offset) + index * chunk_entry_size;
        Chunk chunk;
        chunk.track_index = get_u32(metadata, offset);
        chunk.sector_count = get_u32(metadata, offset + 4u);
        chunk.first_sector = get_u64(metadata, offset + 8u);
        chunk.data_offset = get_u64(metadata, offset + 16u);
        chunk.stored_size = get_u64(metadata, offset + 24u);
        chunk.raw_size = get_u64(metadata, offset + 32u);
        chunk.sha256 =
            encode_sha256(std::span<const std::uint8_t>(metadata).subspan(offset + 40u, 32u));
        if (chunk.track_index >= info.tracks.size() || chunk.sector_count == 0u ||
            chunk.sector_count > packed_disc_chunk_sectors || chunk.stored_size != chunk.raw_size ||
            chunk.raw_size != static_cast<std::uint64_t>(chunk.sector_count) *
                                  info.tracks[chunk.track_index].sector_size ||
            chunk.data_offset < data_offset || chunk.data_offset > file_size ||
            chunk.stored_size > file_size - chunk.data_offset)
            throw std::runtime_error("Katana-Disc-Pack-Chunkindex ist ungueltig.");
        chunks.push_back(std::move(chunk));
    }
    for (std::size_t track_index = 0u; track_index < info.tracks.size(); ++track_index) {
        const auto& track = info.tracks[track_index];
        std::uint64_t expected_sector = 0u;
        for (std::uint64_t local = 0u; local < track.chunk_count; ++local) {
            const auto& chunk = chunks[static_cast<std::size_t>(track.first_chunk + local)];
            if (chunk.track_index != track_index || chunk.first_sector != expected_sector)
                throw std::runtime_error("Katana-Disc-Pack-Chunkfolge besitzt eine Luecke.");
            expected_sector += chunk.sector_count;
        }
        if (expected_sector != track.sector_count)
            throw std::runtime_error("Katana-Disc-Pack-Chunkfolge ist unvollstaendig.");
    }
    return std::shared_ptr<PackedDiscSource>(
        new PackedDiscSource(path, std::move(info), std::move(chunks), file_size));
}

PackedDiscSource::PackedDiscSource(std::filesystem::path path,
                                   PackedDiscInfo info,
                                   std::vector<Chunk> chunks,
                                   const std::uint64_t file_size)
    : path_(std::move(path)), info_(std::move(info)), chunks_(std::move(chunks)),
      identity_("packed-disc-sha256:" + info_.content_identity), file_size_(file_size) {
    stream_.open(path_, std::ios::binary);
    if (!stream_)
        throw std::runtime_error(
            "Katana-Disc-Pack konnte nicht dauerhaft read-only geoeffnet werden.");
    for (std::size_t index = 0u; index < info_.tracks.size(); ++index)
        track_number_index_.emplace(info_.tracks[index].number, index);
    chunk_cache_.reserve(chunk_cache_capacity_);
    chunk_cache_order_.reserve(chunk_cache_capacity_);
}

std::uint64_t PackedDiscSource::size() const noexcept {
    return info_.logical_size;
}
const std::string& PackedDiscSource::identity() const noexcept {
    return identity_;
}

std::vector<DiscTrackLayout> PackedDiscSource::layout() const {
    std::vector<DiscTrackLayout> result;
    result.reserve(info_.tracks.size());
    for (const auto& track : info_.tracks) {
        result.push_back({track.number,
                          track.lba,
                          track.type == GdiTrackType::Data ? DiscTrackKind::Data
                                                          : DiscTrackKind::Audio,
                          track.sector_size,
                          track.sector_count,
                          track.session});
    }
    return result;
}
const PackedDiscInfo& PackedDiscSource::info() const noexcept {
    return info_;
}

std::uint32_t PackedDiscSource::primary_data_lba() const {
    const auto track =
        std::find_if(info_.tracks.rbegin(), info_.tracks.rend(), [](const auto& value) {
            return value.type == GdiTrackType::Data;
        });
    if (track == info_.tracks.rend())
        throw std::runtime_error("Katana-Disc-Pack besitzt keinen Datentrack.");
    return track->lba;
}

std::size_t PackedDiscSource::track_index_for_lba(const std::uint64_t absolute_lba) const {
    const auto next =
        std::upper_bound(info_.tracks.begin(),
                         info_.tracks.end(),
                         absolute_lba,
                         [](const auto lba, const auto& track) { return lba < track.lba; });
    if (next == info_.tracks.begin())
        throw std::out_of_range("Katana-Disc-Pack-LBA liegt in keinem Track.");
    const auto index = static_cast<std::size_t>(std::prev(next) - info_.tracks.begin());
    if (absolute_lba - info_.tracks[index].lba >= info_.tracks[index].sector_count)
        throw std::out_of_range("Katana-Disc-Pack-LBA liegt in keinem Track.");
    return index;
}

std::vector<std::uint8_t> PackedDiscSource::load_chunk(const std::size_t chunk_index) const {
    if (chunk_index >= chunks_.size())
        throw std::out_of_range("Katana-Disc-Pack-Chunk wurde nicht gefunden.");
    {
        const std::lock_guard lock(cache_mutex_);
        const auto cached = chunk_cache_.find(chunk_index);
        if (cached != chunk_cache_.end()) return cached->second;
    }
    const auto& chunk = chunks_[chunk_index];
    if (chunk.stored_size > std::numeric_limits<std::size_t>::max())
        throw std::out_of_range("Katana-Disc-Pack-Chunk ist fuer den Host zu gross.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(chunk.stored_size));
    {
        const std::lock_guard lock(stream_mutex_);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(chunk.data_offset));
        stream_.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream_ || stream_.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw std::runtime_error("Katana-Disc-Pack-Chunk fehlt oder ist abgeschnitten.");
    }
    if (hash_bytes(bytes) != chunk.sha256)
        throw std::runtime_error("Katana-Disc-Pack-Chunkintegritaet ist verletzt.");
    {
        const std::lock_guard lock(cache_mutex_);
        if (!chunk_cache_.contains(chunk_index)) {
            if (chunk_cache_.size() == chunk_cache_capacity_) {
                chunk_cache_.erase(chunk_cache_order_.front());
                chunk_cache_order_.erase(chunk_cache_order_.begin());
            }
            chunk_cache_order_.push_back(chunk_index);
            chunk_cache_.emplace(chunk_index, bytes);
        }
    }
    return bytes;
}

std::vector<std::uint8_t> PackedDiscSource::read_raw_sectors(const std::uint32_t track_number,
                                                             const std::uint64_t first_sector,
                                                             const std::size_t count) const {
    const auto found = track_number_index_.find(track_number);
    if (found == track_number_index_.end())
        throw std::out_of_range("Katana-Disc-Pack-Track wurde nicht gefunden.");
    const auto& track = info_.tracks[found->second];
    if (count == 0u || first_sector > track.sector_count ||
        count > track.sector_count - first_sector ||
        count > std::numeric_limits<std::size_t>::max() / track.sector_size)
        throw std::out_of_range("Katana-Disc-Pack-Sektorbatch liegt ausserhalb des Tracks.");
    std::vector<std::uint8_t> result;
    result.reserve(count * track.sector_size);
    auto sector = first_sector;
    auto remaining = count;
    while (remaining != 0u) {
        const auto local_chunk = sector / packed_disc_chunk_sectors;
        if (local_chunk >= track.chunk_count)
            throw std::runtime_error("Katana-Disc-Pack-Chunkfolge ist unvollstaendig.");
        const auto chunk_index = static_cast<std::size_t>(track.first_chunk + local_chunk);
        const auto& chunk = chunks_[chunk_index];
        if (sector < chunk.first_sector || sector >= chunk.first_sector + chunk.sector_count)
            throw std::runtime_error("Katana-Disc-Pack-Chunkindex passt nicht zum Sektor.");
        const auto within = static_cast<std::size_t>(sector - chunk.first_sector);
        const auto take = std::min<std::size_t>(remaining, chunk.sector_count - within);
        const auto bytes = load_chunk(chunk_index);
        const auto first_byte = within * track.sector_size;
        const auto byte_count = take * track.sector_size;
        result.insert(result.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(first_byte),
                      bytes.begin() + static_cast<std::ptrdiff_t>(first_byte + byte_count));
        sector += take;
        remaining -= take;
    }
    return result;
}

std::vector<std::uint8_t>
PackedDiscSource::read_raw_sector(const std::uint32_t track_number,
                                  const std::uint64_t sector_index) const {
    return read_raw_sectors(track_number, sector_index, 1u);
}

std::vector<std::uint8_t> PackedDiscSource::read_data_sectors(const std::uint64_t absolute_lba,
                                                              const std::size_t count) const {
    if (count == 0u) return {};
    if (count > std::numeric_limits<std::size_t>::max() / 2048u)
        throw std::out_of_range("Katana-Disc-Pack-Datensektorbatch ist fuer den Host zu gross.");
    std::vector<std::uint8_t> result;
    result.reserve(count * 2048u);
    auto lba = absolute_lba;
    auto remaining = count;
    while (remaining != 0u) {
        const auto track_index = track_index_for_lba(lba);
        const auto& track = info_.tracks[track_index];
        if (track.type != GdiTrackType::Data)
            throw std::runtime_error("Katana-Disc-Pack-Audiotrack besitzt keine Datensicht.");
        const auto within_track = lba - track.lba;
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, track.sector_count - within_track));
        const auto raw = read_raw_sectors(track.number, within_track, take);
        for (std::size_t sector = 0u; sector < take; ++sector) {
            const auto raw_offset = sector * track.sector_size;
            std::size_t offset = track.payload_offset;
            if (track.payload_kind == PackedDiscPayloadKind::SectorMode) {
                if (track.sector_size <= 15u || raw_offset > raw.size() ||
                    track.sector_size > raw.size() - raw_offset)
                    throw std::runtime_error("Katana-Disc-Pack-Raw-Datensektor ist abgeschnitten.");
                const auto mode = raw[raw_offset + 15u];
                if (mode != 1u && mode != 2u)
                    throw std::runtime_error(
                        "Katana-Disc-Pack-Raw-Datensektor besitzt keinen unterstuetzten Mode.");
                offset = mode == 1u ? 16u : 24u;
            } else if (track.payload_kind != PackedDiscPayloadKind::Fixed) {
                throw std::runtime_error("Katana-Disc-Pack-Datentrack besitzt keine Datensicht.");
            }
            if (offset + 2048u > track.sector_size)
                throw std::runtime_error("Katana-Disc-Pack-Datensektor ist nicht darstellbar.");
            result.insert(result.end(),
                          raw.begin() + static_cast<std::ptrdiff_t>(raw_offset + offset),
                          raw.begin() + static_cast<std::ptrdiff_t>(raw_offset + offset + 2048u));
        }
        lba += take;
        remaining -= take;
    }
    return result;
}

void PackedDiscSource::read(const std::uint64_t offset,
                            const std::span<std::uint8_t> destination) const {
    if (offset > info_.logical_size ||
        static_cast<std::uint64_t>(destination.size()) > info_.logical_size - offset)
        throw std::out_of_range("Katana-Disc-Pack-Lesezugriff liegt ausserhalb der Disc.");
    if (destination.empty()) return;
    const auto first_sector = offset / 2048u;
    const auto in_sector = static_cast<std::size_t>(offset % 2048u);
    const auto sector_count = (in_sector + destination.size() + 2047u) / 2048u;
    const auto data = read_data_sectors(first_sector, sector_count);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(in_sector),
                destination.size(),
                destination.begin());
}

void PackedDiscSource::verify_all_chunks() const {
    for (std::size_t index = 0u; index < chunks_.size(); ++index)
        static_cast<void>(load_chunk(index));
}

std::size_t PackedDiscSource::chunk_cache_size() const noexcept {
    const std::lock_guard lock(cache_mutex_);
    return chunk_cache_.size();
}
std::size_t PackedDiscSource::chunk_cache_capacity() const noexcept {
    return chunk_cache_capacity_;
}

PackedDiscInfo write_packed_disc(const GdiDiscSource& source,
                                 const std::filesystem::path& destination,
                                 std::string job_generation) {
    if (destination.empty() || source.descriptor().tracks.empty())
        throw std::invalid_argument("Disc-Pack-Export braucht Quelle und Ausgabeziel.");
    try {
        static_cast<void>(decode_sha256(job_generation));
    } catch (const std::invalid_argument&) {
        job_generation = katana::io::sha256_bytes(job_generation);
    }
    std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination;
    temporary += ".katana-stage";
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (cleanup_error)
        throw std::runtime_error("Altes Disc-Pack-Staging konnte nicht entfernt werden.");
    struct StagingCleanup final {
        std::filesystem::path path;
        bool active = true;
        ~StagingCleanup() {
            if (!active) return;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } staging_cleanup{temporary};

    const auto& source_tracks = source.descriptor().tracks;
    std::uint64_t chunk_count = 0u;
    std::uint64_t packed_sectors = 0u;
    std::uint64_t uncompressed_size = 0u;
    std::uint64_t maximum_end_lba = 0u;
    for (const auto& track : source_tracks) {
        chunk_count +=
            (track.sector_count + packed_disc_chunk_sectors - 1u) / packed_disc_chunk_sectors;
        packed_sectors += track.sector_count;
        if (track.sector_count > std::numeric_limits<std::uint64_t>::max() / track.sector_size ||
            uncompressed_size >
                std::numeric_limits<std::uint64_t>::max() - track.sector_count * track.sector_size)
            throw std::out_of_range("Disc-Pack-Groesse laeuft ueber.");
        uncompressed_size += track.sector_count * track.sector_size;
        maximum_end_lba =
            std::max(maximum_end_lba, static_cast<std::uint64_t>(track.lba) + track.sector_count);
    }
    if (source_tracks.size() >
            (std::numeric_limits<std::size_t>::max() - header_size) / track_entry_size ||
        chunk_count > std::numeric_limits<std::size_t>::max() / chunk_entry_size)
        throw std::out_of_range("Disc-Pack-Metadaten sind fuer den Host zu gross.");
    const auto chunk_offset = header_size + source_tracks.size() * track_entry_size;
    const auto data_offset =
        chunk_offset + static_cast<std::size_t>(chunk_count) * chunk_entry_size;
    std::vector<std::uint8_t> metadata(data_offset, 0u);
    std::copy(pack_magic.begin(), pack_magic.end(), metadata.begin());
    put_u32(metadata, 8u, packed_disc_format_version);
    put_u32(metadata, 12u, header_size);
    put_u32(metadata, 16u, feature_raw_sectors | feature_chunk_sha256);
    put_u32(metadata, 20u, static_cast<std::uint32_t>(source_tracks.size()));
    put_u64(metadata, 24u, chunk_count);
    put_u64(metadata, 32u, header_size);
    put_u64(metadata, 40u, chunk_offset);
    put_u64(metadata, 48u, data_offset);
    put_u64(metadata, 56u, maximum_end_lba * 2048u);
    put_u64(metadata, 64u, packed_sectors);
    put_u64(metadata, 72u, uncompressed_size);
    put_sha256(metadata, 112u, job_generation);

    std::fstream output(temporary,
                        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("Disc-Pack-Staging konnte nicht geoeffnet werden.");
    output.write(reinterpret_cast<const char*>(metadata.data()),
                 static_cast<std::streamsize>(metadata.size()));
    if (!output) throw std::runtime_error("Disc-Pack-Metadaten konnten nicht reserviert werden.");

    PackedDiscInfo info;
    info.job_generation = std::move(job_generation);
    info.logical_size = maximum_end_lba * 2048u;
    info.packed_sectors = packed_sectors;
    info.uncompressed_size = uncompressed_size;
    info.chunk_count = chunk_count;
    info.tracks.reserve(source_tracks.size());
    std::uint64_t next_data_offset = data_offset;
    std::uint64_t next_chunk = 0u;
    std::uint32_t session = 1u;
    std::uint64_t previous_end_lba = 0u;
    for (std::size_t track_index = 0u; track_index < source_tracks.size(); ++track_index) {
        const auto& source_track = source_tracks[track_index];
        if (track_index != 0u && source_track.lba > previous_end_lba) ++session;
        previous_end_lba = static_cast<std::uint64_t>(source_track.lba) + source_track.sector_count;
        PackedDiscTrack track;
        track.number = source_track.number;
        track.lba = source_track.lba;
        track.type = source_track.type;
        track.sector_size = source_track.sector_size;
        track.payload_kind = payload_kind(source_track);
        track.payload_offset = payload_offset(source_track);
        track.session = session;
        track.sector_count = source_track.sector_count;
        track.first_chunk = next_chunk;
        track.chunk_count = (source_track.sector_count + packed_disc_chunk_sectors - 1u) /
                            packed_disc_chunk_sectors;
        std::ostringstream track_integrity;
        track_integrity << track.number << ':' << track.lba << ':'
                        << static_cast<unsigned>(track.type) << ':' << track.sector_size << ':'
                        << static_cast<unsigned>(track.payload_kind) << ':' << track.payload_offset
                        << ':' << track.session << ':' << track.sector_count << ';';
        std::uint64_t first_sector = 0u;
        while (first_sector < source_track.sector_count) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                packed_disc_chunk_sectors, source_track.sector_count - first_sector));
            const auto raw = source.read_raw_sectors(source_track.number, first_sector, count);
            const auto hash = hash_bytes(raw);
            const auto index_offset =
                chunk_offset + static_cast<std::size_t>(next_chunk) * chunk_entry_size;
            put_u32(metadata, index_offset, static_cast<std::uint32_t>(track_index));
            put_u32(metadata, index_offset + 4u, static_cast<std::uint32_t>(count));
            put_u64(metadata, index_offset + 8u, first_sector);
            put_u64(metadata, index_offset + 16u, next_data_offset);
            put_u64(metadata, index_offset + 24u, raw.size());
            put_u64(metadata, index_offset + 32u, raw.size());
            put_sha256(metadata, index_offset + 40u, hash);
            output.write(reinterpret_cast<const char*>(raw.data()),
                         static_cast<std::streamsize>(raw.size()));
            if (!output)
                throw std::runtime_error("Disc-Pack-Chunk konnte nicht geschrieben werden.");
            track_integrity << hash << ';';
            next_data_offset += raw.size();
            first_sector += count;
            ++next_chunk;
        }
        track.integrity_sha256 = katana::io::sha256_bytes(track_integrity.str());
        const auto offset = header_size + track_index * track_entry_size;
        put_u32(metadata, offset, track.number);
        put_u32(metadata, offset + 4u, track.lba);
        put_u32(metadata, offset + 8u, static_cast<std::uint32_t>(track.type));
        put_u32(metadata, offset + 12u, track.sector_size);
        put_u32(metadata, offset + 16u, static_cast<std::uint32_t>(track.payload_kind));
        put_u32(metadata, offset + 20u, track.payload_offset);
        put_u32(metadata, offset + 24u, track.session);
        put_u64(metadata, offset + 32u, track.sector_count);
        put_u64(metadata, offset + 40u, track.first_chunk);
        put_u64(metadata, offset + 48u, track.chunk_count);
        put_u64(metadata, offset + 56u, track.sector_count * track.sector_size);
        put_sha256(metadata, offset + 64u, track.integrity_sha256);
        info.tracks.push_back(std::move(track));
    }
    info.content_identity = gdi_content_identity(source.descriptor());
    put_sha256(metadata, 80u, info.content_identity);
    std::fill_n(metadata.begin() + static_cast<std::ptrdiff_t>(metadata_hash_offset),
                32u,
                std::uint8_t{0u});
    put_sha256(metadata, metadata_hash_offset, hash_bytes(metadata));
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(metadata.data()),
                 static_cast<std::streamsize>(metadata.size()));
    output.flush();
    if (!output) throw std::runtime_error("Disc-Pack-Metadaten konnten nicht finalisiert werden.");
    output.close();
    info.pack_size = std::filesystem::file_size(temporary);
    auto verification = PackedDiscSource::open(temporary);
    verification->verify_all_chunks();
    if (verification->info().content_identity != info.content_identity ||
        verification->info().job_generation != info.job_generation)
        throw std::runtime_error("Disc-Pack-Verifikation besitzt eine abweichende Identitaet.");
    verification.reset();
    atomic_replace(temporary, destination);
    staging_cleanup.active = false;
    return info;
}

std::string format_packed_disc_manifest(const PackedDiscInfo& info,
                                        const std::string_view pack_sha256,
                                        const std::string_view executable_sha256,
                                        const std::string_view executable_path,
                                        const std::uint64_t executable_size) {
    static_cast<void>(decode_sha256(pack_sha256));
    if (!executable_sha256.empty()) static_cast<void>(decode_sha256(executable_sha256));
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-packed-disc", "packed-disc-manifest");
    output << ",\"format_version\":" << info.format_version
           << ",\"job_generation\":" << katana::io::quote_json(info.job_generation)
           << ",\"content_identity\":" << katana::io::quote_json(info.content_identity)
           << ",\"logical_size\":" << info.logical_size
           << ",\"packed_sectors\":" << info.packed_sectors
           << ",\"uncompressed_size\":" << info.uncompressed_size
           << ",\"pack_size\":" << info.pack_size << ",\"chunk_count\":" << info.chunk_count
           << ",\"compression\":\"none\",\"subchannel_policy\":\"raw-2448-preserved\""
           << ",\"artifacts\":[{\"role\":\"packed_disc\",\"path\":\"game.katana-disc\""
           << ",\"size\":" << info.pack_size << ",\"format_version\":" << info.format_version
           << ",\"sha256\":" << katana::io::quote_json(pack_sha256)
           << ",\"job_generation\":" << katana::io::quote_json(info.job_generation) << '}';
    if (!executable_sha256.empty())
        output << ",{\"role\":\"host_executable\",\"path\":"
               << katana::io::quote_json(executable_path) << ",\"format_version\":1"
               << ",\"size\":" << executable_size
               << ",\"sha256\":" << katana::io::quote_json(executable_sha256)
               << ",\"job_generation\":" << katana::io::quote_json(info.job_generation) << '}';
    output << "],\"tracks\":[";
    for (std::size_t index = 0u; index < info.tracks.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& track = info.tracks[index];
        output << "{\"number\":" << track.number << ",\"lba\":" << track.lba << ",\"type\":\""
               << (track.type == GdiTrackType::Data ? "data" : "audio")
               << "\",\"sector_size\":" << track.sector_size
               << ",\"payload_kind\":" << static_cast<unsigned>(track.payload_kind)
               << ",\"payload_offset\":" << track.payload_offset
               << ",\"sector_count\":" << track.sector_count << ",\"session\":" << track.session
               << ",\"integrity_sha256\":" << katana::io::quote_json(track.integrity_sha256) << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::runtime
