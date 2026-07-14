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

enum class SymbolKind {
    Unknown,
    Function,
    Object
};

enum class SymbolBinding {
    Local,
    Global,
    Weak,
    Unknown
};

struct ImageSymbol {
    std::string name;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    SymbolKind kind = SymbolKind::Unknown;
    SymbolBinding binding = SymbolBinding::Unknown;
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
    void add_symbol(ImageSymbol symbol);

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    [[nodiscard]] std::span<const ImageSegment> segments() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> entry_points() const noexcept;
    [[nodiscard]] std::span<const ImageSymbol> symbols() const noexcept;
    [[nodiscard]] const ImageSymbol* find_symbol(std::string_view name) const noexcept;
    [[nodiscard]] const ImageSegment* find_segment(
        std::uint32_t address,
        std::size_t width = 1u
    ) const noexcept;

private:
    std::filesystem::path source_path_;
    std::vector<ImageSegment> segments_;
    std::vector<std::uint32_t> entry_points_;
    std::vector<ImageSymbol> symbols_;
};

[[nodiscard]] const char* segment_kind_name(SegmentKind kind) noexcept;
[[nodiscard]] const char* symbol_kind_name(SymbolKind kind) noexcept;

}
