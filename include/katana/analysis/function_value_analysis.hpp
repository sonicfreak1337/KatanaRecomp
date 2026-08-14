#pragma once

#include "katana/analysis/abi.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

// Versioned semantic views of one function evaluation. FullState is the
// fail-closed fallback whenever a register, stack, memory or inventory
// dependency cannot be proven complete. The remaining values are product
// lenses and therefore participate in the exact cache-key schema.
enum class EvaluationLens : std::uint8_t {
    FullState,
    Summary,
    CandidateContract,
    GuardedInventory,
    ContextualReturn,
    IsolatedObservation,
    Count,
};

inline constexpr std::uint32_t evaluation_lens_schema_version = 4u;
inline constexpr std::size_t evaluation_lens_count =
    static_cast<std::size_t>(EvaluationLens::Count);

[[nodiscard]] constexpr std::string_view evaluation_lens_name(
    const EvaluationLens lens) noexcept {
    switch (lens) {
    case EvaluationLens::FullState: return "full-state";
    case EvaluationLens::Summary: return "summary";
    case EvaluationLens::CandidateContract: return "candidate-contract";
    case EvaluationLens::GuardedInventory: return "guarded-inventory";
    case EvaluationLens::ContextualReturn: return "contextual-return";
    case EvaluationLens::IsolatedObservation: return "isolated-observation";
    case EvaluationLens::Count: break;
    }
    return "unknown";
}

// Run-local reason why the optional persistent resolution-root epoch was not
// retained. This is cache policy only: it never changes canonical analysis,
// artifact identity, ABI identity, or fail-closed product semantics.
enum class ResolutionRetentionLimitReason : std::uint8_t {
    None,
    DependencyNodeLimit,
    RootEntryLimit,
    ByteLimit,
    IncompleteRoot,
};

// A persistent-state bypass is stronger than a cache miss: the complete
// invocation must behave as a new session for ProgramGraph, ABI contracts,
// summaries, candidate inputs, function evaluations and resolution roots.
// The reason is run-local evidence and never enters canonical output.
enum class PersistentAnalysisBypassReason : std::uint8_t {
    None,
    FunctionBoundaryChanged,
    RecursiveBaselineRejected,
    ProgramDeltaUnrepresentable,
    ResolutionDependencyUnrepresentable,
    ExplicitTest,
};

enum class FunctionValueResultMaterialization : std::uint8_t {
    // Intermediate monotone Candidate/CFG rounds expose only changed semantic
    // shards and never rebuild the complete public result vectors.
    DeltaOnly,
    // Presentation-only snapshot of the already published epoch. This mode
    // must not start or publish another semantic analysis epoch.
    TerminalFull,
};

[[nodiscard]] constexpr std::string_view
persistent_analysis_bypass_reason_name(
    const PersistentAnalysisBypassReason reason) noexcept {
    switch (reason) {
    case PersistentAnalysisBypassReason::None: return "none";
    case PersistentAnalysisBypassReason::FunctionBoundaryChanged:
        return "function-boundary-changed";
    case PersistentAnalysisBypassReason::RecursiveBaselineRejected:
        return "recursive-baseline-rejected";
    case PersistentAnalysisBypassReason::ProgramDeltaUnrepresentable:
        return "program-delta-unrepresentable";
    case PersistentAnalysisBypassReason::ResolutionDependencyUnrepresentable:
        return "resolution-dependency-unrepresentable";
    case PersistentAnalysisBypassReason::ExplicitTest:
        return "explicit-test";
    }
    return "unknown";
}

enum class FunctionValueDependencyNodeKind : std::uint8_t {
    Function,
    InventoryRegion,
    AnalysisBaseline,
};

struct FunctionValueDependencyNodeId final {
    std::uint32_t address = 0u;
    FunctionValueDependencyNodeKind kind =
        FunctionValueDependencyNodeKind::Function;

    bool operator==(const FunctionValueDependencyNodeId&) const = default;
    bool operator<(const FunctionValueDependencyNodeId& other) const noexcept {
        if (address != other.address) return address < other.address;
        return kind < other.kind;
    }
};

[[nodiscard]] constexpr std::string_view
resolution_retention_limit_reason_name(
    const ResolutionRetentionLimitReason reason) noexcept {
    switch (reason) {
    case ResolutionRetentionLimitReason::None: return "none";
    case ResolutionRetentionLimitReason::DependencyNodeLimit:
        return "dependency-node-limit";
    case ResolutionRetentionLimitReason::RootEntryLimit:
        return "root-entry-limit";
    case ResolutionRetentionLimitReason::ByteLimit:
        return "byte-limit";
    case ResolutionRetentionLimitReason::IncompleteRoot:
        return "incomplete-root";
    }
    return "unknown";
}

// Run-local observability only. These counters never enter canonical analysis
// output, cache keys or product identities.
struct EvaluationLensTelemetry {
    std::array<std::size_t, evaluation_lens_count> requests{};
    std::array<std::size_t, evaluation_lens_count> cache_hits{};
    // Ready/in-flight reuse is valued with the producer's measured physical
    // miss-compute duration. Current hit wall time is deliberately not used as
    // a proxy for work which did not run.
    std::array<std::uint64_t, evaluation_lens_count>
        avoided_evaluation_nanoseconds{};
    std::size_t full_state_fallbacks = 0u;
    std::size_t projected_evaluations = 0u;
    std::size_t reconstructed_results = 0u;
    std::size_t key_interned_sets = 0u;
    std::size_t key_interned_references = 0u;
};

// A bounded, inventory-only carrier for a finite callback payload whose exact
// stack or memory cell identity has been widened away.  It is deliberately
// distinct from a value's ordinary scalar domain: materialization makes it a
// guarded, incomplete candidate only at an observing load or ABI boundary.
// The vectors are canonical sorted sets and are limited by the analysis-wide
// guarded inventory bound.  Truncation is genuine Top; a finite carrier is not.
struct InventoryCandidateCarrier {
    std::vector<std::uint32_t> inventory_code_pointer_values;
    std::vector<std::uint32_t> inventory_pc_relative_code_literal_values;
    // A finite ordinary stack scalar whose exact slot was folded away before
    // a later, independently proven ABI boundary could validate/promote it.
    // It is intentionally not a code-pointer domain by itself.
    std::vector<std::uint32_t> pending_abi_scalar_values;
    bool inventory_code_pointer_values_truncated = false;
    bool inventory_pc_relative_code_literal_values_truncated = false;
    bool pending_abi_scalar_values_truncated = false;
    bool contextual_candidate_dependency = false;
    std::set<std::uint32_t> call_sites;
    std::set<std::uint32_t> callees;

    bool operator==(const InventoryCandidateCarrier&) const = default;
};

struct InventorySavedStackEpochSummary;

// The nested vector is intentionally value-semantic.  A saved-SP pointer in
// a saved stack slot remains a pointer to another epoch; it is never folded
// into the outer slot's direct callback carrier.
struct InventorySavedStackSlotSummary {
    std::int32_t relative_slot = 0;
    InventoryCandidateCarrier carrier;
    std::vector<InventorySavedStackEpochSummary> nested_epochs;

    bool operator==(const InventorySavedStackSlotSummary&) const = default;
};

struct InventorySavedStackEpochSummary {
    bool present = false;
    bool unresolved = false;
    bool tracks_current_epoch = false;
    bool candidate_payload_lost = false;
    // Depth-capped nested saved-SP lineage with no candidate payload. This is
    // an alias fact, deliberately distinct from candidate_payload_lost.
    bool nested_saved_stack_alias_latent = false;
    bool nested_saved_stack_alias_tracks_current_epoch = false;
    std::vector<InventorySavedStackSlotSummary> slots;
    InventoryCandidateCarrier unresolved_candidate_carrier;
    // Outer coordinate loss must not turn an inner saved-SP into a direct
    // callback.  Keep the bounded nested epoch alternatives separately.
    std::vector<InventorySavedStackEpochSummary> unresolved_nested_epochs;

    bool operator==(const InventorySavedStackEpochSummary&) const = default;
};

struct FunctionRegisterValueSummary {
    std::uint8_t register_index = 0u;
    bool complete = false;
    bool guarded = false;
    bool abi_preserved = false;
    bool may_alias_stack = true;
    // Inventory-only return provenance.  These fields never prove a static
    // control-flow edge; they only preserve guarded native-entry evidence
    // across an ordinary helper return.
    bool inventory_code_pointer = false;
    bool inventory_pc_relative_code_literal = false;
    std::vector<std::uint32_t> inventory_code_pointer_values;
    std::vector<std::uint32_t> inventory_pc_relative_code_literal_values;
    bool inventory_code_pointer_values_truncated = false;
    bool inventory_pc_relative_code_literal_values_truncated = false;
    // Internal candidate-return slice dependency. It is not control-flow or
    // code-pointer evidence; it only decides whether a direct helper needs a
    // contextual summary instead of its already authoritative global summary.
    bool contextual_candidate_dependency = false;
    // Inventory-only fail-closed provenance. The concrete return value may be
    // a callback whose finite stack candidate was lost during analysis.
    bool inventory_stack_callback_loss_unresolved = false;
    // Payload-free saved-stack lineage is not a lost callback. It becomes a
    // fail-closed loss only if that same current epoch later receives a
    // relevant callback candidate.
    bool inventory_saved_stack_alias_latent = false;
    bool inventory_saved_stack_alias_tracks_current_epoch = false;
    InventorySavedStackEpochSummary inventory_saved_stack_epoch;
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> return_sites;
    std::vector<std::uint32_t> evidence_callees;
    std::string reason;

    bool operator==(const FunctionRegisterValueSummary&) const = default;
};

struct FunctionMemoryValueSummary {
    std::uint32_t address = 0u;
    bool complete = false;
    bool guarded = false;
    // Address-scoped counterpart of the register provenance above. It remains
    // attached to this exact memory cell across a function return.
    bool inventory_stack_callback_loss_unresolved = false;
    bool inventory_saved_stack_alias_latent = false;
    bool inventory_saved_stack_alias_tracks_current_epoch = false;
    // Address-scoped unknown values can still carry a finite guarded
    // inventory payload. Keep that payload through a function summary instead
    // of dropping it merely because `values` is empty.
    InventoryCandidateCarrier inventory_candidate_carrier;
    InventorySavedStackEpochSummary inventory_saved_stack_epoch;
    std::vector<std::uint32_t> values;

    bool operator==(const FunctionMemoryValueSummary&) const = default;
};

struct FunctionMemoryWriteRange {
    std::uint32_t address = 0u;
    std::uint32_t width = 0u;

    auto operator<=>(const FunctionMemoryWriteRange&) const = default;
};

struct FunctionValueSummary {
    std::uint32_t function_address = 0u;
    std::vector<FunctionRegisterValueSummary> registers;
    bool memory_complete = false;
    // Relative memory effect. An unknown write invalidates every caller fact;
    // otherwise only facts overlapping one of these bounded byte ranges may
    // be replaced by memory_values. Omitted, untouched cells are preserved.
    bool memory_write_unknown = false;
    std::vector<FunctionMemoryWriteRange> memory_write_ranges;
    // Read-before-definition dependency of this function, including callees.
    // `memory_read_complete == false` is the monotone fixpoint's undiscovered
    // value and is never projectable. A complete unknown contract is Top;
    // otherwise the normalized ranges are the exact external byte facts read.
    bool memory_read_complete = false;
    bool memory_read_unknown = false;
    std::vector<FunctionMemoryWriteRange> memory_read_ranges;
    std::vector<FunctionMemoryValueSummary> memory_values;
    // Domain-scoped may-carriers. They survive an imprecise stack or memory
    // address and are observed only by compatible reads; complete-empty read
    // contracts may intentionally remove the corresponding domain.
    InventoryCandidateCarrier inventory_unresolved_stack_carrier;
    InventoryCandidateCarrier inventory_unresolved_memory_carrier;
    InventorySavedStackEpochSummary inventory_unresolved_stack_epoch;
    InventorySavedStackEpochSummary inventory_unresolved_memory_epoch;
    // Evaluation-local effect on aliases that still track the caller's
    // current stack epoch.  This is deliberately separate from the returned
    // unresolved stack domain: the latter is a may-read result state, while
    // this bounded delta is replayed only into caller aliases at apply_call.
    InventoryCandidateCarrier current_stack_epoch_mutation_carrier;
    InventorySavedStackEpochSummary current_stack_epoch_mutation_epoch;
    bool current_stack_epoch_mutation_callback_loss = false;
    // Bounded top for payload-free aliases whose exact storage identity was
    // widened away inside this function (stack=1, memory=2).
    std::uint8_t inventory_unresolved_saved_stack_alias_sources = 0u;
    // Current-epoch tracking is scoped to the same lost storage domain as
    // `inventory_unresolved_saved_stack_alias_sources` (stack=1, memory=2).
    // A single shared bool would let a stack-only mutation poison a retained
    // memory alias (and vice versa).
    std::uint8_t inventory_unresolved_saved_stack_alias_tracks_current_sources = 0u;
    bool inventory_unresolved_stack_callback_loss = false;
    bool inventory_unresolved_memory_callback_loss = false;
    // Bounded storage-identity loss is scoped to the domain that lost the
    // exact key (stack=1, memory=2).  A stack switch discards only stack.
    std::uint8_t inventory_callback_loss_identity_truncated_sources = 0u;

    bool operator==(const FunctionValueSummary&) const = default;
};

struct FunctionValueSummaryShard final {
    FunctionValueDependencyNodeId owner;
    FunctionValueSummary summary;
};

struct InterproceduralTargetResolution {
    std::uint32_t instruction_address = 0u;
    std::uint8_t register_index = 0u;
    bool call = false;
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> call_sites;
    std::vector<std::uint32_t> callees;
    bool guarded = false;
    bool complete = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::string reason;

    bool operator==(const InterproceduralTargetResolution&) const = default;
};

struct FunctionValueResolutionShard final {
    FunctionValueDependencyNodeId owner;
    std::vector<InterproceduralTargetResolution> resolutions;
};

// A finite code address observed either through a non-stack 32-bit memory
// operation with guest-call provenance or in an identity-bound static vector
// of independently valid function entries. The legacy source-address field
// therefore carries an instruction address or the exact static table slot.
// This is only native-inventory evidence and never a concrete dispatch edge.
struct StoredCodeAddressCandidate {
    std::uint32_t target_address = 0u;
    bool complete = false;
    bool guarded = true;
    std::vector<std::uint32_t> store_instruction_addresses;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;

    bool operator==(const StoredCodeAddressCandidate&) const = default;
};

// A bounded initial-snapshot pointer table reached through a finite
// interprocedural return value and then used as the base of a 32-bit load.
// Entries are guarded native-inventory candidates only; the live load remains
// authoritative at runtime.
struct ReturnedCodeAddressTableCandidate {
    std::uint32_t table_address = 0u;
    std::vector<std::uint32_t> target_addresses;
    std::vector<std::uint32_t> load_instruction_addresses;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;
    bool scan_truncated = false;

    bool operator==(const ReturnedCodeAddressTableCandidate&) const = default;
};

enum class ForwardedStoreContextLimitReason : std::uint8_t {
    RootCallSites,
    ContextCount,
    ReevaluationCount,
};

// One bounded terminal capsule per resolution owner. It is analysis-only and
// exists solely to explain a fail-closed export; it never participates in the
// runtime product path or in candidate selection.
struct ForwardedStoreContextLimitDiagnostic {
    std::uint32_t owner_entry = 0u;
    std::uint32_t target = 0u;
    std::uint32_t exemplar_root_call_site = 0u;
    std::size_t context_count = 0u;
    std::size_t root_call_site_count = 0u;
    std::size_t evaluation_count = 0u;
    bool tail = false;
    bool isolated = false;
    ForwardedStoreContextLimitReason reason =
        ForwardedStoreContextLimitReason::ContextCount;

    bool operator==(const ForwardedStoreContextLimitDiagnostic&) const = default;
};

// Bounded inventory walks have several independent resource contracts. Keep
// their terminal evidence separate: an otherwise small candidate inventory
// must not be reported as if the 1,024-entry collector itself overflowed.
// Counts describe unique bounded units, never individual rejected attempts.
struct GuardedCodeInventoryWalkDiagnostics {
    std::size_t inventory_region_count = 0u;
    std::size_t inventory_region_budget = 0u;
    std::size_t pending_inventory_region_count = 0u;
    std::size_t inventory_region_block_budget = 0u;
    std::size_t inventory_region_block_limited_regions = 0u;
    std::size_t forwarded_store_context_budget = 0u;
    std::size_t forwarded_store_context_limited_functions = 0u;
    // Run-local coordinator reuse is an optimization only. A hit is a logical
    // subscriber that reused a ready or in-flight coordinator artifact; a
    // miss is the first producer for that exact context, independently of
    // whether the persistent session cache serves that producer. These
    // counters are scheduling- and run-local, not canonical analysis output.
    std::size_t forwarded_store_evaluation_cache_hits = 0u;
    std::size_t forwarded_store_evaluation_cache_misses = 0u;
    std::vector<ForwardedStoreContextLimitDiagnostic>
        forwarded_store_context_limit_diagnostics;
    std::size_t contextual_return_context_budget = 0u;
    std::size_t contextual_return_context_limited_functions = 0u;
    std::size_t contextual_return_evaluation_budget = 0u;
    std::size_t contextual_return_evaluation_limited_functions = 0u;
    // Provenance replay has its own root-local resource contracts. It can
    // fail a root without exhausting the semantic contextual-evaluation
    // budget, so keep both terminal causes independently observable.
    std::size_t contextual_provenance_replay_capsule_budget = 0u;
    std::size_t contextual_provenance_replay_capsule_limited_functions = 0u;
    std::size_t contextual_provenance_replay_key_byte_budget = 0u;
    std::size_t contextual_provenance_replay_key_byte_limited_functions = 0u;
    std::size_t abi_stack_argument_slot_budget = 0u;
    std::size_t abi_stack_argument_projection_truncated_functions = 0u;
    // A local CFG transfer must return control to the interprocedural budget.
    // Reaching this cap is a fail-closed analysis loss, not a performance
    // counter: otherwise one malformed lattice edge can spin forever inside a
    // single function and bypass every outer fixpoint guard.
    std::size_t local_fixpoint_iteration_budget = 0u;
    std::size_t local_fixpoint_limited_evaluations = 0u;
    // Peak number of local CFG block evaluations in one function analysis.
    // This remains diagnostic while the explicit limit above is not reached.
    std::size_t maximum_local_fixpoint_iterations = 0u;
    // Exact contribution partitions share one logical per-root budget.  The
    // first exhausted category is scheduling-dependent, so expose one
    // canonical fail-closed bit instead of publishing the winning worker's
    // category as semantic output.
    bool resolution_root_logical_budget_exhausted = false;
    bool inventory_candidate_values_truncated = false;
    bool abi_stack_base_unresolved = false;
    // A tail edge carried inventory-relevant state to a target which could
    // not be bound as either an ordinary function or an inventory region.
    // Silently dropping that edge would make product completeness unsound.
    bool inventory_tail_target_unresolved = false;

    [[nodiscard]] constexpr bool
    truncated_except_candidate_stack_resolution_loss() const noexcept {
        return pending_inventory_region_count != 0u ||
               inventory_region_block_limited_regions != 0u ||
               forwarded_store_context_limited_functions != 0u ||
               contextual_return_context_limited_functions != 0u ||
               contextual_return_evaluation_limited_functions != 0u ||
               contextual_provenance_replay_capsule_limited_functions !=
                   0u ||
               contextual_provenance_replay_key_byte_limited_functions !=
                   0u ||
               abi_stack_argument_projection_truncated_functions != 0u ||
               local_fixpoint_limited_evaluations != 0u ||
               resolution_root_logical_budget_exhausted ||
               inventory_tail_target_unresolved;
    }

    [[nodiscard]] constexpr bool truncated() const noexcept {
        return truncated_except_candidate_stack_resolution_loss() ||
               inventory_candidate_values_truncated ||
               abi_stack_base_unresolved;
    }

    bool operator==(const GuardedCodeInventoryWalkDiagnostics&) const = default;
};

// A larger, separately bounded native-code inventory channel.  Its entries
// never become fixed CFG edges; the live runtime value remains authoritative.
// The ordinary abstract-value domain intentionally retains its much smaller
// dataflow bound.
struct GuardedCodeInventory {
    std::vector<StoredCodeAddressCandidate> stored_code_addresses;
    std::vector<ReturnedCodeAddressTableCandidate> returned_code_address_tables;
    std::size_t raw_stored_candidate_budget = 0u;
    std::size_t raw_stored_candidate_count = 0u;
    std::size_t candidate_budget = 0u;
    std::size_t candidate_count = 0u;
    std::size_t shape_validation_work = 0u;
    std::size_t shape_validation_work_budget = 0u;
    std::size_t shape_budget_exceeded_candidates = 0u;
    bool raw_stored_candidates_truncated = false;
    bool candidate_budget_exhausted = false;
    bool candidate_inventory_truncated = false;
    bool table_scan_truncated = false;
    GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
};

// Exact DeltaOnly ownership unit. Replacing a root shard replaces both its
// candidate evidence and its walk diagnostics; consumers must not append the
// flattened inventory because candidate-contract withdrawal is observable.
struct FunctionValueGuardedInventoryShard final {
    FunctionValueDependencyNodeId owner;
    GuardedCodeInventory inventory;
};

namespace detail {

enum class GuardedCodeInventoryPriorityKind : std::uint8_t {
    CompleteStored,
    IncompleteStored,
    CompleteReturnedTable,
    TruncatedReturnedTable,
};

struct GuardedCodeInventoryPriorityTarget {
    std::uint32_t target_address = 0u;
    GuardedCodeInventoryPriorityKind kind =
        GuardedCodeInventoryPriorityKind::IncompleteStored;
};

struct AbiContractObservation {
    std::uint32_t function_address = 0u;
    bool stack_reads_complete = false;
    std::span<const std::int32_t> stack_read_slots;
    std::uint8_t persistent_store_sources = 0u;
};

using AbiContractObserver =
    std::function<void(const AbiContractObservation&)>;

[[nodiscard]] std::vector<std::uint32_t>
guarded_code_inventory_priority_order(
    std::span<const GuardedCodeInventoryPriorityTarget> candidates,
    std::size_t returned_table_reserve);

} // namespace detail

struct FunctionValueAnalysisResult {
    FunctionValueResultMaterialization result_materialization =
        FunctionValueResultMaterialization::TerminalFull;
    std::vector<FunctionValueSummary> summaries;
    std::vector<InterproceduralTargetResolution> resolutions;
    // DeltaOnly replacement/deletion ledgers. The typed owner is the sole
    // authoritative key for replacement and withdrawal.
    std::vector<FunctionValueSummaryShard> summary_replacements;
    std::vector<FunctionValueDependencyNodeId> removed_summary_shards;
    std::vector<FunctionValueResolutionShard> resolution_replacements;
    std::vector<FunctionValueDependencyNodeId>
        removed_resolution_shards;
    std::vector<FunctionValueGuardedInventoryShard>
        guarded_code_inventory_replacements;
    std::vector<FunctionValueDependencyNodeId>
        removed_guarded_code_inventory_shards;
    GuardedCodeInventory guarded_code_inventory;
    std::size_t fixpoint_iterations = 0u;
    std::size_t strongly_connected_components = 0u;
    std::size_t unchanged_ingress_skips = 0u;
    // Scheduler telemetry is deliberately run-local. It may be inspected by
    // tests and live progress reporting, but must not enter canonical analysis
    // reports, product metadata, cache keys, or artifact identities: worker
    // count and stale speculative work vary without changing the semantics.
    std::size_t fixpoint_worker_count = 1u;
    std::size_t fixpoint_parallel_batches = 0u;
    std::size_t fixpoint_speculative_evaluations = 0u;
    std::size_t fixpoint_stale_repairs = 0u;
    std::size_t maximum_fixpoint_batch_size = 1u;
    std::size_t iteration_budget = 0u;
    bool budget_exhausted = false;
    // Observational callback failures never change canonical analysis, but
    // callers can propagate the sticky telemetry-loss state.
    bool progress_callback_failed = false;
    // Run-local KR-4978 evidence. These address sets are observational and
    // never participate in canonical reports, artifacts, or cache keys.
    std::vector<FunctionValueDependencyNodeId>
        incremental_dirty_scc_entries;
    std::vector<std::uint32_t> incremental_dirty_functions;
    std::vector<std::uint32_t> incremental_dirty_inventory_sinks;
    std::vector<std::uint32_t> resolution_root_artifacts_reused;
    std::vector<std::uint32_t> resolution_root_artifacts_recomputed;
    std::size_t resolution_root_artifacts_retained = 0u;
    std::size_t resolution_epoch_retained_bytes = 0u;
    ResolutionRetentionLimitReason resolution_retention_limit_reason =
        ResolutionRetentionLimitReason::None;
    std::size_t full_cpu_recompute_fallbacks = 0u;
    // Non-None is also the DeltaOnly consumer-reset signal. The producer has
    // ignored every previously published semantic shard and the typed
    // replacement vectors therefore form the complete current cold snapshot;
    // consumers must discard all old owner shards before applying them.
    PersistentAnalysisBypassReason persistent_analysis_bypass_reason =
        PersistentAnalysisBypassReason::None;
    std::size_t program_delta_entries_visited = 0u;
    std::size_t function_edge_full_scans = 0u;
    std::size_t function_edge_full_sorts = 0u;
    std::size_t candidate_call_edge_full_scans = 0u;
    std::size_t candidate_call_edge_full_sorts = 0u;
    std::size_t candidate_tail_edge_full_scans = 0u;
    std::size_t candidate_tail_edge_full_sorts = 0u;
    std::size_t program_graph_blocks_built = 0u;
    std::size_t program_graph_blocks_reused = 0u;
    std::size_t program_graph_sccs_built = 0u;
    std::size_t program_graph_sccs_reused = 0u;
    // Physical ResolutionDependency work only. Untouched path-shared shards
    // count in neither column; a shard is reused only after this invocation
    // physically inspects it and retains its exact immutable object.
    std::size_t resolution_dependency_nodes_built = 0u;
    std::size_t resolution_dependency_nodes_reused = 0u;
    std::size_t resolution_dependency_sccs_built = 0u;
    std::size_t resolution_dependency_sccs_reused = 0u;
    std::size_t abi_contract_entries_visited = 0u;
    std::size_t abi_contract_entries_rebuilt = 0u;
    std::size_t summary_candidate_entries_visited = 0u;
    std::size_t summary_candidate_entries_rebuilt = 0u;
    std::size_t inventory_topology_entries_visited = 0u;
    std::size_t resolution_preparation_entries_visited = 0u;
    std::size_t final_materialized_blocks = 0u;
    std::size_t final_materialized_functions = 0u;
};

// Run-local, scalar-only D1 telemetry for one Contextual-Return root. These
// values are observational: they never participate in semantic identities,
// cache keys, budgets, artifacts, or persisted analysis state. Root-scoped
// values are selected from the current head-of-line root by the progress
// transport; no per-lane state or raw digest is exposed. A degraded/drop-only
// record deliberately leaves the root identity absent.
struct ContextualReturnD1Telemetry final {
    std::optional<std::size_t> root_index;
    std::optional<std::uint32_t> root_address;
    std::uint32_t current_function_address = 0u;
    std::uint32_t limiting_function_address = 0u;
    std::size_t wave = 0u;
    std::size_t frontier = 0u;
    std::size_t maximum_frontier = 0u;
    std::size_t context_budget = 0u;
    std::size_t evaluation_budget = 0u;
    std::size_t contexts_admitted = 0u;
    std::size_t evaluations_admitted = 0u;
    bool context_budget_exhausted = false;
    bool evaluation_budget_exhausted = false;
    bool composite_budget_exhausted = false;
    bool incomplete_root = false;
    bool retention_enabled = false;
    std::size_t retained_bytes = 0u;
    std::size_t logical_requests = 0u;
    std::size_t logical_admissions = 0u;
    std::size_t semantic_lane_cardinality = 0u;
    std::size_t physical_evaluations = 0u;
    std::size_t cache_reuses = 0u;
    std::size_t exact_subscriber_cardinality = 0u;
    std::size_t provenance_cardinality = 0u;
    std::size_t root_lane_creations = 0u;
    std::size_t descendant_lane_creations = 0u;
    std::size_t requeues_initial_root_seed = 0u;
    std::size_t requeues_new_lane = 0u;
    std::size_t requeues_input_widening = 0u;
    std::size_t requeues_summary_change = 0u;
    std::size_t requeues_forward_edge_insert_or_widen = 0u;
    std::size_t requeues_stale_dependency = 0u;
    std::size_t stale_snapshot_discards = 0u;
    std::size_t snapshot_count = 0u;
    std::uint64_t snapshot_nanoseconds = 0u;
    std::size_t key_count = 0u;
    std::uint64_t key_nanoseconds = 0u;
    std::size_t cache_evaluation_count = 0u;
    // Inclusive end-to-end cache request time: key construction, cache wait,
    // and any physical evaluation, including nested apply/binding work.
    std::uint64_t cache_evaluation_nanoseconds = 0u;
    std::size_t apply_call_count = 0u;
    // Inclusive apply_call time; binding_merge_nanoseconds is nested in it.
    // Snapshot, FullState-key, Evidence-restore, serial-commit, and publish
    // timings remain separate sibling domains.
    std::uint64_t apply_call_nanoseconds = 0u;
    std::size_t binding_lookups = 0u;
    std::size_t bindings_examined = 0u;
    std::size_t binding_equality_attempts = 0u;
    std::size_t binding_merge_attempts = 0u;
    std::uint64_t binding_merge_nanoseconds = 0u;
    std::size_t binding_exact_hits = 0u;
    std::size_t binding_join_hits = 0u;
    std::size_t maximum_binding_count = 0u;
    std::size_t maximum_binding_hit_position = 0u;
    std::size_t evidence_restore_count = 0u;
    std::uint64_t evidence_restore_nanoseconds = 0u;
    std::size_t serial_commit_count = 0u;
    std::uint64_t serial_commit_nanoseconds = 0u;
    std::size_t publish_count = 0u;
    std::uint64_t publish_nanoseconds = 0u;
    std::size_t maximum_full_state_key_bytes = 0u;
    std::size_t maximum_projected_key_bytes = 0u;
    std::size_t maximum_capsule_entries = 0u;
    std::size_t projected_digest_cardinality = 0u;
    std::size_t projected_digest_dropped = 0u;
    bool projected_digest_degraded = false;
    std::size_t alpha_normalization_fallbacks = 0u;
    std::size_t semantic_lane_widenings = 0u;
    std::size_t provenance_only_lane_widenings = 0u;
    std::size_t lane_widening_classification_dropped = 0u;
    bool lane_widening_classification_degraded = false;
    std::size_t telemetry_dropped = 0u;
    bool telemetry_degraded = false;
};

struct FunctionValueAnalysisProgress {
    // Owned because callbacks commonly enqueue or retain snapshots after the
    // synchronous producer call returns.
    std::string phase;
    std::string subphase;
    std::size_t subphase_planned = 0u;
    std::size_t subphase_processed = 0u;
    std::size_t subphase_queued = 0u;
    std::size_t subphase_iterations = 0u;
    std::size_t functions = 0u;
    std::size_t blocks = 0u;
    std::size_t fixpoint_iterations = 0u;
    // These counters intentionally describe two different work domains.
    // `summarized_functions` is bounded by `functions`; resolution commits
    // are bounded by `resolution_functions_total` below.
    std::size_t summarized_functions = 0u;
    std::size_t pending = 0u;
    std::size_t resolutions = 0u;
    // Run-local live telemetry. These counters never participate in
    // canonical analysis output, cache keys, or product identities.
    std::size_t active_workers = 0u;
    std::size_t executor_running_workers = 0u;
    std::size_t executor_waiting_workers = 0u;
    std::size_t executor_idle_workers = 0u;
    std::size_t executor_queued_work = 0u;
    std::size_t executor_memory_blocked_work = 0u;
    std::size_t executor_continuations = 0u;
    std::size_t analysis_memory_capacity_bytes = 0u;
    std::size_t analysis_memory_used_bytes = 0u;
    std::size_t analysis_memory_peak_bytes = 0u;
    std::size_t logical_evaluations = 0u;
    std::size_t physical_evaluations = 0u;
    // Exact run-local activity domains. Counts are logical admissions into
    // that domain; active values are instantaneous. Durations are cumulative
    // and maximum wall-clock nanoseconds across possibly parallel work.
    std::size_t active_evaluation_requests = 0u;
    std::uint64_t evaluation_request_nanoseconds = 0u;
    std::uint64_t maximum_evaluation_request_nanoseconds = 0u;
    std::size_t cache_key_builds = 0u;
    std::size_t active_cache_key_builds = 0u;
    std::uint64_t cache_key_build_nanoseconds = 0u;
    std::uint64_t maximum_cache_key_build_nanoseconds = 0u;
    std::size_t cache_waits = 0u;
    std::size_t active_cache_waits = 0u;
    std::uint64_t cache_wait_nanoseconds = 0u;
    std::uint64_t maximum_cache_wait_nanoseconds = 0u;
    std::size_t cache_replays = 0u;
    std::size_t active_cache_replays = 0u;
    std::uint64_t cache_replay_nanoseconds = 0u;
    std::uint64_t maximum_cache_replay_nanoseconds = 0u;
    std::size_t active_physical_evaluations = 0u;
    std::uint64_t physical_evaluation_nanoseconds = 0u;
    std::uint64_t maximum_physical_evaluation_nanoseconds = 0u;
    std::size_t cache_commits = 0u;
    std::size_t active_cache_commits = 0u;
    std::uint64_t cache_commit_nanoseconds = 0u;
    std::uint64_t maximum_cache_commit_nanoseconds = 0u;
    // Extra interpreter executions forced by an in-flight cache hit whose
    // bounded inventory artifact could not retain an exact replay stream.
    // This is independent of the primary cache hit/miss partition.
    std::size_t cache_replay_fallback_recomputes = 0u;
    // Reserved explicit-cache-bypass compatibility counter. Stack diagnostics
    // retain the normal session-cache path and leave this at zero; physical
    // work remains misses plus replay fallbacks.
    std::size_t cache_diagnostic_bypass_evaluations = 0u;
    // Run-local physical-work fanout. In-flight subscribers always share one
    // producer. Completed aliases are byte-bounded and may re-enter the
    // session cache after LRU eviction without changing analysis semantics.
    std::size_t multi_root_context_requests = 0u;
    std::size_t multi_root_unique_contexts = 0u;
    std::size_t multi_root_ready_reuses = 0u;
    std::size_t multi_root_in_flight_reuses = 0u;
    std::size_t multi_root_provenance_links = 0u;
    std::size_t multi_root_retained_contexts = 0u;
    std::size_t multi_root_retained_payload_bytes = 0u;
    std::size_t multi_root_evictions = 0u;
    std::size_t resolution_functions_total = 0u;
    std::size_t resolution_functions_started = 0u;
    std::size_t resolution_functions_ready = 0u;
    std::size_t resolution_functions_committed = 0u;
    std::size_t resolution_head_of_line_index = 0u;
    std::size_t resolution_head_of_line_elapsed_milliseconds = 0u;
    std::size_t configured_workers = 1u;
    std::size_t session_cache_lookups = 0u;
    std::size_t session_cache_ready_hits = 0u;
    std::size_t session_cache_in_flight_coalesces = 0u;
    std::size_t session_cache_hits = 0u;
    std::size_t session_cache_misses = 0u;
    std::size_t session_cache_evictions = 0u;
    std::size_t session_cache_entries = 0u;
    // Deterministic cache admission payload, not allocator bytes or process
    // RSS. OS/process resource telemetry remains authoritative for RAM.
    std::size_t session_cache_retained_payload_bytes = 0u;
    std::size_t session_cache_miss_cold = 0u;
    std::size_t session_cache_miss_evicted = 0u;
    std::size_t session_cache_miss_oversize_or_no_exact_replay = 0u;
    std::size_t session_cache_miss_function_shape_changed = 0u;
    std::size_t session_cache_miss_projected_ingress_changed = 0u;
    std::size_t session_cache_miss_summary_dependency_changed = 0u;
    std::size_t session_cache_miss_abi_contract_changed = 0u;
    std::size_t session_cache_miss_resolution_lens_changed = 0u;
    std::size_t session_cache_miss_inventory_sink_changed = 0u;
    std::size_t session_cache_miss_isolation_partition_changed = 0u;
    std::size_t session_cache_miss_contextual_summary_changed = 0u;
    std::size_t session_cache_miss_tail_ingress_changed = 0u;
    EvaluationLensTelemetry evaluation_lenses;
    // Run-local reuse telemetry for the persistent program graph and the
    // incrementally published analysis epochs. These counters are
    // observational only and never participate in semantic identities.
    std::size_t program_graph_builds = 0u;
    std::size_t program_graph_reuses = 0u;
    std::size_t program_graph_functions_built = 0u;
    std::size_t program_graph_functions_reused = 0u;
    std::size_t caller_scc_invalidations = 0u;
    std::size_t abi_contract_epoch_reuses = 0u;
    std::size_t summary_state_reuses = 0u;
    std::size_t analysis_epochs_published = 0u;
    std::size_t analysis_epochs_discarded = 0u;
    std::size_t incremental_epochs_started = 0u;
    // Persistent terminal-root reuse. The total equation is exact for every
    // snapshot: total = reused + recomputed.
    std::size_t resolution_root_artifacts_total = 0u;
    std::size_t resolution_root_artifacts_reused = 0u;
    std::size_t resolution_root_artifacts_recomputed = 0u;
    std::size_t resolution_root_artifacts_retained = 0u;
    std::size_t resolution_epoch_retained_bytes = 0u;
    ResolutionRetentionLimitReason resolution_retention_limit_reason =
        ResolutionRetentionLimitReason::None;
    std::size_t dirty_sccs = 0u;
    std::size_t dirty_functions = 0u;
    std::size_t dirty_inventory_sinks = 0u;
    std::size_t full_cpu_recompute_fallbacks = 0u;
    PersistentAnalysisBypassReason persistent_analysis_bypass_reason =
        PersistentAnalysisBypassReason::None;
    std::size_t program_delta_entries_visited = 0u;
    std::size_t function_edge_full_scans = 0u;
    std::size_t function_edge_full_sorts = 0u;
    std::size_t candidate_call_edge_full_scans = 0u;
    std::size_t candidate_call_edge_full_sorts = 0u;
    std::size_t candidate_tail_edge_full_scans = 0u;
    std::size_t candidate_tail_edge_full_sorts = 0u;
    std::size_t program_graph_blocks_built = 0u;
    std::size_t program_graph_blocks_reused = 0u;
    std::size_t program_graph_sccs_built = 0u;
    std::size_t program_graph_sccs_reused = 0u;
    std::size_t resolution_dependency_nodes_built = 0u;
    std::size_t resolution_dependency_nodes_reused = 0u;
    std::size_t resolution_dependency_sccs_built = 0u;
    std::size_t resolution_dependency_sccs_reused = 0u;
    std::size_t abi_contract_entries_visited = 0u;
    std::size_t abi_contract_entries_rebuilt = 0u;
    std::size_t summary_candidate_entries_visited = 0u;
    std::size_t summary_candidate_entries_rebuilt = 0u;
    std::size_t inventory_topology_entries_visited = 0u;
    std::size_t resolution_preparation_entries_visited = 0u;
    std::size_t final_materialized_blocks = 0u;
    std::size_t final_materialized_functions = 0u;
    std::optional<ContextualReturnD1Telemetry> contextual_return;
};

using FunctionValueAnalysisProgressCallback =
    std::function<void(const FunctionValueAnalysisProgress& progress)>;

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges = {});

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const FunctionBoundary> function_boundaries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges = {});

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const std::uint32_t> function_entries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback);

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        std::span<const katana::sh4::DisassemblyLine> lines,
                        std::span<const FunctionBoundary> function_boundaries,
                        std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback);

namespace detail {

// Process-local instrumentation used by regressions to prove that callers
// which disable progress do not accidentally activate the callback bridge or
// its heartbeat thread. It is observational and never enters analysis output.
struct FunctionValueProgressRuntimeStatistics {
    std::size_t callback_activations = 0u;
    std::size_t pulse_threads_started = 0u;
    std::size_t detailed_cache_sessions_started = 0u;
};

[[nodiscard]] FunctionValueProgressRuntimeStatistics
function_value_progress_runtime_statistics_for_testing() noexcept;

// Narrow test observer for the already-computed ABI fixed points. The normal
// product analysis neither retains nor copies these per-function contracts.
[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values_with_abi_contract_observer_for_testing(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionBoundary> function_boundaries,
    std::span<const ResolvedControlFlowEdge> resolved_edges,
    const AbiContractObserver& observer);

} // namespace detail

} // namespace katana::analysis
