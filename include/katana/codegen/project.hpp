#pragma once

#include "katana/codegen/cache.hpp"
#include "katana/progress.hpp"

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
    katana::ProgressReporter progress;
};

struct ProjectWriteResult {
    std::vector<std::filesystem::path> written_files;
    std::vector<std::filesystem::path> removed_files;
    std::size_t cache_hits = 0u;
    std::size_t cache_misses = 0u;
};

// Replaces content in an already authenticated generated project without
// changing its file set or build graph. Every existing manifest entry must
// still carry the exact file binding published by write_codegen_project, and
// each replacement names the expected previous content identity. This is a
// narrow downstream-consumer refresh primitive; it cannot authorize new AOT
// sources, remove stale files, or recover an untrusted/corrupt generation.
struct ProjectArtifactReplacement {
    std::filesystem::path relative_path;
    std::string expected_sha256;
    std::string content;
};

[[nodiscard]] ProjectWriteResult write_codegen_project(const std::filesystem::path& output_root,
                                                       std::vector<ProjectArtifact> artifacts,
                                                       const ProjectWriteOptions& options = {});

[[nodiscard]] ProjectWriteResult rewrite_codegen_project_artifacts(
    const std::filesystem::path& output_root,
    std::vector<ProjectArtifactReplacement> replacements);

} // namespace katana::codegen
