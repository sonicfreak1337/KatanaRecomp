#pragma once

#include "katana/codegen/cache.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace katana::codegen {

struct ProjectArtifact {
    std::filesystem::path relative_path;
    std::string content;
};

struct ProjectWriteOptions {
    std::size_t parallel_jobs = 1u;
    CodegenCache* cache = nullptr;
    std::string cache_key;
};

struct ProjectWriteResult {
    std::vector<std::filesystem::path> written_files;
    std::vector<std::filesystem::path> removed_files;
    std::size_t cache_hits = 0u;
    std::size_t cache_misses = 0u;
};

[[nodiscard]] ProjectWriteResult write_codegen_project(const std::filesystem::path& output_root,
                                                       std::vector<ProjectArtifact> artifacts,
                                                       const ProjectWriteOptions& options = {});

} // namespace katana::codegen
