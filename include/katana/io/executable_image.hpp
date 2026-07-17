#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::io {

enum class SegmentKind { Unknown, Code, Data };

enum class SymbolKind { Unknown, Function, Object };

enum class SymbolBinding { Local, Global, Weak, Unknown };

enum class RelocationKind { None, Absolute32, PcRelative32, Unsupported };

enum class GuestCallAbi { Unknown, SuperHC };

enum class InitialSnapshotPolicy { ImmutableOnly, EntryPointStraightLine };

struct ImageSymbol {
    std::string name;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    SymbolKind kind = SymbolKind::Unknown;
    SymbolBinding binding = SymbolBinding::Unknown;
};

struct ImageRelocation {
    std::uint32_t address = 0;
    std::uint32_t raw_type = 0;
    RelocationKind kind = RelocationKind::Unsupported;
    std::string symbol_name;
    std::uint32_t symbol_address = 0;
    std::int32_t addend = 0;
    std::optional<std::uint32_t> applied_value;
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
    void add_relocation(ImageRelocation relocation);
    void set_guest_call_abi(GuestCallAbi abi) noexcept;
    void set_initial_snapshot_policy(InitialSnapshotPolicy policy) noexcept;

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    [[nodiscard]] std::span<const ImageSegment> segments() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> entry_points() const noexcept;
    [[nodiscard]] std::span<const ImageSymbol> symbols() const noexcept;
    [[nodiscard]] std::span<const ImageRelocation> relocations() const noexcept;
    [[nodiscard]] GuestCallAbi guest_call_abi() const noexcept;
    [[nodiscard]] InitialSnapshotPolicy initial_snapshot_policy() const noexcept;
    [[nodiscard]] const ImageSymbol* find_symbol(std::string_view name) const noexcept;
    [[nodiscard]] const ImageSegment* find_segment(std::uint32_t address,
                                                   std::size_t width = 1u) const noexcept;
    [[nodiscard]] std::uint32_t read_u32_le(std::uint32_t address) const;
    void write_u32_le(std::uint32_t address, std::uint32_t value);

  private:
    std::filesystem::path source_path_;
    std::vector<ImageSegment> segments_;
    std::vector<std::uint32_t> entry_points_;
    std::vector<ImageSymbol> symbols_;
    std::vector<ImageRelocation> relocations_;
    GuestCallAbi guest_call_abi_ = GuestCallAbi::Unknown;
    InitialSnapshotPolicy initial_snapshot_policy_ = InitialSnapshotPolicy::ImmutableOnly;
};

[[nodiscard]] const char* segment_kind_name(SegmentKind kind) noexcept;
[[nodiscard]] const char* symbol_kind_name(SymbolKind kind) noexcept;
[[nodiscard]] const char* relocation_kind_name(RelocationKind kind) noexcept;

} // namespace katana::io
