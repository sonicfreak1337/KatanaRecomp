#include "katana/io/executable_image.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::io {
namespace {

constexpr std::uint64_t kAddressSpaceSize = 0x100000000ull;

bool ranges_overlap(
    const std::uint64_t first_begin,
    const std::uint64_t first_end,
    const std::uint64_t second_begin,
    const std::uint64_t second_end
) noexcept {
    return first_begin < second_end && second_begin < first_end;
}

}

std::uint64_t ImageSegment::end_address() const noexcept {
    return static_cast<std::uint64_t>(virtual_address) + memory_size;
}

bool ImageSegment::contains(
    const std::uint32_t address,
    const std::size_t width
) const noexcept {
    if (width == 0u) {
        return false;
    }
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + static_cast<std::uint64_t>(width);
    return begin >= virtual_address && end >= begin && end <= end_address();
}

std::optional<std::size_t> ImageSegment::byte_offset(
    const std::uint32_t address
) const noexcept {
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
        throw std::invalid_argument("Die Speichergroesse eines Segments ist kleiner als seine Dateidaten.");
    }
    if (segment.end_address() > kAddressSpaceSize) {
        throw std::out_of_range("Ein Image-Segment ueberschreitet den 32-Bit-Adressraum.");
    }
    if (segment.file_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes.size()) {
        throw std::out_of_range("Dateioffset und Segmentgroesse laufen ueber.");
    }

    for (const auto& existing : segments_) {
        if (ranges_overlap(
                segment.virtual_address,
                segment.end_address(),
                existing.virtual_address,
                existing.end_address()
            )) {
            throw std::invalid_argument(
                "Image-Segmente ueberlappen sich: " + existing.name + " und " + segment.name + "."
            );
        }
    }

    segments_.push_back(std::move(segment));
    std::sort(
        segments_.begin(),
        segments_.end(),
        [](const ImageSegment& first, const ImageSegment& second) {
            return first.virtual_address < second.virtual_address;
        }
    );
}

void ExecutableImage::add_entry_point(const std::uint32_t address) {
    if (std::find(entry_points_.begin(), entry_points_.end(), address) != entry_points_.end()) {
        return;
    }
    entry_points_.push_back(address);
    std::sort(entry_points_.begin(), entry_points_.end());
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

const ImageSegment* ExecutableImage::find_segment(
    const std::uint32_t address,
    const std::size_t width
) const noexcept {
    for (const auto& segment : segments_) {
        if (segment.contains(address, width)) {
            return &segment;
        }
    }
    return nullptr;
}

const char* segment_kind_name(const SegmentKind kind) noexcept {
    switch (kind) {
        case SegmentKind::Unknown: return "unknown";
        case SegmentKind::Code: return "code";
        case SegmentKind::Data: return "data";
    }
    return "unknown";
}

}
