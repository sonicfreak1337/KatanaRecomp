#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace katana::analysis {

struct JumpOverride {
    std::uint32_t instruction_address = 0u;
    std::uint32_t target = 0u;
};

struct JumpTableOverride {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::size_t entry_count = 0u;
};

struct AnalysisOverrides {
    std::uint32_t version = 1u;
    std::vector<std::uint32_t> function_entries;
    std::vector<JumpOverride> jumps;
    std::vector<JumpTableOverride> jump_tables;
};

[[nodiscard]] AnalysisOverrides parse_analysis_overrides(
    const std::filesystem::path& path
);

}
