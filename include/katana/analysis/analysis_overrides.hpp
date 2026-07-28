#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace katana::analysis {

inline constexpr std::uint32_t analysis_directives_current_version = 2u;

enum class AnalysisDirectiveMode : std::uint8_t { Override, Hint };

struct FunctionOverride {
    std::uint32_t address = 0u;
    std::size_t line = 0u;
    // Zero preserves the historical entry-only directive. A non-zero even
    // size is an exact, committed function interval beginning at address.
    std::uint32_t size = 0u;
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
};

struct AnalysisOverrides {
    std::uint32_t version = analysis_directives_current_version;
    AnalysisDirectiveMode mode = AnalysisDirectiveMode::Override;
    std::filesystem::path source_path;
    std::vector<FunctionOverride> functions;
    std::vector<JumpOverride> jumps;
    std::vector<JumpTableOverride> jump_tables;
};

[[nodiscard]] const char* analysis_directive_mode_name(AnalysisDirectiveMode mode) noexcept;

[[nodiscard]] AnalysisOverrides parse_analysis_overrides(const std::filesystem::path& path);

} // namespace katana::analysis
