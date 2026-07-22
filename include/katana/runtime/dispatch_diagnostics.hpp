#pragma once

#include "katana/runtime/block_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t dispatch_diagnostic_schema_version = 1u;

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
    GenerationMismatch,
    RelocationMismatch,
    StaleBlock
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
    [[nodiscard]] std::string serialize_json() const;
    [[nodiscard]] std::string serialize_hotspots_json(std::size_t limit = 16u) const;

  private:
    std::vector<DispatchDiagnosticEvent> events_;
    std::size_t capacity_ = default_capacity;
    std::uint64_t total_occurrences_ = 0u;
    std::uint64_t dropped_unique_events_ = 0u;
};

[[nodiscard]] const char* dispatch_resolution_origin_name(DispatchResolutionOrigin value) noexcept;
[[nodiscard]] const char* dispatch_alias_origin_name(DispatchAliasOrigin value) noexcept;
[[nodiscard]] const char* dispatch_fallback_reason_name(DispatchFallbackReason value) noexcept;
[[nodiscard]] const char* dispatch_fallback_action_name(DispatchFallbackAction value) noexcept;
[[nodiscard]] const char* dispatch_diagnostic_error_name(DispatchDiagnosticError value) noexcept;

} // namespace katana::runtime
