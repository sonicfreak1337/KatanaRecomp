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

using PortExportProgressCallback = void (*)(std::string_view phase);

struct PortExportOptions {
    std::string target_name;
    std::string tool_version;
    PartitionOptions partition_options;
    std::filesystem::path forbidden_source_root;
    bool diagnostic_partial = false;
    std::string console_profile = "japan-ntsc";
    PortExportProgressCallback progress_callback = nullptr;
};

struct PortExportResult {
    std::filesystem::path output_root;
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
    std::size_t generated_files = 0u;
    std::size_t removed_files = 0u;
    std::filesystem::path disc_install_recipe;
    std::string job_generation;
    std::string content_identity;
    std::size_t disc_tracks = 0u;
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
    bool discover_latent_aot = false;
    std::uint32_t disc_volume_start_lba = 0u;
    std::uint32_t disc_extent_lba_bias = 0u;
};

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const PreparedPortProgram& prepared,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

// Moves local-only mutable state from a replaced port into its freshly
// published successor without ever copying it through codegen staging.
void preserve_local_port_user_data(const std::filesystem::path& previous_root,
                                   const std::filesystem::path& published_root);

} // namespace katana::codegen
