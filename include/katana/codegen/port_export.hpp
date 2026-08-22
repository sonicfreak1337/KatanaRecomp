#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/build_contract.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/native_disc_analysis_artifact.hpp"
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::platform {
struct DreamcastDiscBoot;
}

namespace katana::runtime {
class GdiDiscSource;
}

namespace katana::codegen {

struct NativeDiscAnalysisState;

inline constexpr std::uint32_t port_project_contract_version =
    build_contract::port_project_contract_version;
inline constexpr std::uint32_t port_partition_emission_schema_version = 9u;
inline constexpr std::uint32_t port_metadata_cache_schema_version = 12u;

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
    // Prepared boot IR cache codec and unoptimized IR/lowering contract.
    std::string analysis_cache_implementation_identity;
    // Product optimizer identity. Native-disc and latent checkpoints currently
    // retain optimized IR and therefore bind this independently from generic
    // codegen/emitter churn.
    std::string ir_product_implementation_identity;
    std::string codegen_implementation_identity;
    // A transactionally committed analysis archive supplied by
    // `analyze-port --resume`. The archive remains bound to its ledger key;
    // current analyzer/cache identities and every source binding are
    // revalidated before it can seed a new agent generation. This deliberately
    // bypasses codegen-only identity churn because admission is replayed from
    // the stored analysis product.
    std::span<const std::uint8_t> resume_analysis_artifact;
    std::string_view resume_analysis_artifact_key;
    // Optional external, identity-bound game project. The caller owns the
    // definition and all referenced spans for the complete export call.
    const katana::runtime::GameProjectDefinition* game_project = nullptr;
    // Independent native product definition. It is deliberately not nested
    // in the historical Dreamcast GameProjectDefinition. The caller owns the
    // definition and every referenced span for the complete export call.
    const katana::runtime::NativePortDefinition* native_port_definition =
        nullptr;
    // Exact identity of the verified private artifact which supplied
    // native_port_definition.  The generated product embeds this identity
    // and the CLI verifies it again after linking, before publication.  API
    // callers without an artifact may leave it empty and receive the
    // definition-derived export identity instead.
    std::string native_port_artifact_identity;
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
    // Analysis-only tooling may request a private, source-bound archive for
    // agent query/diff. Product exports keep this false so a disabled positive
    // cache does not spend time serializing and writing an unusable 256-MiB
    // component artifact.
    bool analysis_artifact_archive_requested = false;
    // Materialization-World/agent JSON is analysis-tool output. Ordinary
    // product exports keep this false and do not serialize/discard the full
    // world merely to build the port.
    bool agent_analysis_artifacts_requested = false;
    // Explicit agent-workflow refreshes retain the committed World as their
    // comparison baseline and keep all lower, identity-bound analysis caches,
    // but must not reuse the monolithic whole-disc checkpoint they are meant
    // to supersede. A successfully serialized refresh may replace only the
    // exact bounded cache artifact observed under the same identity.
    bool analysis_artifact_refresh_requested = false;
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

struct NativeDiscAnalysisSummary {
    std::size_t primary_functions = 0u;
    std::size_t combined_functions = 0u;
    std::size_t latent_modules = 0u;
    std::size_t external_primary_roots = 0u;
    std::size_t native_resume_entries = 0u;
    std::size_t known_hardware_sites = 0u;
    std::size_t native_hardware_gaps = 0u;
    std::size_t provider_semantic_contracts = 0u;
    std::size_t provider_semantic_summaries = 0u;
    std::size_t provider_semantic_matches = 0u;
    std::size_t provider_semantic_misses = 0u;
    std::size_t provider_semantic_legacy_admissions = 0u;
    bool provider_semantic_coverage_required = false;
    std::size_t sdk_provider_candidates = 0u;
    bool guarded_inventory_complete = false;
    bool native_hardware_closure_complete = false;
    bool backend_admitted = false;
};

// Complete owning result of the authoritative NativeDisc analysis prefix.
// It contains no generated C++, CMake project, host binary or published port
// tree.  Both the analysis-only CLI and the product exporter consume this
// exact state so latent/cross-image closure can never diverge between them.
struct NativeDiscAnalysisResult {
    katana::io::ExecutableImage image;
    katana::io::ExecutableImage pre_bootstrap_image;
    katana::analysis::ControlFlowAnalysisResult analysis;
    std::vector<katana::ir::Function> program;
    std::vector<katana::io::InputProvenance> inputs;
    katana::analysis::DreamcastHardwareAudit hardware_audit;
    katana::analysis::AnalysisGraph control_flow_graph;
    katana::analysis::AnalysisGraph call_graph;
    std::vector<std::uint32_t> latent_external_primary_roots;
    // Opaque owning state produced by the same pre-codegen admission pass
    // used by the product exporter.  Keeping it here prevents a following
    // export from repeating CFA/FVA, latent discovery or hardware closure.
    std::shared_ptr<NativeDiscAnalysisState> admitted_state;
    NativeDiscAnalysisArtifactIdentity analysis_artifact_identity;
    // Present only when --resume accepted a prior analyzer generation across
    // a strictly revalidated NativePort/admission-only identity change. The
    // CLI uses it to bind the old World/Ledger as input while committing the
    // freshly published current generation under analysis_artifact_identity.
    std::optional<NativeDiscAnalysisArtifactIdentity>
        resumed_from_analysis_artifact_identity;
    // Present only when an analysis archive was explicitly requested or the
    // positive product cache is enabled, and only for a complete, positive,
    // source-bound analysis. With positive reuse disabled these bytes are an
    // archive for agent query/diff, never a claimed cache hit source.
    std::vector<std::uint8_t> analysis_artifact_bytes;
    // Deterministic, source-bound agent view of the same admitted state.
    // The binary form is the resumable/queryable artifact; JSON is a bounded
    // human- and agent-readable projection of exactly that world.
    std::vector<std::uint8_t> materialization_world_artifact_bytes;
    std::string materialization_world_json;
    std::string agent_decision;
    std::string agent_decision_reason;
    std::uint64_t agent_decision_focus = 0u;
    std::size_t agent_actionable_frontier = 0u;
    NativeDiscAnalysisSummary summary;
    std::shared_ptr<katana::runtime::GdiDiscSource> disc_source;
    std::string project_identity;
    std::uint32_t entry_address = 0u;
    std::uint32_t boot_address = 0u;
    std::size_t boot_size = 0u;
    std::uint32_t disc_volume_start_lba = 0u;
    std::uint32_t disc_extent_lba_bias = 0u;
    bool boot_analysis_cache_hit = false;
    std::size_t boot_analysis_pipeline_runs = 0u;
    bool latent_primary_root_seed_cache_hit = false;
    bool latent_primary_root_seed_cache_published = false;
    bool latent_primary_root_seed_cache_publish_missed = false;
    bool analysis_artifact_cache_hit = false;
    bool analysis_artifact_cache_published = false;
    bool analysis_artifact_cache_publish_missed = false;
};

enum class NativeDiscAnalysisHintPublicationResult : std::uint8_t {
    NotPending,
    Published,
    Missed,
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

// Executes the complete NativeDisc product-analysis path but stops before
// partitioning, source emission, project generation, host build and publish.
// The returned state owns every image/IR/graph/module object needed by a
// following product export.
[[nodiscard]] NativeDiscAnalysisResult
analyze_native_disc_port(
    const katana::platform::DreamcastDiscBoot& disc,
    const PortExportOptions& options,
    PortAnalysisMode analysis_mode = PortAnalysisMode::PlatformAbi);

// Publishes only bounded, fully validated analysis hints that were deferred
// while analyze-port's archive/World generation was still a candidate. The
// caller must invoke this exactly after its archive, World and authority
// ledger transaction has committed. CFA/FVA summaries and latent module
// artifacts remain outside this hint-only publication path.
[[nodiscard]] bool has_deferred_native_disc_analysis_hints(
    const NativeDiscAnalysisResult& analyzed) noexcept;

[[nodiscard]] NativeDiscAnalysisHintPublicationResult
publish_committed_native_disc_analysis_hints(
    NativeDiscAnalysisResult& analyzed) noexcept;

// Computes only the stable, pre-analysis identity of the exact materialized
// disc/options contract. It performs no CFA/FVA, admission, World generation
// or completeness decision. A caller may compare it with an authenticated
// committed manifest to identify a true no-op resume.
[[nodiscard]] NativeDiscAnalysisArtifactIdentity
native_disc_analysis_resume_manifest_identity(
    const katana::platform::DreamcastDiscBoot& disc,
    const PortExportOptions& options,
    std::span<const std::uint32_t> external_primary_roots,
    PortAnalysisMode analysis_mode = PortAnalysisMode::PlatformAbi);

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
