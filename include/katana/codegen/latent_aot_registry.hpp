#pragma once

#include "katana/analysis/hardware_audit.hpp"
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
inline constexpr std::size_t
    maximum_prepared_latent_aot_external_code_pointer_candidates = 4096u;
inline constexpr std::size_t
    maximum_prepared_latent_aot_external_transfers = 4096u;
inline constexpr std::size_t
    maximum_prepared_latent_aot_code_pointer_evidence = 16'384u;
inline constexpr std::size_t
    maximum_prepared_latent_aot_function_identities = 2'048u;
inline constexpr std::uint64_t
    maximum_prepared_latent_aot_function_identity_bytes =
        64ull * 1024ull * 1024ull;

// A local, export-time-only request for a native entry in an exact disc module.
// byte_size binds the encoded disc extent. byte_identity and
// module_relative_offset bind the executable view: for an identity source
// that is the same extent, while a declared transform such as Sega PRS binds
// the decoded bytes. The offset is never inferred from mutable runtime memory.
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
    // Authoritative, byte-identity-bound entries may retain only
    // candidate-domain or ABI-stack-base inventory loss. They come either
    // from exact external hints or from a fully bounded entry table embedded
    // in the transformed module itself. Their emitted CFG remains complete
    // and RuntimeOnly dispatch still stops on every omitted target.
    ExactRuntimeOnlyStopOnMiss,
};

// An identity-bound primary-image registrar which persistently consumes one
// or more SH-4 ABI arguments as callback addresses. The bit mask uses r4..r7
// as bits 0..3. Latent modules may use this contract only to add guarded local
// entry roots; it never makes a runtime indirect target set complete.
struct LatentAotExternalCallbackSink final {
    std::uint32_t function_address = 0u;
    std::uint8_t argument_mask = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalCallbackSink&) const = default;
};

// Identity-bound primary-image function which persists an incoming 32-bit
// argument. This alone is not executable-pointer evidence. Latent discovery
// may consume it only after proving that the argument is an affine local
// record-table address and pairing the record shape with a separately proven
// callback-field sink.
struct LatentAotExternalPersistentPointerSink final {
    std::uint32_t function_address = 0u;
    std::uint8_t argument_mask = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalPersistentPointerSink&) const = default;
};

// Identity-bound primary-image record walker which loads a callback from a
// fixed-width field and invokes it indirectly. A latent module may pair this
// shape only with an exact local code literal stored through a proven
// constructor-return receiver. The result remains guarded inventory and
// never makes the walker's dynamic target set complete.
struct LatentAotExternalCallbackFieldSink final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t load_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    bool call = false;

    [[nodiscard]] bool operator==(
        const LatentAotExternalCallbackFieldSink&) const = default;
};

struct LatentAotDiscoveryOptions {
    LatentAotDiscoveryMode mode = LatentAotDiscoveryMode::HintsAndHeuristics;
    LatentAotCompletenessPolicy completeness_policy =
        LatentAotCompletenessPolicy::Strict;
    // Optional persistent, local-only negative analysis cache. The caller must place it
    // below its existing private codegen-cache root; neither this path nor a
    // cache key survives into exported product metadata. Persistent caching is
    // Each cache layer is enabled only when an exact build-bound identity is
    // present. Legacy callers that supply only analysis_implementation_identity
    // bind both layers to that stronger identity; an explicit cache identity
    // may separate the IR-cache codec from the persistent FVA epoch.
    std::filesystem::path analysis_cache_root;
    // Pure analyzer/FVA semantics and persistent epoch codec identity.
    std::string analysis_implementation_identity;
    // Latent positive/negative IR-cache codec and IR contract identity.
    std::string analysis_cache_implementation_identity;
    katana::ProgressReporter progress;
    std::size_t maximum_directory_entries = 4096u;
    std::size_t maximum_directory_bytes = 4u * 1024u * 1024u;
    std::size_t maximum_total_directory_bytes = 16u * 1024u * 1024u;
    // Maximum number of unique identity-source heuristic templates analyzed.
    // Byte-identical later extents may still be hashed within
    // maximum_total_file_bytes so their source bindings are not lost.
    std::size_t maximum_candidate_files = 128u;
    std::size_t maximum_file_bytes =
        katana::runtime::maximum_native_aot_template_extent;
    std::size_t maximum_total_file_bytes = 64u * 1024u * 1024u;
    // Compressed transform sources are streamed and discarded independently
    // from ordinary raw-file candidates. The larger sequential-I/O cap lets a
    // disc expose a broad asset family without retaining it in analysis RAM.
    std::size_t maximum_total_transform_source_bytes =
        256u * 1024u * 1024u;
    // Separate cap for deterministically transformed module bytes. This keeps
    // only admitted decoded code candidates bounded in analysis memory.
    std::size_t maximum_total_transformed_bytes = 64u * 1024u * 1024u;
    // Transformed entry-zero heuristics have an independent cardinality cap.
    // Complete embedded entry tables remain authoritative and use their own
    // fail-closed source-binding cap.  Keeping the two heuristic classes
    // separate prevents referenced raw files from crowding executable
    // compressed modules out before either class reaches full analysis.
    std::size_t maximum_transformed_candidate_files = 128u;
    std::size_t maximum_workers = 64u;
    std::size_t maximum_entry_scan_instructions = 1024u;
    std::size_t maximum_native_instructions_per_module = 32768u;
    std::size_t maximum_blocks_per_module = 8192u;
    std::size_t maximum_functions_per_module = 2048u;
    std::size_t maximum_analysis_iterations = 64u;
    std::size_t maximum_analysis_contexts = 65536u;
    std::uint32_t source_address_begin = 0x88000000u;
    std::uint32_t source_address_end = 0x8C000000u;
    // Optional sorted, unique, P1-normalized primary-image function entries.
    // Module discovery records only aligned pointer cells equal to one of these
    // identity-bound targets. The list never creates module entries by itself;
    // the exporter performs the final executable-image admission.
    std::span<const std::uint32_t> external_code_targets;
    // Sorted, unique subset of external_code_targets proven from exact
    // primary-image function boundaries and persistent callback stores.
    std::span<const LatentAotExternalCallbackSink>
        external_callback_sinks;
    // Sorted, unique subset of external_code_targets proven to persist a
    // caller-supplied word beyond the call. The module must independently
    // prove an affine local descriptor address before this contract applies.
    std::span<const LatentAotExternalPersistentPointerSink>
        external_persistent_pointer_sinks;
    // Sorted, unique field-load/call shapes from identity-bound primary code.
    std::span<const LatentAotExternalCallbackFieldSink>
        external_callback_field_sinks;
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

// Hash over one complete contiguous function extent in the transformed
// module. Unlike block_identities this is export-time provider evidence, not
// a runtime dispatch entry. Keeping it separate prevents a function boundary
// from accidentally becoming an executable root while still allowing the
// native hook audit to bind whole-function replacements in latent modules.
struct PreparedLatentAotFunctionIdentity {
    std::uint32_t source_offset = 0u;
    std::uint32_t size = 0u;
    std::string sha256;

    [[nodiscard]] bool operator==(
        const PreparedLatentAotFunctionIdentity&) const = default;
};

enum class PreparedLatentAotExternalTransferKind : std::uint8_t {
    Call,
    Jump,
};

// Exact transfer recovered from an instruction and literal inside the
// transformed module, targeting an admitted primary-image code entry. It is
// deliberately applied only after module and primary IR have been combined;
// isolated module verification must never pretend the external callee is a
// local function.
struct PreparedLatentAotExternalTransfer {
    std::uint32_t source_offset = 0u;
    std::uint32_t target_address = 0u;
    PreparedLatentAotExternalTransferKind kind =
        PreparedLatentAotExternalTransferKind::Call;

    [[nodiscard]] bool operator==(
        const PreparedLatentAotExternalTransfer&) const = default;
};

enum class PreparedLatentAotCodePointerEvidenceKind : std::uint8_t {
    // An aligned module cell points at an independently declared resident
    // code entry. The cell alone never discovers a new entry.
    DeclaredPointerCell,
    // A module instruction consumes an exact literal as its register target.
    LiteralCall,
    LiteralJump,
    // A module call forwards an exact literal through one proven callback
    // argument of an identity-bound resident higher-order API.
    CallbackArgument,
};

// Bounded, identity-scoped evidence ledger for code pointers crossing a
// latent-module boundary. Structural entry shape is deliberately not stored
// here: the product exporter validates it independently against the exact
// primary-image bytes before admitting a root. This keeps declarations,
// observed transfers and mere structural hints in separate proof domains.
struct PreparedLatentAotCodePointerEvidence {
    std::uint32_t source_offset = 0u;
    std::uint32_t target_address = 0u;
    PreparedLatentAotCodePointerEvidenceKind kind =
        PreparedLatentAotCodePointerEvidenceKind::DeclaredPointerCell;
    std::uint32_t sink_address = 0u;
    std::uint8_t argument_index = 0xFFu;

    [[nodiscard]] bool operator==(
        const PreparedLatentAotCodePointerEvidence&) const = default;
};

// Describes how the identity-bound disc extent becomes the analyzed module
// bytes. Identity is a byte-for-byte load. SegaPrs is the strict, bounded
// legacy PRS transform; its encoded and decoded identities are both retained.
enum class LatentAotSourceTransform : std::uint8_t {
    Identity,
    SegaPrs,
};

// One exact logical disc extent which may materialize a byte-identical native
// template at runtime. The identifier is descriptor-local and carries neither
// a source path nor source bytes. byte_identity belongs to the encoded source;
// PreparedLatentAotModule::byte_identity belongs to the transformed module.
struct PreparedLatentAotSourceBinding {
    std::string id;
    LatentAotSourceTransform transform = LatentAotSourceTransform::Identity;
    std::string byte_identity;
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
    // hash-bound offsets for an authoritative candidate. Authoritative roots
    // are either explicit external hints or a complete bounded entry table
    // derived from the transformed module bytes. The exporter must require a
    // native source block for every listed offset before publishing a
    // loaded-module template.
    std::vector<std::uint32_t> entry_offsets;
    // Sorted, unique P1-normalized values which point outside the module's
    // synthetic AOT extent. Values come either from aligned pointer-width
    // cells already admitted by the external declaration set or from an exact
    // PC-relative literal consumed by a register Call/Jump instruction, or
    // from an exact literal forwarded through a proven callback argument of
    // a resident higher-order API. They remain candidates: the product
    // exporter may promote a declared value, or may independently prove the
    // transfer/callback target against bounded executable primary-image
    // bytes. Keeping discovery and admission separate prevents arbitrary
    // module data from becoming code while preserving stripped cross-image
    // calls and callbacks.
    std::vector<std::uint32_t> external_code_pointer_candidates;
    std::vector<PreparedLatentAotCodePointerEvidence>
        external_code_pointer_evidence;
    std::vector<PreparedLatentAotExternalTransfer> external_transfers;
    // Sorted identities for every uniquely emitted native IR block and exact
    // internal resume entry. Resume ranges may be nested suffixes of their IR
    // owner, but entry offsets are unique. Only hashes survive export-time
    // discovery; source bytes do not.
    std::vector<PreparedLatentAotBlockIdentity> block_identities;
    // Sorted, bounded whole-function identities. These do not create roots or
    // dispatch entries and are admitted only when the final closed-CFG size
    // agrees exactly with source_offset and size.
    std::vector<PreparedLatentAotFunctionIdentity> function_identities;
    std::vector<katana::ir::Function> program;
    katana::analysis::DreamcastHardwareAudit hardware_audit;
};

struct LatentAotDiscovery {
    struct CandidateDiagnostic {
        std::uint32_t transformed_byte_size = 0u;
        std::uint32_t source_byte_size = 0u;
        std::uint32_t entry_count = 0u;
        bool transformed_source = false;
        bool admitted = false;
        std::string rejection;
        // Path- and identity-free terminal completeness dimensions. This is
        // intentionally a bounded aggregate so one private product export can
        // distinguish a real budget loss from a conservative value/stack
        // projection without exposing source names, hashes, addresses or
        // bytes.
        std::string rejection_detail;
    };

    std::vector<PreparedLatentAotModule> modules;
    // One wall-clock sample per unique analyzed candidate, in deterministic
    // candidate order. Workers only write their own slot; reporting happens
    // after the parallel region so telemetry never races on stdout.
    std::vector<std::uint64_t> analysis_candidate_duration_ms;
    // Bounded, path-free candidate outcomes. Sizes and entry counts let a
    // private product run identify a rejected transform class without
    // exporting source names, content hashes, or bytes.
    std::vector<CandidateDiagnostic> analysis_candidate_diagnostics;
    std::size_t examined_files = 0u;
    std::size_t rejected_files = 0u;
    std::size_t duplicate_files = 0u;
    std::uint64_t examined_bytes = 0u;
    // Strict Sega PRS source-transform telemetry. Decoded bytes have their own
    // total budget and are never counted as additional disc I/O bytes.
    std::size_t prs_files_examined = 0u;
    std::size_t prs_files_decoded = 0u;
    std::size_t prs_files_rejected = 0u;
    std::size_t prs_candidates_admitted = 0u;
    std::uint64_t prs_decoded_bytes = 0u;
    bool prs_decoded_budget_exhausted = false;
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

// Like the stable public overload above, but gives bounded heuristic discovery
// a deterministic priority set derived from strings in the already verified
// executable image.  References are selection hints only: unlike exact entry
// hints they never force a candidate to pass analysis.  This lets a native
// port discover late-loaded executable files beyond the generic directory
// candidate cap without turning arbitrary data-file names into trusted code.
[[nodiscard]] LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    std::uint32_t volume_start_lba,
    std::uint32_t extent_lba_bias,
    std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    std::span<const LatentAotEntryHint> entry_hints,
    std::span<const std::string> prioritized_file_references);

} // namespace katana::codegen
