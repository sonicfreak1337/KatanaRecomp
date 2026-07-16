#include "katana/io/executable_image.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::io {
namespace {

constexpr std::uint64_t kAddressSpaceSize = 0x100000000ull;

bool ranges_overlap(const std::uint64_t first_begin,
                    const std::uint64_t first_end,
                    const std::uint64_t second_begin,
                    const std::uint64_t second_end) noexcept {
    return first_begin < second_end && second_begin < first_end;
}

} // namespace

std::uint64_t ImageSegment::end_address() const noexcept {
    return static_cast<std::uint64_t>(virtual_address) + memory_size;
}

bool ImageSegment::contains(const std::uint32_t address, const std::size_t width) const noexcept {
    if (width == 0u) {
        return false;
    }
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + static_cast<std::uint64_t>(width);
    return begin >= virtual_address && end >= begin && end <= end_address();
}

std::optional<std::size_t> ImageSegment::byte_offset(const std::uint32_t address) const noexcept {
    if (!contains(address)) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::uint64_t>(address) - virtual_address;
    if (offset >= bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(offset);
}

ExecutableImage::ExecutableImage(std::filesystem::path source_path)
    : source_path_(std::move(source_path)) {}

void ExecutableImage::add_segment(ImageSegment segment) {
    if (segment.name.empty()) {
        throw std::invalid_argument("Ein Image-Segment braucht einen Namen.");
    }
    if (segment.memory_size == 0u) {
        throw std::invalid_argument("Ein Image-Segment darf nicht leer sein.");
    }
    if (segment.memory_size < segment.bytes.size()) {
        throw std::invalid_argument(
            "Die Speichergroesse eines Segments ist kleiner als seine Dateidaten.");
    }
    if (segment.end_address() > kAddressSpaceSize) {
        throw std::out_of_range("Ein Image-Segment ueberschreitet den 32-Bit-Adressraum.");
    }
    if (segment.file_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes.size()) {
        throw std::out_of_range("Dateioffset und Segmentgroesse laufen ueber.");
    }

    for (const auto& existing : segments_) {
        if (ranges_overlap(segment.virtual_address,
                           segment.end_address(),
                           existing.virtual_address,
                           existing.end_address())) {
            throw std::invalid_argument("Image-Segmente ueberlappen sich: " + existing.name +
                                        " und " + segment.name + ".");
        }
    }

    segments_.push_back(std::move(segment));
    std::sort(segments_.begin(),
              segments_.end(),
              [](const ImageSegment& first, const ImageSegment& second) {
                  return first.virtual_address < second.virtual_address;
              });
}

void ExecutableImage::add_entry_point(const std::uint32_t address) {
    if (std::find(entry_points_.begin(), entry_points_.end(), address) != entry_points_.end()) {
        return;
    }
    entry_points_.push_back(address);
    std::sort(entry_points_.begin(), entry_points_.end());
}

void ExecutableImage::add_symbol(ImageSymbol symbol) {
    if (symbol.name.empty()) {
        throw std::invalid_argument("Ein Image-Symbol braucht einen Namen.");
    }
    if (find_symbol(symbol.name) != nullptr) {
        throw std::invalid_argument("Doppelter Symbolname: " + symbol.name + ".");
    }
    symbols_.push_back(std::move(symbol));
    std::sort(
        symbols_.begin(), symbols_.end(), [](const ImageSymbol& first, const ImageSymbol& second) {
            if (first.address != second.address) {
                return first.address < second.address;
            }
            return first.name < second.name;
        });
}

void ExecutableImage::add_relocation(ImageRelocation relocation) {
    relocations_.push_back(std::move(relocation));
    std::sort(relocations_.begin(),
              relocations_.end(),
              [](const ImageRelocation& first, const ImageRelocation& second) {
                  if (first.address != second.address) {
                      return first.address < second.address;
                  }
                  return first.raw_type < second.raw_type;
              });
}

const std::filesystem::path& ExecutableImage::source_path() const noexcept {
    return source_path_;
}

std::span<const ImageSegment> ExecutableImage::segments() const noexcept {
    return segments_;
}

std::span<const std::uint32_t> ExecutableImage::entry_points() const noexcept {
    return entry_points_;
}

std::span<const ImageSymbol> ExecutableImage::symbols() const noexcept {
    return symbols_;
}

std::span<const ImageRelocation> ExecutableImage::relocations() const noexcept {
    return relocations_;
}

const ImageSymbol* ExecutableImage::find_symbol(const std::string_view name) const noexcept {
    for (const auto& symbol : symbols_) {
        if (symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const ImageSegment* ExecutableImage::find_segment(const std::uint32_t address,
                                                  const std::size_t width) const noexcept {
    for (const auto& segment : segments_) {
        if (segment.contains(address, width)) {
            return &segment;
        }
    }
    return nullptr;
}

std::uint32_t ExecutableImage::read_u32_le(const std::uint32_t address) const {
    const auto* segment = find_segment(address, 4u);
    if (segment == nullptr) {
        throw std::out_of_range("32-Bit-Lesezugriff liegt ausserhalb der Image-Segmente.");
    }
    const auto offset = segment->byte_offset(address);
    if (!offset.has_value() || segment->bytes.size() < 4u || *offset > segment->bytes.size() - 4u) {
        throw std::out_of_range("32-Bit-Lesezugriff liegt ausserhalb der committed Segmentdaten.");
    }
    return static_cast<std::uint32_t>(segment->bytes[*offset]) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(segment->bytes[*offset + 3u]) << 24u);
}

void ExecutableImage::write_u32_le(const std::uint32_t address, const std::uint32_t value) {
    for (auto& segment : segments_) {
        if (!segment.contains(address, 4u)) {
            continue;
        }
        const auto offset = segment.byte_offset(address);
        if (!offset.has_value() || segment.bytes.size() < 4u ||
            *offset > segment.bytes.size() - 4u) {
            throw std::out_of_range(
                "32-Bit-Schreibzugriff liegt ausserhalb der committed Segmentdaten.");
        }
        for (std::size_t index = 0; index < 4u; ++index) {
            segment.bytes[*offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        }
        return;
    }
    throw std::out_of_range("32-Bit-Schreibzugriff liegt ausserhalb der Image-Segmente.");
}

const char* segment_kind_name(const SegmentKind kind) noexcept {
    switch (kind) {
    case SegmentKind::Unknown:
        return "unknown";
    case SegmentKind::Code:
        return "code";
    case SegmentKind::Data:
        return "data";
    }
    return "unknown";
}

const char* symbol_kind_name(const SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::Unknown:
        return "unknown";
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Object:
        return "object";
    }
    return "unknown";
}

const char* relocation_kind_name(const RelocationKind kind) noexcept {
    switch (kind) {
    case RelocationKind::None:
        return "none";
    case RelocationKind::Absolute32:
        return "absolute32";
    case RelocationKind::PcRelative32:
        return "pc-relative32";
    case RelocationKind::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

} // namespace katana::io
