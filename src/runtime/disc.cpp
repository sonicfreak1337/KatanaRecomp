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

} // namespace katana::runtime
