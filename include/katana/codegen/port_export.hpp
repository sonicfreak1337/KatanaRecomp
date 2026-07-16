#pragma once

#include "katana/codegen/partition.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t port_project_contract_version = 1u;

struct PortExportOptions {
    std::string target_name;
    std::string tool_version;
    PartitionOptions partition_options;
    std::filesystem::path forbidden_source_root;
};

struct PortExportResult {
    std::filesystem::path output_root;
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
    std::size_t generated_files = 0u;
    std::size_t removed_files = 0u;
    std::vector<std::string> checkpoints;
};

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

} // namespace katana::codegen
