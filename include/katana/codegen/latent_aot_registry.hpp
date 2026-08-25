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
#include <string_view>
#include <vector>

namespace katana::codegen {

// Discovery production and checkpoint replay validate the same bounded
// aggregate. Keep these limits shared so a producer cannot serialize a valid
// discovery that the export-time source revalidator later rejects.
inline constexpr std::size_t maximum_prepared_latent_aot_source_bindings =
    1024u;
inline constexpr std::uint64_t
    maximum_prepared_latent_aot_total_module_bytes =
        256ull * 1024ull * 1024ull;
inline constexpr std::uint64_t
    maximum_prepared_latent_aot_total_source_bytes =
        256ull * 1024ull * 1024ull;

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
    maximum_prepared_latent_aot_pc_literal_evidence = 65'536u;
inline constexpr std::size_t
    maximum_prepared_latent_aot_function_identities = 2'048u;
inline constexpr std::uint64_t
    maximum_prepared_latent_aot_function_identity_bytes =
        64ull * 1024ull * 1024ull;

// A local, export-time-only request for a native entry in an exact disc module.
// byte_size binds the encoded disc extent. byte_identity and
// module_relative_offset bind the executable view: for an identity source
// that is the same extent, while a declared transform such as Sega PRS binds
// the decoded bytes. The optional source_address pins the page-aligned
// analysis address for products whose native hook ABI already binds that
// canonical latent range. Zero retains deterministic automatic placement.
// proven_runtime_base is a separate, optional loader-proven runtime placement
// for the exact transformed module identity. It is analysis evidence only:
// it neither changes the synthetic source placement nor creates a runtime
// mapping. Zero leaves the existing bounded pointer-cluster inference in
// charge. Neither address is inferred from mutable runtime memory.
struct LatentAotEntryHint {
    std::string byte_identity;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::uint32_t module_relative_offset = 0u;
    std::uint32_t source_address = 0u;
    std::uint32_t proven_runtime_base = 0u;

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
    // Subset of argument_mask proven to receive the persisted callback's
    // record as incoming r4 when the primary-image consumer invokes it.
    std::uint8_t record_argument_mask = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalCallbackSink&) const = default;
};

// Identity-bound primary-image function which persists an incoming 32-bit
// argument. This alone is not executable-pointer evidence. Latent discovery
// may consume it only after pairing the persisted value with either an affine
// local record-table address or an exact input-alias record whose local code
// literal is written through a separately proven callback-field shape.
struct LatentAotExternalPersistentPointerSink final {
    std::uint32_t function_address = 0u;
    std::uint8_t argument_mask = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalPersistentPointerSink&) const = default;
};

// Identity-bound primary-image record walker which loads a callback from a
// fixed-width field and invokes it indirectly. A latent module may pair this
// shape only with an exact local code literal stored through a proven
// constructor-return receiver, the canonical incoming r4 record receiver, or
// an exact input alias already published through a proven persistent-pointer
// sink.
// The result remains guarded inventory and never makes the walker's dynamic
// target set complete.
struct LatentAotExternalCallbackFieldSink final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t load_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    bool call = false;
    std::uint8_t receiver_argument_mask = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalCallbackFieldSink&) const = default;
};

// Identity-bound primary-image consumer shape for callbacks stored in a
// loaded-module record array.  The primary image proves the header-pointer
// field, record stride, callback field and registrar ABI.  Discovery still
// requires an immutable primary data pointer to the concrete module header
// and a bounded terminating table in the exact candidate bytes.
struct LatentAotExternalCallbackRecordTable final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t callback_load_instruction_address = 0u;
    std::uint32_t callback_sink_address = 0u;
    std::int32_t header_table_pointer_displacement = 0;
    std::uint32_t record_stride = 0u;
    std::int32_t callback_displacement = 0;
    std::uint8_t callback_argument = 0u;
    std::uint8_t width = 0u;

    [[nodiscard]] bool operator==(
        const LatentAotExternalCallbackRecordTable&) const = default;
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
    // Latent positive/negative IR-cache codec and lowering contract identity.
    std::string analysis_cache_implementation_identity;
    // Prepared latent modules retain optimized IR. Keep their cache authority
    // tied to the optimizer without widening the primary CFA/FVA cache key.
    std::string ir_product_implementation_identity;
    // Authority-gated agent runs may import exact persistent entries, but
    // must not mutate them until their complete candidate generation has
    // passed the outer session publication gate.
    bool persistent_cache_writes_enabled = true;
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
    // Separate cap for deterministically transformed module bytes. Keep it
    // aligned with the bounded transformed-source family so a broad set of
    // identity-bound modules cannot consume the entire budget before
    // authoritative discovery examines its remaining candidates.
    std::size_t maximum_total_transformed_bytes = 256u * 1024u * 1024u;
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
    // Sorted, unique direct-mapped values found in immutable primary-image
    // words which point outside the primary image.  They are data identity
    // only and cannot create code roots without a matching record-table
    // consumer contract and candidate-local table validation.
    std::span<const std::uint32_t> external_data_targets;
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
    std::span<const LatentAotExternalCallbackRecordTable>
        external_callback_record_tables;
};

// Permanent, disc-independent diagnostic for one already decoded latent
// module.  It deliberately executes the same candidate analysis and entry
// fixpoint as product discovery; the audit is not a second decoder or a test
// heuristic.  This keeps expensive whole-disc exports out of the diagnostic
// loop while making every configured loader-tail decision machine-readable.
enum class LatentAotLoaderTailAuditStatus : std::uint8_t {
    RuntimeBaseMissing,
    RootOutOfRange,
    RootFunctionMissing,
    TerminalDelaySlotMissing,
    TerminalRegisterJumpMissing,
    PrRestoreMissing,
    EpilogueInvalid,
    TargetLiteralMissing,
    TargetUnresolved,
    RuntimeBaseIdentityMismatch,
    TargetOutOfRange,
    TargetControlFlowMissing,
    Accepted,
};

[[nodiscard]] std::string_view latent_aot_loader_tail_audit_status_name(
    LatentAotLoaderTailAuditStatus status) noexcept;

struct LatentAotLoaderTailAuditDiagnostic {
    std::size_t analysis_pass = 0u;
    std::uint32_t root_offset = 0u;
    std::uint32_t block_offset = 0u;
    std::uint32_t literal_offset = 0u;
    std::uint32_t raw_target = 0u;
    std::uint32_t target_offset = 0u;
    LatentAotLoaderTailAuditStatus status =
        LatentAotLoaderTailAuditStatus::RootFunctionMissing;
};

struct LatentAotModuleAuditResult {
    std::string byte_identity;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<std::uint32_t> initial_entry_offsets;
    std::vector<std::uint32_t> final_entry_offsets;
    // Emitted source owners are reported separately from public runtime roots.
    // An internal direct target may already be compiled without requiring a
    // second loaded-module dispatch entry.
    std::vector<std::uint32_t> emitted_block_offsets;
    std::vector<std::uint32_t> emitted_function_offsets;
    // Audit-only visibility into the last local discovery pass.  A runtime
    // base of zero means that the bounded alias vote did not select one
    // unique placement; it is never used as an execution fallback.
    std::uint32_t inferred_runtime_base = 0u;
    bool inferred_runtime_base_identity_consistent = false;
    std::vector<std::uint32_t> analyzed_function_offsets;
    std::vector<LatentAotLoaderTailAuditDiagnostic>
        loader_tail_diagnostics;
    bool admitted = false;
    std::string rejection{"none"};
    std::string rejection_detail{"none"};
};

[[nodiscard]] LatentAotModuleAuditResult audit_latent_aot_module(
    std::span<const std::uint8_t> decoded_bytes,
    std::uint32_t source_address,
    std::span<const std::uint32_t> entry_offsets,
    const LatentAotDiscoveryOptions& options = {});

[[nodiscard]] LatentAotModuleAuditResult audit_latent_aot_module(
    std::span<const std::uint8_t> decoded_bytes,
    std::uint32_t source_address,
    std::span<const std::uint32_t> entry_offsets,
    std::uint32_t proven_runtime_base,
    const LatentAotDiscoveryOptions& options = {});

// Product-parity audit for an encoded Sega PRS extent.  The registry owns the
// bounded decode and retains the encoded source identity in the candidate
// binding, exactly as whole-disc discovery does.  Callers must not decode PRS
// themselves and then present an odd-sized result as an Identity source.
[[nodiscard]] LatentAotModuleAuditResult audit_latent_aot_sega_prs_module(
    std::span<const std::uint8_t> encoded_bytes,
    std::uint32_t source_address,
    std::span<const std::uint32_t> entry_offsets,
    const LatentAotDiscoveryOptions& options = {});

[[nodiscard]] LatentAotModuleAuditResult audit_latent_aot_sega_prs_module(
    std::span<const std::uint8_t> encoded_bytes,
    std::uint32_t source_address,
    std::span<const std::uint32_t> entry_offsets,
    std::uint32_t proven_runtime_base,
    const LatentAotDiscoveryOptions& options = {});

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

enum class LoadedAotExternalTransferClassification : std::uint8_t {
    UnresolvedExactSite,
    AlreadyIdenticalResolved,
    Conflict,
};

// Discovery and export must interpret the isolated module IR identically.
// An empty exact site may receive the independently authenticated cross-image
// edge. A complete singleton edge to the same target is retained as evidence
// without rewriting its IR. Every other shape is a conflicting authority.
[[nodiscard]] inline LoadedAotExternalTransferClassification
classify_loaded_aot_external_transfer(
    const PreparedLatentAotExternalTransfer& transfer,
    const katana::ir::Instruction& instruction,
    const katana::ir::BasicBlock& block) noexcept {
    const auto expected_operation =
        transfer.kind == PreparedLatentAotExternalTransferKind::Call
            ? katana::ir::Operation::CallRegister
            : katana::ir::Operation::JumpRegister;
    if (instruction.operation != expected_operation ||
        instruction.branch_register_relative)
        return LoadedAotExternalTransferClassification::Conflict;

    if (instruction.resolved_targets.empty()) {
        return block.has_indirect_successor
                   ? LoadedAotExternalTransferClassification::
                         UnresolvedExactSite
                   : LoadedAotExternalTransferClassification::Conflict;
    }

    const bool complete_target_class =
        instruction.dynamic_target_class ==
            katana::ir::DynamicTargetClass::GuardedComplete ||
        instruction.dynamic_target_class ==
            katana::ir::DynamicTargetClass::ExactGuarded;
    if (instruction.resolved_targets.size() == 1u &&
        instruction.resolved_targets.front() == transfer.target_address &&
        complete_target_class && !block.has_indirect_successor)
        return LoadedAotExternalTransferClassification::
            AlreadyIdenticalResolved;

    return LoadedAotExternalTransferClassification::Conflict;
}

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

// One exact PC-relative scalar consumed by authenticated module IR.  The
// parent module identity binds the bytes; retaining only this bounded ledger
// lets later owner/provider proof recover immutable literals without keeping
// retail source bytes in the analysis artifact.
struct PreparedLatentAotPcLiteralEvidence final {
    std::uint32_t instruction_offset = 0u;
    std::uint32_t literal_offset = 0u;
    std::uint32_t bits = 0u;
    std::uint8_t width_bytes = 0u;
    bool signed_value = false;

    [[nodiscard]] bool operator==(
        const PreparedLatentAotPcLiteralEvidence&) const = default;
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
    // Sorted by instruction/literal offset.  Every entry is regenerated from
    // the validated transformed module and rechecked against the current disc
    // binding before admission or resume publication.
    std::vector<PreparedLatentAotPcLiteralEvidence> pc_literal_evidence;
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
    // source-shape-derived negative rejection or an exact in-session terminal
    // candidate-value-inventory rejection may bypass it.
    std::size_t analysis_full_pipeline_runs = 0u;
};

// In-process state for one analyze-port discovery run.  The session owns the
// bounded source catalog and source-byte-backed candidate work plus a
// non-persistent cache of source-bound static candidate graphs. It must be
// discarded at the end of a run. A later discovery call may reuse that catalog
// and graph only when the exact source/placement/hint/static analyzer key
// matches. Cross-image contracts remain call-local: monotonic additions are
// re-resolved against the retained graph, while removals, identity changes or
// a newly required CFA root take the closed cold path.
class LatentAotDiscoverySession final {
  public:
    struct Impl;

    LatentAotDiscoverySession();
    ~LatentAotDiscoverySession();
    LatentAotDiscoverySession(const LatentAotDiscoverySession&) = delete;
    LatentAotDiscoverySession& operator=(
        const LatentAotDiscoverySession&) = delete;
    LatentAotDiscoverySession(LatentAotDiscoverySession&&) noexcept;
    LatentAotDiscoverySession& operator=(
        LatentAotDiscoverySession&&) noexcept;

    // Drops all in-process catalog state.  No source bytes or analysis
    // products survive this boundary.
    void reset() noexcept;

  private:
    std::unique_ptr<Impl> impl_;

    friend LatentAotDiscovery discover_latent_aot_modules(
        std::shared_ptr<const katana::runtime::DiscSource> source,
        std::uint32_t volume_start_lba,
        std::uint32_t extent_lba_bias,
        std::span<const std::string> excluded_byte_identities,
        const LatentAotDiscoveryOptions& options,
        std::span<const LatentAotOccupiedRange> occupied_source_ranges,
        std::span<const LatentAotEntryHint> entry_hints,
        std::span<const std::string> prioritized_file_references,
        LatentAotDiscoverySession& session);
};

// Every address that the native backend relocates must stay inside the exact
// byte-identity extent. Otherwise a synthetic export-time base could leak into
// execution when the file is loaded at another guest address.
[[nodiscard]] bool latent_aot_program_is_relocation_closed(
    std::span<const katana::ir::Function> program,
    std::uint32_t source_start,
    std::uint32_t extent) noexcept;

// Rebinds a prepared positive discovery to the current immutable disc bytes.
// Every encoded source identity, deterministic transform, decoded module,
// entry/block/function identity and original SH-4 opcode is checked. No
// analyzer result is inferred or repaired; any mismatch is a clean false.
[[nodiscard]] bool validate_latent_aot_discovery_source_binding(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const LatentAotDiscovery& discovery) noexcept;

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

// Session-aware overload for repeated discovery calls within one
// analyze-port cross-image/fixpoint run.  The caller must keep `session`
// alive for the complete run and reset/destroy it before changing the source,
// placement or discovery contract.  Dynamic external targets/sinks remain
// call-local and are never treated as persistent authority.
[[nodiscard]] LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    std::uint32_t volume_start_lba,
    std::uint32_t extent_lba_bias,
    std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    std::span<const LatentAotEntryHint> entry_hints,
    std::span<const std::string> prioritized_file_references,
    LatentAotDiscoverySession& session);

} // namespace katana::codegen
