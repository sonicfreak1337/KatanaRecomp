#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/build_contract.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/ir.hpp"
#include "katana/runtime/game_project.hpp"

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
inline constexpr std::uint32_t port_partition_emission_schema_version = 5u;
inline constexpr std::uint32_t port_metadata_cache_schema_version = 1u;

using PortExportProgressCallback = void (*)(std::string_view phase);

struct PortExportOptions {
    std::string target_name;
    std::string tool_version;
    PartitionOptions partition_options;
    std::filesystem::path forbidden_source_root;
    bool diagnostic_partial = false;
    std::string console_profile = "japan-ntsc";
    PortExportProgressCallback progress_callback = nullptr;
    // Optional persistent, local-only cache. Each AOT partition is looked up
    // here before invoking the backend emitter.
    std::filesystem::path codegen_cache_root;
    // Optional external, identity-bound game project. The caller owns the
    // definition and all referenced spans for the complete export call.
    const katana::runtime::GameProjectDefinition* game_project = nullptr;
};

struct PortExportResult {
    std::filesystem::path output_root;
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
    std::size_t generated_files = 0u;
    std::size_t removed_files = 0u;
    std::size_t codegen_cache_hits = 0u;
    std::size_t codegen_cache_misses = 0u;
    bool metadata_cache_hit = false;
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
    // DirectBootExecutable starts the verified BOOT executable while retaining
    // the configured shared firmware services. False preserves the historical
    // NativeDiscBoot/firmware-mode mapping.
    bool direct_boot_executable = false;
};

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const PreparedPortProgram& prepared,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

// Bring-up export from an immutable private boot-executable artifact. The
// generated distributable contains native AOT plus the hash/layout recipe, but
// never copies the artifact's retail boot bytes.
[[nodiscard]] PortExportResult
export_dreamcast_port_project_from_boot_artifact(
    const std::filesystem::path& artifact_manifest_path,
    const std::filesystem::path& output_root,
    const PortExportOptions& options);

// Moves local-only mutable state from a replaced port into its freshly
// published successor without ever copying it through codegen staging.
void preserve_local_port_user_data(const std::filesystem::path& previous_root,
                                   const std::filesystem::path& published_root);

} // namespace katana::codegen
