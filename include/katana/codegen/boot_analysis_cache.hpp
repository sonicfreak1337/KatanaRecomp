#pragma once

#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/ir/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t boot_analysis_cache_schema_version = 12u;
inline constexpr std::uint32_t boot_analysis_cache_ir_schema_version = 3u;
inline constexpr std::uint32_t boot_analysis_checkpoint_schema_version = 2u;
inline constexpr std::size_t maximum_boot_analysis_cache_artifact_bytes =
    256u * 1024u * 1024u;
// A byte-bound cached program cannot prove that it did not omit a
// Function-Value/guarded-inventory discovered callback or indirect target.
// Product exports therefore never consume or publish positive boot-analysis
// artifacts. The codec remains available for bounded compatibility parsing
// and hostile-artifact regression coverage.
inline constexpr bool boot_analysis_positive_product_cache_enabled = false;

// This is the deliberately small post-analysis capsule consumed by port
// export. It contains every CFA-derived value still used after lowering, plus
// unoptimized IR. Run-local lattices, evidence interners and instruction
// arenas are intentionally absent: their effects have already been
// materialized into these fields and the cached hardware/graph products.
struct PreparedBootAnalysisArtifact {
    katana::analysis::ControlFlowAnalysisResult analysis;
    std::vector<katana::ir::Function> lowered_program;
    std::vector<katana::analysis::HardwareNaturalLoop> hardware_loops;
    katana::analysis::AnalysisGraph control_flow_graph;
    katana::analysis::AnalysisGraph call_graph;
};

enum class BootAnalysisCacheState : std::uint8_t {
    Miss = 0u,
    Hit,
    Corrupt,
};

struct BootAnalysisCacheParseResult {
    BootAnalysisCacheState state = BootAnalysisCacheState::Miss;
    PreparedBootAnalysisArtifact artifact;
    std::string reason;
};

// semantic_contract_identity binds analysis-visible GameProject/runtime-image
// and latent-hint contracts. Target name, output path, console presentation,
// codegen/runtime implementation IDs and packaging are deliberately excluded.
[[nodiscard]] std::string make_boot_analysis_cache_key(
    const katana::io::ExecutableImage& image,
    const katana::analysis::AnalysisOverrides* overrides,
    std::string_view semantic_contract_identity,
    std::string_view analysis_implementation_identity);

// Only complete native product analyses are cacheable. Diagnostic-partial
// analyses and incomplete inventories must never enter this layer.
[[nodiscard]] bool boot_analysis_artifact_cacheable(
    const PreparedBootAnalysisArtifact& artifact) noexcept;

// A checkpoint is a bounded, source-revalidated analyzer snapshot. Unlike the
// positive product cache it may carry guarded or unresolved control-flow
// evidence; those completeness facts remain data and are never upgraded by
// serialization or replay.
[[nodiscard]] bool boot_analysis_artifact_checkpointable(
    const PreparedBootAnalysisArtifact& artifact) noexcept;

[[nodiscard]] std::vector<std::uint8_t> serialize_boot_analysis_cache(
    std::string_view key,
    const PreparedBootAnalysisArtifact& artifact);

// The envelope, counts and enums are checked here. Source-byte binding and
// current IR verification are checked separately by
// validate_boot_analysis_cache_source_binding().
[[nodiscard]] BootAnalysisCacheParseResult parse_boot_analysis_cache(
    std::string_view expected_key,
    std::span<const std::uint8_t> artifact);

// Separate envelope for incomplete-but-structurally-valid analysis state.
// Keeping this distinct from the positive cache prevents a partial checkpoint
// from becoming product authority through a shared parser or predicate.
[[nodiscard]] std::vector<std::uint8_t> serialize_boot_analysis_checkpoint(
    std::string_view key,
    const PreparedBootAnalysisArtifact& artifact);

[[nodiscard]] BootAnalysisCacheParseResult parse_boot_analysis_checkpoint(
    std::string_view expected_key,
    std::span<const std::uint8_t> artifact);

// Positive product reuse is fail-closed while source bytes alone cannot prove
// the complete CFA/FVA coverage universe. Consequently this product admission
// gate returns false; callers must run the complete analysis pipeline.
[[nodiscard]] bool validate_boot_analysis_cache_source_binding(
    PreparedBootAnalysisArtifact& artifact,
    const katana::io::ExecutableImage& image,
    const katana::analysis::AnalysisOverrides* overrides = nullptr,
    katana::analysis::DreamcastHardwareAudit* rebuilt_hardware_audit =
        nullptr) noexcept;

} // namespace katana::codegen
