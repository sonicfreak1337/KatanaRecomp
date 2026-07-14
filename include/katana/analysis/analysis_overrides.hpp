#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace katana::analysis {

struct FunctionOverride {
    std::uint32_t address = 0u;
    std::size_t line = 0u;
};

struct JumpOverride {
    std::uint32_t instruction_address = 0u;
    std::uint32_t target = 0u;
    std::size_t line = 0u;
};

struct JumpTableOverride {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::size_t entry_count = 0u;
    std::size_t line = 0u;
};

struct AnalysisOverrides {
    std::uint32_t version = 1u;
    std::filesystem::path source_path;
    std::vector<FunctionOverride> functions;
    std::vector<JumpOverride> jumps;
    std::vector<JumpTableOverride> jump_tables;
};

[[nodiscard]] AnalysisOverrides parse_analysis_overrides(
    const std::filesystem::path& path
);

}
