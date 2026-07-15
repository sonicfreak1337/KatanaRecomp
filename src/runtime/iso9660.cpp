#include "katana/runtime/iso9660.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {
std::uint32_t read_le32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::uint32_t read_be32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u) |
        static_cast<std::uint32_t>(bytes[offset + 3u]);
}

std::string normalize_name(std::string name) {
    const auto version = name.find(';');
    if (version != std::string::npos) { name.erase(version); }
    std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return name;
}

Iso9660Entry parse_record(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto length = static_cast<std::size_t>(bytes[offset]);
    if (length < 34u || length > bytes.size() - offset) {
        throw std::runtime_error("Ungueltiger ISO9660-Directory-Record.");
    }
    const auto extent_le = read_le32(bytes, offset + 2u);
    const auto extent_be = read_be32(bytes, offset + 6u);
    const auto size_le = read_le32(bytes, offset + 10u);
    const auto size_be = read_be32(bytes, offset + 14u);
    if (extent_le != extent_be || size_le != size_be) {
        throw std::runtime_error("ISO9660-Both-Endian-Felder widersprechen sich.");
    }
    const auto name_length = static_cast<std::size_t>(bytes[offset + 32u]);
    if (33u + name_length > length) {
        throw std::runtime_error("ISO9660-Dateiname liegt ausserhalb des Directory-Records.");
    }
    std::string name(
        reinterpret_cast<const char*>(bytes.data() + offset + 33u),
        name_length
    );
    return {
        normalize_name(std::move(name)),
        extent_le,
        size_le,
        (bytes[offset + 25u] & 0x02u) != 0u
    };
}

std::uint64_t sector_offset(
    const std::uint32_t volume_start_lba,
    const std::uint32_t relative_lba,
    const std::uint32_t sector_size
) {
    const auto absolute_lba = static_cast<std::uint64_t>(volume_start_lba) + relative_lba;
    if (absolute_lba > std::numeric_limits<std::uint64_t>::max() / sector_size) {
        throw std::out_of_range("ISO9660-Sektoroffset laeuft ueber.");
    }
    return absolute_lba * sector_size;
}
}

Iso9660Filesystem::Iso9660Filesystem(
    std::shared_ptr<const DiscSource> source,
    const std::uint32_t sector_size,
    const std::uint32_t volume_start_lba,
    const std::optional<std::uint32_t> extent_lba_bias
) : source_(std::move(source)), sector_size_(sector_size), volume_start_lba_(volume_start_lba),
    extent_lba_bias_(extent_lba_bias.value_or(volume_start_lba)) {
    if (!source_) { throw std::invalid_argument("ISO9660 braucht eine Disc-Quelle."); }
    if (sector_size_ < 2048u) { throw std::invalid_argument("ISO9660-Sektorgroesse ist kleiner als 2048 Byte."); }
    const auto descriptor = source_->read(
        sector_offset(volume_start_lba_, 16u, sector_size_),
        sector_size_
    );
    if (descriptor[0] != 1u || std::string_view(
        reinterpret_cast<const char*>(descriptor.data() + 1u), 5u) != "CD001" || descriptor[6] != 1u) {
        throw std::runtime_error("ISO9660 Primary Volume Descriptor fehlt oder ist ungueltig.");
    }
    root_ = parse_record(descriptor, 156u);
    root_.name = "/";
    if (!root_.directory) { throw std::runtime_error("ISO9660-Rootrecord ist kein Verzeichnis."); }
}

std::vector<std::string> Iso9660Filesystem::split_path(const std::string_view path) {
    std::vector<std::string> result;
    std::size_t start = 0u;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') { ++start; }
        if (start == path.size()) { break; }
        const auto end = path.find('/', start);
        const auto length = end == std::string_view::npos ? path.size() - start : end - start;
        result.push_back(normalize_name(std::string(path.substr(start, length))));
        start = end == std::string_view::npos ? path.size() : end + 1u;
    }
    return result;
}

std::vector<Iso9660Entry> Iso9660Filesystem::read_directory(const Iso9660Entry& directory) const {
    if (!directory.directory) { throw std::invalid_argument("ISO9660-Pfad ist kein Verzeichnis."); }
    const auto bytes = source_->read(
        sector_offset(extent_lba_bias_, directory.lba, sector_size_),
        directory.size
    );
    std::vector<Iso9660Entry> result;
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        if (bytes[offset] == 0u) {
            const auto next = ((offset / sector_size_) + 1u) * sector_size_;
            if (next <= offset) { break; }
            offset = std::min(next, bytes.size());
            continue;
        }
        const auto entry = parse_record(bytes, offset);
        const auto length = bytes[offset];
        if (entry.name != std::string(1u, '\0') && entry.name != std::string(1u, '\1')) {
            result.push_back(entry);
        }
        offset += length;
    }
    return result;
}

Iso9660Entry Iso9660Filesystem::resolve(const std::string_view path) const {
    auto current = root_;
    for (const auto& component : split_path(path)) {
        const auto entries = read_directory(current);
        const auto match = std::find_if(entries.begin(), entries.end(), [&](const Iso9660Entry& entry) {
            return entry.name == component;
        });
        if (match == entries.end()) { throw std::out_of_range("ISO9660-Pfad wurde nicht gefunden."); }
        current = *match;
    }
    return current;
}

std::vector<Iso9660Entry> Iso9660Filesystem::list_directory(const std::string_view path) const {
    return read_directory(resolve(path));
}

std::vector<std::uint8_t> Iso9660Filesystem::read_file(const std::string_view path) const {
    const auto entry = resolve(path);
    if (entry.directory) { throw std::invalid_argument("ISO9660-Pfad bezeichnet ein Verzeichnis."); }
    return source_->read(
        sector_offset(extent_lba_bias_, entry.lba, sector_size_),
        entry.size
    );
}

} // namespace katana::runtime
