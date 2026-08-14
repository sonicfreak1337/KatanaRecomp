#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/build_contract.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/ir.hpp"
#include "katana/progress.hpp"
#include "katana/runtime/game_project.hpp"
#include "katana/runtime/native_port.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::platform {
struct DreamcastDiscBoot;
}

namespace katana::codegen {

inline constexpr std::uint32_t port_project_contract_version =
    build_contract::port_project_contract_version;
inline constexpr std::uint32_t port_partition_emission_schema_version = 7u;
inline constexpr std::uint32_t port_metadata_cache_schema_version = 8u;

using PortExportProgressCallback =
    std::function<void(std::string_view phase)>;

// One authoritative code-generation budget contract is shared by the actual
// partition workers and post-configure telemetry. The global value is the
// primary request; the legacy port-local value remains a restricting cap.
[[nodiscard]] std::size_t resolve_port_codegen_jobs(
    std::size_t partition_count,
    std::size_t detected_jobs,
    std::optional<std::string_view> global_requested,
    std::optional<std::string_view> legacy_port_cap);
[[nodiscard]] std::size_t configured_port_codegen_jobs(
    std::size_t partition_count);

// Private export-time payload for one descriptor-only game-project runtime
// image. The caller owns both the identifier and bytes for the complete
// export call. Payload bytes are validated but never copied into the external
// game-project artifact or generated port distribution.
struct GameProjectRuntimeImagePayload {
    std::string_view image_id;
    std::span<const std::uint8_t> bytes;
};

// Export-time-only post-bootstrap bytes. They authenticate and construct the
// exact executable view used by static analysis; neither paths nor bytes are
// copied into the generated product. Runtime materialization is independently
// revalidated against the same NativePortBootstrapWriteBinding identities.
struct NativePortBootstrapWritePayload {
    std::uint32_t guest_address = 0u;
    std::span<const std::uint8_t> bytes;
};

enum class PortAnalysisMode : std::uint8_t {
    PlatformAbi,
    ConservativeRuntimeOnly,
};

struct PortExportOptions {
    std::string target_name;
    std::string tool_version;
    PartitionOptions partition_options;
    std::filesystem::path forbidden_source_root;
    bool diagnostic_partial = false;
    std::string console_profile = "japan-ntsc";
    PortExportProgressCallback progress_callback;
    // Structured, nested progress for CLI/GUI consumers. The legacy phase
    // callback remains for stable textual checkpoints.
    katana::ProgressReporter progress;
    // Enables component-level FunctionValue cache miss diagnostics. Basic
    // progress remains cheap and must not retain the detailed key history.
    bool detailed_analysis_telemetry = false;
    // Optional persistent, local-only caches. Analysis and codegen identities
    // are build-bound to their respective implementation components so a
    // runtime/UI-only rebuild does not invalidate native analysis artifacts.
    std::filesystem::path analysis_cache_root;
    std::filesystem::path codegen_cache_root;
    // Pure analyzer/FVA semantics and persistent epoch codec identity.
    std::string analysis_implementation_identity;
    // Prepared boot/latent IR cache codec and IR contract identity.
    std::string analysis_cache_implementation_identity;
    std::string codegen_implementation_identity;
    // Optional external, identity-bound game project. The caller owns the
    // definition and all referenced spans for the complete export call.
    const katana::runtime::GameProjectDefinition* game_project = nullptr;
    // Independent native product definition. It is deliberately not nested
    // in the historical Dreamcast GameProjectDefinition. The caller owns the
    // definition and every referenced span for the complete export call.
    const katana::runtime::NativePortDefinition* native_port_definition =
        nullptr;
    // Exact private payloads for descriptor-only runtime images. Every
    // descriptor requires one uniquely identified payload and extra payloads
    // are rejected.
    std::span<const GameProjectRuntimeImagePayload>
        game_project_runtime_image_payloads;
    std::span<const NativePortBootstrapWritePayload>
        native_port_bootstrap_write_payloads;
    // Opt-in exact mid-block continuation entries for static AOT. Each entry
    // is accepted only when it is an instruction fallthrough inside the
    // hash-bound immutable boot image at which generated code can yield.
    std::span<const std::uint32_t> native_aot_resume_entries;
    // Export-time-only, hash-bound native entries for exact disc modules.
    // The caller owns the descriptors; they contain no source bytes or paths.
    std::span<const LatentAotEntryHint> latent_aot_entry_hints;
    LatentAotDiscoveryMode latent_aot_discovery_mode =
        LatentAotDiscoveryMode::HintsAndHeuristics;
};

struct PortExportResult {
    std::filesystem::path output_root;
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
    std::size_t generated_files = 0u;
    std::size_t removed_files = 0u;
    std::size_t codegen_cache_hits = 0u;
    std::size_t codegen_cache_misses = 0u;
    bool boot_analysis_cache_hit = false;
    std::size_t boot_analysis_pipeline_runs = 0u;
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
    // Optional products precomputed by the persistent boot-analysis layer.
    // Their owners must outlive the complete export call.
    const katana::analysis::DreamcastHardwareAudit*
        precomputed_hardware_audit = nullptr;
    const katana::analysis::AnalysisGraph*
        precomputed_control_flow_graph = nullptr;
    const katana::analysis::AnalysisGraph*
        precomputed_call_graph = nullptr;
    // Immutable source view before any explicitly bound native bootstrap
    // materialization changes the analysis/AOT image. Native prepared callers
    // with bootstrap writes must provide it; the wrapper overloads do so.
    // Appended so existing aggregate callers retain their field mapping.
    const katana::io::ExecutableImage* pre_bootstrap_image = nullptr;
};

// Validates the one-to-one descriptor/payload binding, including identifier,
// byte count and SHA-256. This is public so the CLI can reject an invalid
// private provider before admitting a whole-export cache hit.
void validate_game_project_runtime_image_payloads(
    const katana::runtime::GameProjectDefinition* game_project,
    std::span<const GameProjectRuntimeImagePayload> payloads,
    const katana::runtime::NativePortDefinition* native_port);

void validate_native_port_bootstrap_write_payloads(
    const katana::runtime::NativePortDefinition* native_port,
    std::span<const NativePortBootstrapWritePayload> payloads);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const PreparedPortProgram& prepared,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                              const std::filesystem::path& output_root,
                              const PortExportOptions& options);

// Reuses a single fully validated disc load for content-addressed cache
// admission and a following NativeDisc export without rehashing the GDI.
[[nodiscard]] PortExportResult
export_dreamcast_port_project(
    const katana::platform::DreamcastDiscBoot& disc,
    const std::filesystem::path& output_root,
    const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project(
    const katana::platform::DreamcastDiscBoot& disc,
    const std::filesystem::path& output_root,
    const PortExportOptions& options,
    PortAnalysisMode analysis_mode);

// Bring-up export from an immutable private boot-executable artifact. The
// generated distributable contains native AOT plus the hash/layout recipe, but
// never copies the artifact's retail boot bytes.
[[nodiscard]] PortExportResult
export_dreamcast_port_project_from_boot_artifact(
    const std::filesystem::path& artifact_manifest_path,
    const std::filesystem::path& output_root,
    const PortExportOptions& options);

[[nodiscard]] PortExportResult
export_dreamcast_port_project_from_boot_artifact(
    const std::filesystem::path& artifact_manifest_path,
    const std::filesystem::path& output_root,
    const PortExportOptions& options,
    PortAnalysisMode analysis_mode);

// Moves local-only mutable state and the optional direct-launch content-root
// binding from a replaced port into its freshly published successor without
// ever copying either through codegen staging.
void preserve_local_port_user_data(const std::filesystem::path& previous_root,
                                   const std::filesystem::path& published_root);

} // namespace katana::codegen
