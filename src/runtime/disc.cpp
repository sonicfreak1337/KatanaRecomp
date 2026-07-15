#include "katana/runtime/disc.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {
void validate_identity(const std::string& identity) {
    if (identity.empty()) {
        throw std::invalid_argument("Eine Disc-Quelle braucht eine nichtleere semantische Identitaet.");
    }
}

void validate_range(
    const std::uint64_t source_size,
    const std::uint64_t offset,
    const std::size_t length
) {
    if (offset > source_size || static_cast<std::uint64_t>(length) > source_size - offset) {
        throw std::out_of_range("Disc-Lesezugriff liegt ausserhalb der Quelle.");
    }
}
}

std::vector<std::uint8_t> DiscSource::read(
    const std::uint64_t offset,
    const std::size_t length
) const {
    std::vector<std::uint8_t> result(length);
    read(offset, result);
    return result;
}

MemoryDiscSource::MemoryDiscSource(
    const std::span<const std::uint8_t> bytes,
    std::string identity
) : bytes_(bytes.begin(), bytes.end()), identity_(std::move(identity)) {
    validate_identity(identity_);
}

std::uint64_t MemoryDiscSource::size() const noexcept { return bytes_.size(); }
const std::string& MemoryDiscSource::identity() const noexcept { return identity_; }

void MemoryDiscSource::read(
    const std::uint64_t offset,
    const std::span<std::uint8_t> destination
) const {
    validate_range(size(), offset, destination.size());
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    std::copy(begin, begin + static_cast<std::ptrdiff_t>(destination.size()), destination.begin());
}

FileDiscSource::FileDiscSource(std::filesystem::path path, std::string identity)
    : path_(std::move(path)), identity_(std::move(identity)) {
    validate_identity(identity_);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error) {
        throw std::invalid_argument("Disc-Dateiquelle ist keine lesbare regulaere Datei.");
    }
    size_ = std::filesystem::file_size(path_, error);
    if (error) {
        throw std::runtime_error("Groesse der Disc-Dateiquelle konnte nicht gelesen werden.");
    }
}

std::uint64_t FileDiscSource::size() const noexcept { return size_; }
const std::string& FileDiscSource::identity() const noexcept { return identity_; }

void FileDiscSource::read(
    const std::uint64_t offset,
    const std::span<std::uint8_t> destination
) const {
    validate_range(size_, offset, destination.size());
    if (destination.empty()) { return; }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::out_of_range("Disc-Dateilesebereich ist fuer den Hoststream zu gross.");
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) { throw std::runtime_error("Disc-Dateiquelle konnte nicht read-only geoeffnet werden."); }
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(destination.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("Disc-Dateiquelle lieferte einen unvollstaendigen Read.");
    }
}

GdRomDrive::GdRomDrive(
    std::shared_ptr<const DiscSource> source,
    const std::uint32_t sector_size
) : source_(std::move(source)), sector_size_(sector_size) {
    if (!source_) { throw std::invalid_argument("GD-ROM-Laufwerk braucht eine Disc-Quelle."); }
    if (sector_size_ == 0u) { throw std::invalid_argument("GD-ROM-Sektorgroesse darf nicht null sein."); }
}

namespace {
void append_be32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24u));
    output.push_back(static_cast<std::uint8_t>(value >> 16u));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
}
}

GdRomResponse GdRomDrive::execute(const GdRomRequest& request) const {
    const auto sectors = source_->size() / sector_size_;
    if (request.command == GdRomCommand::TestUnitReady) {
        return {sectors == 0u ? GdRomStatus::NoMedia : GdRomStatus::Good, {}, 0u};
    }
    if (request.command == GdRomCommand::GetStatus) {
        return {GdRomStatus::Good, {
            static_cast<std::uint8_t>(sectors == 0u ? GdRomStatus::NoMedia : GdRomStatus::Good),
            0u, 0u, 0u
        }, 0u};
    }
    if (request.command == GdRomCommand::GetCapacity) {
        if (sectors == 0u) { return {GdRomStatus::NoMedia, {}, 0u}; }
        if (sectors - 1u > std::numeric_limits<std::uint32_t>::max()) {
            return {GdRomStatus::OutOfRange, {}, 0u};
        }
        GdRomResponse response;
        append_be32(response.data, static_cast<std::uint32_t>(sectors - 1u));
        append_be32(response.data, sector_size_);
        return response;
    }
    if (request.command != GdRomCommand::ReadSectors) {
        return {GdRomStatus::InvalidCommand, {}, 0u};
    }
    if (request.sector_count == 0u) {
        return {GdRomStatus::InvalidField, {}, 0u};
    }
    const auto lba = static_cast<std::uint64_t>(request.lba);
    const auto count = static_cast<std::uint64_t>(request.sector_count);
    if (lba >= sectors || count > sectors - lba) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
    const auto byte_count = count * sector_size_;
    const auto byte_offset = lba * sector_size_;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
    try {
        return {
            GdRomStatus::Good,
            source_->read(byte_offset, static_cast<std::size_t>(byte_count)),
            request.sector_count
        };
    } catch (const std::out_of_range&) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
}

std::uint32_t GdRomDrive::sector_size() const noexcept { return sector_size_; }

GdRomAsyncReader::GdRomAsyncReader(GdRomDrive drive, const GdRomTiming timing)
    : drive_(std::move(drive)), timing_(timing) {}

std::uint64_t GdRomAsyncReader::submit(const GdRomRequest& request) {
    const auto sectors = request.command == GdRomCommand::ReadSectors
        ? static_cast<std::uint64_t>(request.sector_count)
        : 0u;
    if (sectors != 0u && timing_.cycles_per_sector >
        (std::numeric_limits<std::uint64_t>::max() - timing_.command_latency) / sectors) {
        throw std::out_of_range("GD-ROM-Requestlatenz laeuft ueber.");
    }
    const auto duration = timing_.command_latency + sectors * timing_.cycles_per_sector;
    if (current_cycle_ > std::numeric_limits<std::uint64_t>::max() - duration) {
        throw std::out_of_range("GD-ROM-Fertigstellungszyklus laeuft ueber.");
    }
    if (next_request_id_ == 0u) { throw std::overflow_error("GD-ROM-Request-ID ist erschoepft."); }
    const auto id = next_request_id_++;
    pending_.push_back({id, current_cycle_ + duration, request});
    return id;
}

void GdRomAsyncReader::advance_to(const std::uint64_t cycle) {
    if (cycle < current_cycle_) { throw std::invalid_argument("GD-ROM-Zyklusuhr darf nicht rueckwaerts laufen."); }
    current_cycle_ = cycle;
    auto iterator = pending_.begin();
    while (iterator != pending_.end()) {
        if (iterator->ready_cycle > cycle) { ++iterator; continue; }
        completed_.push_back({iterator->request_id, iterator->ready_cycle, drive_.execute(iterator->request)});
        iterator = pending_.erase(iterator);
    }
    std::sort(completed_.begin(), completed_.end(), [](const auto& left, const auto& right) {
        if (left.ready_cycle != right.ready_cycle) { return left.ready_cycle < right.ready_cycle; }
        return left.request_id < right.request_id;
    });
}

std::optional<GdRomAsyncCompletion> GdRomAsyncReader::take_completed() {
    if (completed_.empty()) { return std::nullopt; }
    auto result = std::move(completed_.front());
    completed_.erase(completed_.begin());
    return result;
}

std::size_t GdRomAsyncReader::pending_count() const noexcept { return pending_.size(); }
std::uint64_t GdRomAsyncReader::current_cycle() const noexcept { return current_cycle_; }

} // namespace katana::runtime
