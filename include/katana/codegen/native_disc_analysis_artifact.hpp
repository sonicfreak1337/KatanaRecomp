#pragma once

#include "katana/analysis/hardware_audit.hpp"
#include "katana/codegen/boot_analysis_cache.hpp"
#include "katana/codegen/latent_aot_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

// Schema 8 binds the optimized Primary/Latent IR retained by this monolithic
// checkpoint to its product optimizer independently from lowering/CFA caches.
// Schema 8 additionally retains the bounded, source-authenticated PC-relative
// literal ledger of every accepted latent module.
// Schema 6 gave incomplete analyzer closure a distinct, bounded nested
// checkpoint envelope. It also removes downstream codegen implementation
// churn from the canonical analysis lookup key; current product admission is
// always replayed and validated before any result is consumed.
inline constexpr std::uint32_t native_disc_analysis_artifact_schema_version = 8u;
inline constexpr std::uint32_t native_disc_analysis_artifact_codec_version = 1u;
inline constexpr std::size_t maximum_native_disc_analysis_artifact_bytes =
    256u * 1024u * 1024u;
// Current bytes can authenticate a bounded analysis checkpoint, but cannot
// prove that an incomplete whole-disc result omitted no callback, transfer
// target or hardware owner. Analysis tooling may resume an exact checkpoint;
// product admission remains independently gated on complete proofs.
inline constexpr bool native_disc_analysis_positive_product_cache_enabled =
    false;

// Stable, path-free identities which bind one positive analysis product to
// the exact product inputs and semantic contracts that created it.  The
// aggregate key remains authoritative; the individual fields make a clean
// cache miss explainable without weakening admission.
struct NativeDiscAnalysisArtifactIdentity final {
    std::string key;
    std::string content_identity;
    std::string boot_byte_identity;
    std::string project_identity;
    // Stable pre-analysis contract key. It includes every analysis-visible
    // option, hint, runtime-image payload identity and bootstrap-write
    // payload identity, but not the roots discovered by the analysis itself.
    std::string analysis_contract_identity;
    std::string image_analysis_key;
    std::string game_project_identity;
    std::string native_port_identity;
    std::string native_port_artifact_identity;
    std::string analysis_implementation_identity;
    std::string analysis_cache_implementation_identity;
    // The current monolithic artifact serializes optimized Primary/Latent IR.
    // This identity is therefore authoritative even though generic emitter and
    // packaging changes remain downstream.
    std::string ir_product_implementation_identity;
    // Downstream runtime-frontier binding only. Schema-8 analysis checkpoints
    // deliberately neither hash nor serialize this field; current admission
    // fills it after replay before any generated product can consume it.
    std::string codegen_implementation_identity;
    std::uint32_t analyzer_abi = 0u;
    std::uint32_t backend_abi = 0u;
    std::uint32_t analysis_mode = 0u;
    std::uint32_t disc_volume_start_lba = 0u;
    std::uint32_t disc_extent_lba_bias = 0u;

    [[nodiscard]] bool operator==(
        const NativeDiscAnalysisArtifactIdentity&) const = default;
};

// Pointer-free analysis-derived portions of NativePortProgramIndex.  The IR
// itself reconstructs owners and block boundaries, but these relations also
// depend on CFA/FVA evidence which the compact boot codec deliberately omits.
// Keeping them typed and canonical lets a checkpoint recompute NativePort
// admission under a changed provider contract without silently weakening the
// analyzed product graph.
struct NativeDiscProgramIndexAdjacency final {
    std::uint32_t address = 0u;
    std::vector<std::uint32_t> related_addresses;

    [[nodiscard]] bool operator==(
        const NativeDiscProgramIndexAdjacency&) const = default;
};

struct NativeDiscProgramIndexCheckpoint final {
    std::vector<NativeDiscProgramIndexAdjacency> incoming_edge_sources;
    std::vector<NativeDiscProgramIndexAdjacency>
        incoming_instruction_addresses;
    std::vector<NativeDiscProgramIndexAdjacency> outgoing_function_entries;
    std::vector<std::uint32_t> seed_entries;
    std::vector<std::uint32_t> incomplete_outgoing_function_entries;

    [[nodiscard]] bool operator==(
        const NativeDiscProgramIndexCheckpoint&) const = default;
};

struct NativeDiscHardwareGapArtifact final {
    std::uint32_t instruction_address = 0u;
    std::string reason;

    [[nodiscard]] bool operator==(
        const NativeDiscHardwareGapArtifact&) const = default;
};

// Computes the canonical path-free lookup key from stable input identities.
// image_analysis_key is deliberately validated after the artifact's proven
// external roots have been rebound to the current image, avoiding a circular
// cache lookup. Parsing and admission recompute this key rather than trusting
// the redundant envelope field.
[[nodiscard]] std::string native_disc_analysis_artifact_identity_key(
    const NativeDiscAnalysisArtifactIdentity& identity);

// Portable, pointer-free output of the authoritative NativeDisc analysis
// prefix. Derived lookup maps and admission indices are rebuilt after parse;
// no process-local arena, shared_ptr or unordered-container state survives.
struct NativeDiscAnalysisArtifact final {
    NativeDiscAnalysisArtifactIdentity identity;
    PreparedBootAnalysisArtifact primary;
    LatentAotDiscovery latent;
    katana::analysis::DreamcastHardwareAudit primary_hardware_audit;
    katana::analysis::DreamcastHardwareAudit native_hardware_audit;
    std::vector<std::uint32_t> external_primary_roots;
    std::vector<std::uint32_t> native_resume_entries;
    std::uint32_t entry_address = 0u;
    std::uint32_t boot_address = 0u;
    std::uint64_t boot_size = 0u;
    std::uint32_t product_entry_address = 0u;
    std::uint32_t product_entry_owner = 0u;
    std::uint64_t known_hardware_sites = 0u;
    std::uint64_t native_hardware_gaps = 0u;
    std::uint64_t sdk_provider_candidates = 0u;
    NativeDiscProgramIndexCheckpoint native_port_program_index;
    std::vector<NativeDiscHardwareGapArtifact> native_hardware_gap_details;
    std::vector<std::uint32_t>
        replacement_reachability_incomplete_frontier;
    bool guarded_inventory_complete = false;
    bool native_hardware_closure_complete = false;
    bool replacement_reachability_proven = false;
    bool backend_admitted = false;
};

enum class NativeDiscAnalysisArtifactState : std::uint8_t {
    Miss = 0u,
    Hit,
    Corrupt,
};

struct NativeDiscAnalysisArtifactParseResult final {
    NativeDiscAnalysisArtifactState state =
        NativeDiscAnalysisArtifactState::Miss;
    NativeDiscAnalysisArtifact artifact;
    std::string reason;
};

[[nodiscard]] bool native_disc_analysis_artifact_publishable(
    const NativeDiscAnalysisArtifact& artifact) noexcept;

// An analysis checkpoint preserves an exact, source-bound analyzer result
// without promoting its completeness bits. It is consumed only by analysis
// tooling; native product admission must additionally satisfy the stronger
// product predicate below.
[[nodiscard]] bool native_disc_analysis_artifact_checkpointable(
    const NativeDiscAnalysisArtifact& artifact) noexcept;

[[nodiscard]] bool native_disc_analysis_artifact_product_admissible(
    const NativeDiscAnalysisArtifact& artifact) noexcept;

[[nodiscard]] std::vector<std::uint8_t>
serialize_native_disc_analysis_artifact(
    const NativeDiscAnalysisArtifact& artifact);

// A key mismatch is a clean miss. Malformed, non-canonical, over-budget or
// checksum-invalid data is Corrupt and must never partially populate state.
[[nodiscard]] NativeDiscAnalysisArtifactParseResult
parse_native_disc_analysis_artifact(
    std::string_view expected_key,
    std::span<const std::uint8_t> bytes);

} // namespace katana::codegen
