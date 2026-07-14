#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::io {

enum class SegmentKind {
    Unknown,
    Code,
    Data
};

struct SegmentPermissions {
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

struct ImageSegment {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint64_t file_offset = 0;
    std::uint64_t memory_size = 0;
    SegmentKind kind = SegmentKind::Unknown;
    SegmentPermissions permissions;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::uint64_t end_address() const noexcept;
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t width = 1u) const noexcept;
    [[nodiscard]] std::optional<std::size_t> byte_offset(std::uint32_t address) const noexcept;
};

class ExecutableImage {
public:
    explicit ExecutableImage(std::filesystem::path source_path = {});

    void add_segment(ImageSegment segment);
    void add_entry_point(std::uint32_t address);

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    [[nodiscard]] std::span<const ImageSegment> segments() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> entry_points() const noexcept;
    [[nodiscard]] const ImageSegment* find_segment(
        std::uint32_t address,
        std::size_t width = 1u
    ) const noexcept;

private:
    std::filesystem::path source_path_;
    std::vector<ImageSegment> segments_;
    std::vector<std::uint32_t> entry_points_;
};

[[nodiscard]] const char* segment_kind_name(SegmentKind kind) noexcept;

}
