#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/build_contract.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t port_project_contract_version =
    build_contract::port_project_contract_version;

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

struct PreparedPortProgram {
    const katana::io::ExecutableImage& image;
    const katana::analysis::ControlFlowAnalysisResult& analysis;
    std::span<const katana::ir::Function> program;
    std::span<const katana::io::InputProvenance> inputs;
    std::uint32_t entry_address = 0u;
    std::uint32_t boot_address = 0u;
    std::size_t boot_size = 0u;
    std::string_view project_identity;
    bool hle_bios_abi = false;
};

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const PreparedPortProgram& prepared,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

} // namespace katana::codegen
