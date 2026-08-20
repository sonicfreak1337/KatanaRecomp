#pragma once

#include "katana/runtime/block_abi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t dispatch_diagnostic_schema_version = 2u;

enum class DispatchResolutionOrigin : std::uint8_t {
    StaticProof,
    Override,
    TableLookup,
    RuntimeOnly,
    InlineCache,
    Fallback
};
enum class DispatchAliasOrigin : std::uint8_t { None, ExactVirtual, CanonicalPhysical };
enum class DispatchFallbackReason : std::uint8_t {
    None,
    UnknownOpcode,
    UnresolvedControlFlow,
    DynamicCode,
    ManifestDenied
};
enum class DispatchFallbackAction : std::uint8_t { None, Abort, Diagnose, Interpreter, UserHook };
enum class DispatchDiagnosticError : std::uint8_t {
    None,
    UnknownCode,
    UnknownTarget,
    UnmappedMemory,
    FirmwareDenied,
    Misaligned,
    InvalidBoundary,
    PermissionDenied,
    ProvenNonCode,
    MaterializationBudget,
    ByteIdentityMismatch,
    MissingAot,
    GenerationMismatch,
    RelocationMismatch,
    StaleBlock,
    AotTemplateMismatch
};

struct DispatchDiagnosticEvent {
    std::uint32_t callsite = 0u;
    std::uint32_t source_virtual = 0u;
    std::uint32_t source_physical = 0u;
    std::optional<std::uint32_t> virtual_target;
    std::optional<std::uint32_t> canonical_target;
    std::uint32_t pr = 0u;
    BlockEndKind block_end = BlockEndKind::DynamicBranch;
    DispatchResolutionOrigin origin = DispatchResolutionOrigin::TableLookup;
    DispatchAliasOrigin alias_origin = DispatchAliasOrigin::None;
    DispatchFallbackReason fallback_reason = DispatchFallbackReason::None;
    DispatchFallbackAction fallback_action = DispatchFallbackAction::None;
    std::uint64_t guest_instructions = 0u;
    std::uint32_t exit_pc = 0u;
    DispatchDiagnosticError error = DispatchDiagnosticError::None;
    std::uint64_t occurrences = 1u;
};

inline constexpr std::size_t dispatch_diagnostic_snapshot_capacity = 16u;
inline constexpr std::size_t dispatch_diagnostic_snapshot_line_capacity = 16'384u;

// Fixed crash-path projection of DispatchDiagnosticEvent. Optional addresses are
// represented by explicit presence bytes so the snapshot stays trivially copyable.
struct DispatchDiagnosticSnapshotEvent {
    std::uint32_t callsite = 0u;
    std::uint32_t source_virtual = 0u;
    std::uint32_t source_physical = 0u;
    std::uint32_t virtual_target = 0u;
    std::uint32_t canonical_target = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t exit_pc = 0u;
    std::uint64_t guest_instructions = 0u;
    std::uint64_t occurrences = 0u;
    DispatchDiagnosticError error = DispatchDiagnosticError::None;
    BlockEndKind block_end = BlockEndKind::DynamicBranch;
    DispatchResolutionOrigin origin = DispatchResolutionOrigin::TableLookup;
    DispatchAliasOrigin alias_origin = DispatchAliasOrigin::None;
    DispatchFallbackReason fallback_reason = DispatchFallbackReason::None;
    DispatchFallbackAction fallback_action = DispatchFallbackAction::None;
    std::uint8_t has_virtual_target = 0u;
    std::uint8_t has_canonical_target = 0u;
    std::uint8_t reserved[2u]{};
};

struct DispatchDiagnosticSnapshot {
    std::uint64_t total_occurrences = 0u;
    std::uint64_t dropped_unique_events = 0u;
    std::uint32_t event_count = 0u;
    std::uint32_t reserved = 0u;
    std::array<DispatchDiagnosticSnapshotEvent, dispatch_diagnostic_snapshot_capacity> events{};
};

struct DispatchDiagnosticSerializedLine {
    std::array<char, dispatch_diagnostic_snapshot_line_capacity> bytes{};
    std::uint32_t size = 0u;
    bool truncated = false;

    [[nodiscard]] std::string_view view() const noexcept {
        const auto bounded_size = size < bytes.size() ? size : bytes.size();
        return std::string_view(bytes.data(), bounded_size);
    }
};

class DispatchDiagnosticRecorder final {
  public:
    static constexpr std::size_t default_capacity = 1024u;

    explicit DispatchDiagnosticRecorder(std::size_t capacity = default_capacity);
    void record(DispatchDiagnosticEvent event);
    [[nodiscard]] bool try_record(DispatchDiagnosticEvent event) noexcept;
    void clear() noexcept;
    [[nodiscard]] const std::vector<DispatchDiagnosticEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t total_occurrences() const noexcept;
    [[nodiscard]] std::uint64_t dropped_unique_events() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    // Copies at most dispatch_diagnostic_snapshot_capacity events without allocation,
    // sorting, locking, or callbacks. The normal dynamic recorder remains unchanged.
    void capture_crash_snapshot(DispatchDiagnosticSnapshot& snapshot) const noexcept;
    [[nodiscard]] std::string serialize_json() const;
    [[nodiscard]] std::string serialize_hotspots_json(std::size_t limit = 16u) const;

  private:
    std::vector<DispatchDiagnosticEvent> events_;
    std::unordered_multimap<std::uint64_t, std::size_t> event_index_;
    std::size_t capacity_ = default_capacity;
    std::uint64_t total_occurrences_ = 0u;
    std::uint64_t dropped_unique_events_ = 0u;
};

[[nodiscard]] const char* dispatch_resolution_origin_name(DispatchResolutionOrigin value) noexcept;
[[nodiscard]] const char* dispatch_block_end_name(BlockEndKind value) noexcept;
[[nodiscard]] const char* dispatch_alias_origin_name(DispatchAliasOrigin value) noexcept;
[[nodiscard]] const char* dispatch_fallback_reason_name(DispatchFallbackReason value) noexcept;
[[nodiscard]] const char* dispatch_fallback_action_name(DispatchFallbackAction value) noexcept;
[[nodiscard]] const char* dispatch_diagnostic_error_name(DispatchDiagnosticError value) noexcept;

// Bounded JSON projection for a crash path. It never allocates and never throws;
// callers can inspect `truncated` before writing the fixed char buffer.
[[nodiscard]] DispatchDiagnosticSerializedLine
serialize_dispatch_diagnostic_snapshot_json(
    const DispatchDiagnosticSnapshot& snapshot) noexcept;

static_assert(std::is_standard_layout_v<DispatchDiagnosticSnapshotEvent>);
static_assert(std::is_trivially_copyable_v<DispatchDiagnosticSnapshotEvent>);
static_assert(std::is_standard_layout_v<DispatchDiagnosticSnapshot>);
static_assert(std::is_trivially_copyable_v<DispatchDiagnosticSnapshot>);
static_assert(std::is_standard_layout_v<DispatchDiagnosticSerializedLine>);
static_assert(std::is_trivially_copyable_v<DispatchDiagnosticSerializedLine>);

} // namespace katana::runtime
