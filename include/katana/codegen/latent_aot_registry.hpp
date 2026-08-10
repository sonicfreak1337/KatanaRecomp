#pragma once

#include "katana/ir/ir.hpp"
#include "katana/progress.hpp"
#include "katana/runtime/disc.hpp"
#include "katana/runtime/native_aot_template.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::codegen {

// Native internal resume entries are separate authenticated dispatch entries,
// but they are not separate IR blocks. Keep their transport budget aligned
// with the exporter/runtime template contract instead of reusing the smaller
// analysis block budget.
inline constexpr std::size_t maximum_prepared_latent_aot_block_identities =
    65'536u;
inline constexpr std::uint64_t
    maximum_prepared_latent_aot_block_identity_bytes =
        64ull * 1024ull * 1024ull;

// A local, export-time-only request for a native entry in an exact disc module.
// The offset is meaningful only for the exact byte identity and logical disc
// location; it is never inferred from mutable runtime memory.
struct LatentAotEntryHint {
    std::string byte_identity;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::uint32_t module_relative_offset = 0u;

    [[nodiscard]] bool operator==(const LatentAotEntryHint&) const = default;
};

enum class LatentAotDiscoveryMode : std::uint8_t {
    // Exact hints are resolved first, followed by bounded unhinted discovery.
    HintsAndHeuristics,
    // Only exact hints are resolved; an empty hint set performs no disc scan.
    ExactOnly,
};

enum class LatentAotCompletenessPolicy : std::uint8_t {
    // Publication requires the complete guarded inventory contract.
    Strict,
    // Exact, byte-identity-bound entries may retain only candidate-domain or
    // ABI-stack-base inventory loss. Their emitted CFG remains complete and
    // RuntimeOnly dispatch still stops on every omitted target.
    ExactRuntimeOnlyStopOnMiss,
};

struct LatentAotDiscoveryOptions {
    LatentAotDiscoveryMode mode = LatentAotDiscoveryMode::HintsAndHeuristics;
    LatentAotCompletenessPolicy completeness_policy =
        LatentAotCompletenessPolicy::Strict;
    // Optional persistent, local-only negative analysis cache. The caller must place it
    // below its existing private codegen-cache root; neither this path nor a
    // cache key survives into exported product metadata. Persistent caching is
    // Each cache layer is enabled only when its own exact build-bound
    // implementation identity is present; empty identities deliberately
    // disable the corresponding layer.
    std::filesystem::path analysis_cache_root;
    // Pure analyzer/FVA semantics and persistent epoch codec identity.
    std::string analysis_implementation_identity;
    // Latent positive/negative IR-cache codec and IR contract identity.
    std::string analysis_cache_implementation_identity;
    katana::ProgressReporter progress;
    std::size_t maximum_directory_entries = 4096u;
    std::size_t maximum_directory_bytes = 4u * 1024u * 1024u;
    std::size_t maximum_total_directory_bytes = 16u * 1024u * 1024u;
    // Maximum number of unique heuristic templates analyzed. Byte-identical
    // later extents may still be hashed within maximum_total_file_bytes so
    // their source bindings are not lost.
    std::size_t maximum_candidate_files = 128u;
    std::size_t maximum_file_bytes =
        katana::runtime::maximum_native_aot_template_extent;
    std::size_t maximum_total_file_bytes = 64u * 1024u * 1024u;
    std::size_t maximum_workers = 64u;
    std::size_t maximum_entry_scan_instructions = 1024u;
    std::size_t maximum_native_instructions_per_module = 32768u;
    std::size_t maximum_blocks_per_module = 8192u;
    std::size_t maximum_functions_per_module = 2048u;
    std::size_t maximum_analysis_iterations = 64u;
    std::size_t maximum_analysis_contexts = 65536u;
    std::uint32_t source_address_begin = 0x88000000u;
    std::uint32_t source_address_end = 0x8C000000u;
};

struct LatentAotOccupiedRange {
    std::uint32_t start = 0u;
    std::uint64_t size = 0u;

    [[nodiscard]] bool operator==(const LatentAotOccupiedRange&) const = default;
};

struct PreparedLatentAotBlockIdentity {
    std::uint32_t source_offset = 0u;
    std::uint32_t size = 0u;
    std::string sha256;

    [[nodiscard]] bool operator==(const PreparedLatentAotBlockIdentity&) const = default;
};

// One exact logical disc extent which may materialize a byte-identical native
// template at runtime. The identifier is descriptor-local and carries neither
// a source path nor source bytes.
struct PreparedLatentAotSourceBinding {
    std::string id;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;

    [[nodiscard]] bool operator==(const PreparedLatentAotSourceBinding&) const = default;
};

// Export-time description of one byte template whose finite native SH-4 graph
// was accepted. Multiple exact disc extents may bind the same template. Paths,
// names and source bytes deliberately do not survive this boundary.
struct PreparedLatentAotModule {
    std::string id;
    std::string byte_identity;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<PreparedLatentAotSourceBinding> source_bindings;
    // Contains offset zero for a heuristic candidate, or exactly the accepted
    // hash-bound explicit offsets for an exact candidate. The exporter must
    // require a native source block for every listed offset before publishing
    // a loaded-module template.
    std::vector<std::uint32_t> entry_offsets;
    // Sorted identities for every uniquely emitted native IR block and exact
    // internal resume entry. Resume ranges may be nested suffixes of their IR
    // owner, but entry offsets are unique. Only hashes survive export-time
    // discovery; source bytes do not.
    std::vector<PreparedLatentAotBlockIdentity> block_identities;
    std::vector<katana::ir::Function> program;
};

struct LatentAotDiscovery {
    std::vector<PreparedLatentAotModule> modules;
    // One wall-clock sample per unique analyzed candidate, in deterministic
    // candidate order. Workers only write their own slot; reporting happens
    // after the parallel region so telemetry never races on stdout.
    std::vector<std::uint64_t> analysis_candidate_duration_ms;
    std::size_t examined_files = 0u;
    std::size_t rejected_files = 0u;
    std::size_t duplicate_files = 0u;
    std::uint64_t examined_bytes = 0u;
    std::size_t analysis_cache_positive_hits = 0u;
    std::size_t analysis_cache_negative_hits = 0u;
    std::size_t analysis_cache_misses = 0u;
    std::size_t analysis_cache_corrupt_entries = 0u;
    std::size_t analysis_cache_stores = 0u;
    // Number of candidates that entered the complete CFA/FVA/lowering
    // pipeline. Every positive candidate does so; only a current-byte
    // source-shape-derived negative rejection may bypass it.
    std::size_t analysis_full_pipeline_runs = 0u;
};

// Every address that the native backend relocates must stay inside the exact
// byte-identity extent. Otherwise a synthetic export-time base could leak into
// execution when the file is loaded at another guest address.
[[nodiscard]] bool latent_aot_program_is_relocation_closed(
    std::span<const katana::ir::Function> program,
    std::uint32_t source_start,
    std::uint32_t extent) noexcept;

[[nodiscard]] LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    std::uint32_t volume_start_lba,
    std::uint32_t extent_lba_bias,
    std::span<const std::string> excluded_byte_identities = {},
    const LatentAotDiscoveryOptions& options = {},
    std::span<const LatentAotOccupiedRange> occupied_source_ranges = {},
    std::span<const LatentAotEntryHint> entry_hints = {});

} // namespace katana::codegen
