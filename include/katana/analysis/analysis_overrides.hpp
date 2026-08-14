#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace katana::analysis {

inline constexpr std::uint32_t analysis_directives_current_version = 5u;

enum class AnalysisDirectiveMode : std::uint8_t { Override, Hint };

struct FunctionOverride {
    std::uint32_t address = 0u;
    std::size_t line = 0u;
    // Zero preserves the historical entry-only directive. A non-zero even
    // size is an exact, committed function interval beginning at address.
    std::uint32_t size = 0u;
};

// An exact ownership/extent constraint which does not make the function an
// analysis root.  This keeps identity-bound metadata available to recursive,
// value and IR analysis without reviving otherwise unreachable code.
struct FunctionBoundaryOverride {
    std::uint32_t address = 0u;
    std::size_t line = 0u;
    std::uint32_t size = 0u;
};

// A non-root function-entry hint.  It cannot make code reachable on its own
// and supplies no extent.  Analyses may use it only to corroborate an
// independently proven semantic edge, such as an exact executable value
// flowing into a callback field that is later loaded by an indirect call.
struct FunctionEntryHintOverride {
    std::uint32_t address = 0u;
    std::size_t line = 0u;
};

struct JumpOverride {
    std::uint32_t instruction_address = 0u;
    std::uint32_t target = 0u;
    std::size_t line = 0u;
};

enum class JumpTableOverrideEncoding : std::uint8_t {
    Absolute32,
    SignedRelative16,
    SignedRelative32
};

enum class JumpTableOverrideTransfer : std::uint8_t {
    Inferred,
    Jump,
    Call
};

struct JumpTableOverride {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::size_t entry_count = 0u;
    std::size_t line = 0u;
    std::uint32_t entry_stride = sizeof(std::uint32_t);
    std::uint32_t relative_base = 0u;
    JumpTableOverrideEncoding encoding =
        JumpTableOverrideEncoding::Absolute32;
    JumpTableOverrideTransfer transfer =
        JumpTableOverrideTransfer::Inferred;
    // Edge-only metadata may remain inert when its dispatch is unreachable.
    // Once the dispatch is reached, all ordinary exact validation still
    // applies. Parsed `jump_table` directives remain required by default.
    bool require_dispatch = true;
};

struct AnalysisOverrides {
    std::uint32_t version = analysis_directives_current_version;
    AnalysisDirectiveMode mode = AnalysisDirectiveMode::Override;
    std::filesystem::path source_path;
    std::vector<FunctionOverride> functions;
    std::vector<FunctionBoundaryOverride> function_boundaries;
    std::vector<FunctionEntryHintOverride> function_entry_hints;
    std::vector<JumpOverride> jumps;
    std::vector<JumpTableOverride> jump_tables;
};

[[nodiscard]] const char* analysis_directive_mode_name(AnalysisDirectiveMode mode) noexcept;

[[nodiscard]] AnalysisOverrides parse_analysis_overrides(const std::filesystem::path& path);

} // namespace katana::analysis
