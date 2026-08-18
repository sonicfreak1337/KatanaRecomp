#pragma once

#include "katana/ir/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t latent_aot_analysis_cache_schema_version = 7u;
inline constexpr std::uint32_t latent_aot_analysis_address_layout_schema = 1u;
// Positive artifacts contain untrusted, unoptimized IR. Current bytes can
// authenticate every retained instruction but cannot prove that the cached
// program omitted no indirect target or callback. Product discovery therefore
// neither consumes nor publishes positive entries; safe source-shape-derived
// negative entries remain enabled.
inline constexpr bool latent_aot_positive_product_cache_enabled = false;
inline constexpr std::uint32_t latent_aot_analysis_ir_schema = 4u;
inline constexpr std::uint32_t latent_aot_analysis_optimizer_schema = 2u;
inline constexpr std::string_view latent_aot_analysis_implementation_id =
    "katana-latent-aot-analysis-v27";

inline constexpr std::size_t maximum_latent_aot_analysis_cache_artifact_bytes =
    32u * 1024u * 1024u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_functions = 2'048u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_blocks = 8'192u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_instructions = 32'768u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_successors = 65'536u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_targets = 262'144u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_callsites = 65'536u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_entries = 4'096u;
inline constexpr std::size_t maximum_latent_aot_analysis_cache_parser_depth = 8u;

// Only deterministic candidate outcomes belong in the negative cache. Process
// failures, cancellation and I/O failures deliberately have no enum value.
enum class LatentAotAnalysisRejection : std::uint8_t {
    None = 0u,
    NoEntryPoints,
    EntryDecodeFailed,
    ControlFlowIncomplete,
    InventoryTruncated,
    ProgramInvalid,
    RelocationNotClosed,
    EntryBlockMissing,
    FunctionBudgetExceeded,
    BlockBudgetExceeded,
    InstructionBudgetExceeded,
    AnalysisIterationBudgetExceeded,
    AnalysisContextBudgetExceeded,
};

struct LatentAotAnalysisCacheKeyInputs {
    // Exactly 64 lowercase hexadecimal characters, without a "sha256:" prefix.
    std::string byte_sha256;
    std::uint32_t byte_size = 0u;
    // Treated as a set: the key builder sorts and deduplicates these offsets.
    std::vector<std::uint32_t> entry_offsets;
    bool exact_candidate = false;
    std::uint32_t source_address = 0u;
    std::uint32_t address_layout_schema =
        latent_aot_analysis_address_layout_schema;

    std::size_t maximum_entry_scan_instructions = 0u;
    std::size_t maximum_native_instructions = 0u;
    std::size_t maximum_blocks = 0u;
    std::size_t maximum_functions = 0u;
    std::size_t maximum_analysis_iterations = 0u;
    std::size_t maximum_analysis_contexts = 0u;

    std::uint32_t analyzer_abi = 0u;
    std::string analyzer_implementation_id{
        latent_aot_analysis_implementation_id};
    std::uint32_t ir_schema = latent_aot_analysis_ir_schema;
    std::uint32_t optimizer_schema = latent_aot_analysis_optimizer_schema;
};

[[nodiscard]] std::string make_latent_aot_analysis_cache_key(
    const LatentAotAnalysisCacheKeyInputs& inputs);

enum class LatentAotAnalysisCacheState : std::uint8_t {
    Miss = 0u,
    Positive,
    Negative,
    Corrupt,
};

struct LatentAotAnalysisCacheParseResult {
    LatentAotAnalysisCacheState state = LatentAotAnalysisCacheState::Miss;
    // Positive entries remain codec data only. Product discovery must treat
    // them as a miss and run complete CFA/FVA/lowering.
    std::vector<katana::ir::Function> program;
    LatentAotAnalysisRejection rejection = LatentAotAnalysisRejection::None;
};

// Shared bounded binary IR codec used by integrity-bound analysis caches.
// The payload has no envelope of its own: callers must bind it to their
// schema/key/checksum and must validate it against the current source bytes
// before treating it as executable input.
struct IrProgramCacheLimits {
    std::size_t maximum_payload_bytes = 0u;
    std::size_t maximum_functions = 0u;
    std::size_t maximum_blocks = 0u;
    std::size_t maximum_instructions = 0u;
    std::size_t maximum_successors = 0u;
    std::size_t maximum_targets = 0u;
    std::size_t maximum_callsites = 0u;
    std::size_t maximum_parser_depth = 0u;
    // Cumulative capacity reserved by every nested decoded vector. This is
    // separate from the serialized-byte limit because compact counts can
    // otherwise amplify into much larger C++ object graphs.
    std::size_t maximum_allocation_bytes = 0u;
};

[[nodiscard]] std::vector<std::uint8_t>
serialize_ir_program_cache_payload(
    std::span<const katana::ir::Function> program,
    const IrProgramCacheLimits& limits);

// Throws std::runtime_error for malformed, non-canonical or over-budget
// payloads. A successful parse consumes the complete payload.
[[nodiscard]] std::vector<katana::ir::Function>
parse_ir_program_cache_payload(
    std::span<const std::uint8_t> payload,
    const IrProgramCacheLimits& limits);

// The returned artifact is a complete bounded envelope. Invalid keys, invalid
// IR, non-deterministic rejection values and limit violations throw.
[[nodiscard]] std::vector<std::uint8_t> serialize_latent_aot_positive_cache(
    std::string_view key,
    std::span<const katana::ir::Function> program);

[[nodiscard]] std::vector<std::uint8_t> serialize_latent_aot_negative_cache(
    std::string_view key,
    LatentAotAnalysisRejection rejection);

// A schema or exact-key mismatch is a clean Miss. Malformed, truncated,
// oversized, checksum-invalid or non-canonical artifacts are Corrupt.
[[nodiscard]] LatentAotAnalysisCacheParseResult parse_latent_aot_analysis_cache(
    std::string_view expected_key,
    std::span<const std::uint8_t> artifact);

} // namespace katana::codegen
