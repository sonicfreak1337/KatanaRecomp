#pragma once

#include "katana/analysis/analysis_index.hpp"
#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/runtime_code_copy_analysis.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

class AnalysisMemoryBudget;

enum class ControlFlowAnalysisTerminationReason : std::uint8_t {
    None,
    AnalysisIterationBudgetExceeded,
    InstructionBudgetExceeded,
    AnalysisContextBudgetExceeded,
};

[[nodiscard]] constexpr std::string_view
control_flow_analysis_termination_reason_name(
    const ControlFlowAnalysisTerminationReason reason) noexcept {
    switch (reason) {
    case ControlFlowAnalysisTerminationReason::None:
        return "none";
    case ControlFlowAnalysisTerminationReason::
        AnalysisIterationBudgetExceeded:
        return "analysis-iteration-budget-exceeded";
    case ControlFlowAnalysisTerminationReason::
        InstructionBudgetExceeded:
        return "instruction-budget-exceeded";
    case ControlFlowAnalysisTerminationReason::
        AnalysisContextBudgetExceeded:
        return "analysis-context-budget-exceeded";
    }
    return "unknown";
}

enum class AnalysisDirectiveDiagnosticStatus : std::uint8_t {
    Accepted,
    Confirmed,
    Rejected,
    Stale
};

struct AnalysisDirectiveDiagnostic {
    std::size_t line = 0u;
    std::uint32_t address = 0u;
    AnalysisDirectiveDiagnosticStatus status = AnalysisDirectiveDiagnosticStatus::Accepted;
    std::string reason;
};

struct ControlFlowSite {
    std::uint32_t instruction_address = 0u;
    IndirectControlFlowKind kind = IndirectControlFlowKind::Jump;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    IndirectControlFlowOriginClass origin_class = IndirectControlFlowOriginClass::NotApplicable;
    std::vector<AnalysisEvidenceOrigin> evidence_origins;
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> evidence_call_sites;
    std::vector<std::uint32_t> evidence_callees;
};

enum class GuardedAotEntryOrigin : std::uint8_t {
    IndirectCall,
    TailIngress,
    JumpTableTail,
    StaticReturn,
    StoredCodeAddress,
    ReturnedCodeAddressTable
};

struct GuardedAotEntry {
    std::uint32_t guest_address = 0u;
    // Diagnostic ownership hint only. The guest entry remains independently
    // dispatchable so its branch and delay-slot side effects are never skipped.
    std::uint32_t shared_body_address = 0u;
    ControlFlowEvidence evidence = ControlFlowEvidence::GuardedPartial;
    std::vector<GuardedAotEntryOrigin> origins;
    // Instruction/call sites that produced this guarded inventory entry.
    std::vector<std::uint32_t> source_sites;
    // Owning functions, backing tables or other non-instruction provenance.
    std::vector<std::uint32_t> source_objects;
    std::string source_identity;
    std::uint64_t source_byte_offset = 0u;
    std::uint32_t entry_byte_extent = 0u;
    std::string entry_byte_identity;
};

enum class GuardedAotEntryRejectionReason : std::uint8_t {
    OddAddress,
    OutsideSegments,
    NotCodeSegment,
    NotExecutableSegment,
    OutsideCommittedData,
    InstructionNotAnalyzed,
    DelaySlotEntry,
    UnknownInstruction,
    EntryExtentUnavailable,
    SourceByteOffsetUnavailable,
    SegmentByteOffsetUnavailable,
    EntryBytesUnavailable
};

struct GuardedAotEntryRejection {
    std::uint32_t guest_address = 0u;
    std::uint32_t resolved_address = 0u;
    GuardedAotEntryRejectionReason reason =
        GuardedAotEntryRejectionReason::InstructionNotAnalyzed;
    ControlFlowEvidence evidence = ControlFlowEvidence::GuardedPartial;
    std::vector<GuardedAotEntryOrigin> origins;
    std::vector<std::uint32_t> source_sites;
    std::vector<std::uint32_t> source_objects;
};

// Canonical positive callback contracts discovered while the primary-image
// control-flow fixpoint is current. They retain only the externally reusable
// ABI/record shape; analyzer-local receiver provenance remains private and no
// indirect target set is made complete by publishing these contracts.
struct StaticCallbackSinkContract final {
    std::uint32_t function_address = 0u;
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t argument_mask = 0u;

    bool operator==(const StaticCallbackSinkContract&) const = default;
};

// Canonical primary-image ABI contract for a function which persists an
// incoming 32-bit argument outside its stack frame.  This does not claim that
// the word is executable.  A latent-image consumer must independently prove
// that the argument is an identity-bound record-table address before pairing
// it with a callback-field sink.
struct StaticPersistentPointerSinkContract final {
    std::uint32_t function_address = 0u;
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t argument_mask = 0u;

    bool operator==(
        const StaticPersistentPointerSinkContract&) const = default;
};

struct StaticCallbackFieldSinkContract final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t load_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    bool call = false;

    bool operator==(const StaticCallbackFieldSinkContract&) const = default;
};

// A primary-image consumer may obtain one callback from a record array whose
// base is stored in a header.  This is deliberately a shape contract rather
// than a target list: a loaded module must independently prove an
// identity-bound header pointer and a bounded, terminating record table
// before any local callback entry becomes guarded AOT inventory.
struct StaticCallbackRecordTableContract final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t callback_load_instruction_address = 0u;
    std::uint32_t callback_sink_address = 0u;
    std::int32_t header_table_pointer_displacement = 0;
    std::uint32_t record_stride = 0u;
    std::int32_t callback_displacement = 0;
    // Zero-based ABI argument index: r4..r7.
    std::uint8_t callback_argument = 0u;
    std::uint8_t width = 0u;

    bool operator==(
        const StaticCallbackRecordTableContract&) const = default;
};

struct ControlFlowAnalysisResult {
    RecursiveAnalysisResult recursive;
    RuntimeCodeCopyAnalysis runtime_code_copies;
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<StaticReturnContinuationCandidate> static_return_continuations;
    std::vector<JumpTableAnalysis> jump_tables;
    std::vector<FunctionValueSummary> function_value_summaries;
    std::vector<ResolvedControlFlowEdge> resolved_edges;
    std::vector<ControlFlowSite> sites;
    // Every accepted entry remains runtime-guarded, but code generation must
    // still provide a native entry block or a byte-bound native template.
    std::vector<GuardedAotEntry> guarded_aot_entries;
    // An accepted inventory candidate may only fail entry materialization
    // through this typed, provenance-preserving contract. Product export is
    // fail-closed while diagnostic-partial export may report the rejection.
    std::vector<GuardedAotEntryRejection> guarded_aot_entry_rejections;
    // These are the final canonical sink views from the same recursive CFG
    // epoch as this result. Consumers must fall back to their own bounded
    // extraction when materialized is false (for example an ABI mode whose
    // primary fixpoint did not run the callback companion analysis).
    std::vector<StaticCallbackSinkContract> static_callback_sinks;
    std::vector<StaticPersistentPointerSinkContract>
        static_persistent_pointer_sinks;
    std::vector<StaticCallbackFieldSinkContract>
        static_callback_field_sinks;
    std::vector<StaticCallbackRecordTableContract>
        static_callback_record_tables;
    bool static_callback_contracts_materialized = false;
    std::shared_ptr<const InstructionArena> instruction_arena;
    std::vector<InstructionSpan> block_spans;
    EvidenceInterner evidence_ids;
    JumpTableCacheCounters jump_table_cache;
    std::size_t fixpoint_iterations = 0u;
    std::size_t function_summary_iterations = 0u;
    std::size_t function_scc_count = 0u;
    std::size_t unchanged_ingress_skips = 0u;
    std::size_t function_iteration_budget = 0u;
    bool function_budget_exhausted = false;
    std::size_t raw_stored_code_inventory_candidates = 0u;
    std::size_t raw_stored_code_inventory_budget = 0u;
    bool raw_stored_code_inventory_truncated = false;
    std::size_t guarded_code_inventory_candidates = 0u;
    std::size_t guarded_code_inventory_budget = 0u;
    bool guarded_code_inventory_candidate_budget_exhausted = false;
    GuardedCodeInventoryWalkDiagnostics guarded_code_inventory_walk;
    std::size_t guarded_code_shape_validation_work = 0u;
    std::size_t guarded_code_shape_validation_work_budget = 0u;
    std::size_t guarded_code_shape_budget_exceeded_candidates = 0u;
    bool candidate_inventory_truncated = false;
    bool returned_table_scan_truncated = false;
    bool progress_callback_failed = false;
    ControlFlowAnalysisTerminationReason termination_reason =
        ControlFlowAnalysisTerminationReason::None;
    std::vector<AnalysisDirectiveDiagnostic> directive_diagnostics;
    std::vector<SymbolicAddress> symbolic_addresses;
    // Canonical monotone seed ledger. Causes preserve why a target entered
    // the decode/function contract without turning guarded candidates into
    // fixed CFG edges.
    enum class SeedCauseKind : std::uint8_t {
        EntryPoint,
        Symbol,
        FunctionDirective,
        RuntimeCodeCopySource,
        RuntimeCodePatch,
        StaticReturnContinuation,
        IndirectControlFlowTarget,
        IndirectAnalysisCandidate,
        JumpTableEntry,
        StoredCodeAddress,
        ReturnedCodeAddressTable,
    };
    struct SeedCause {
        SeedCauseKind kind = SeedCauseKind::EntryPoint;
        // Guest address zero is valid.  Absence must therefore remain typed
        // instead of sharing a sentinel with a real address/register/line.
        std::optional<std::uint32_t> source_address;
        std::optional<std::uint32_t> source_object;
        std::optional<std::uint32_t> owner_address;
        // Evidence dimensions are independent canonical sets. Keeping them
        // on the cause avoids compressing provenance to the first element or
        // inventing a call-site/callee Cartesian correlation.
        std::vector<std::uint32_t> evidence_call_sites;
        std::vector<std::uint32_t> evidence_callees;

        bool operator==(const SeedCause&) const = default;
    };
    struct SeedFact {
        std::uint32_t target_address = 0u;
        std::vector<FunctionOrigin> origins;
        bool proven = false;
        ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
        std::uint32_t function_size = 0u;
        std::vector<SeedCause> causes;
    };
    std::vector<SeedFact> seed_facts;
    std::size_t seed_targets_added = 0u;
    std::size_t seed_targets_strengthened = 0u;
    std::size_t seed_causes_added = 0u;
    std::size_t seed_decode_targets = 0u;
    std::size_t seed_metadata_targets = 0u;
    std::size_t recursive_incremental_passes = 0u;
    std::size_t recursive_full_recompute_fallbacks = 0u;
    PersistentAnalysisBypassReason persistent_analysis_bypass_reason =
        PersistentAnalysisBypassReason::None;
    std::size_t recursive_snapshot_epochs = 0u;
    std::size_t recursive_final_materializations = 0u;
    RecursiveAnalysisPhysicalWork recursive_physical_work;
    std::size_t runtime_copy_instruction_visits = 0u;
    std::size_t runtime_copy_result_entries_visited = 0u;
    std::size_t runtime_copy_result_entries_rebuilt = 0u;
    std::size_t local_control_flow_instruction_visits = 0u;
    std::size_t local_control_flow_result_entries_visited = 0u;
    std::size_t local_control_flow_result_entries_rebuilt = 0u;
    std::size_t dispatch_index_entries_visited = 0u;
    std::size_t dispatch_index_entries_rebuilt = 0u;
    // Candidate-contract normalization is delta-driven between the one cold
    // baseline scan and the one terminal verification scan.  These ledgers
    // make an accidental warm whole-program scan observable.
    std::size_t runtime_contract_normalization_entries_visited = 0u;
    std::size_t runtime_contract_normalization_full_scans = 0u;
    std::size_t decode_boundary_normalization_entries_visited = 0u;
    std::size_t decode_boundary_normalization_full_scans = 0u;
    std::size_t jump_table_instruction_visits = 0u;
    std::size_t jump_table_result_entries_visited = 0u;
    std::size_t jump_table_result_entries_rebuilt = 0u;
    std::size_t function_boundary_entries_visited = 0u;
    std::size_t function_boundary_entries_rebuilt = 0u;
    std::size_t function_edge_family_entries_visited = 0u;
    std::size_t function_edge_family_entries_rebuilt = 0u;
    std::size_t function_edge_state_encode_items = 0u;
    std::size_t function_edge_state_copy_items = 0u;
    std::size_t function_edge_state_exact_compare_items = 0u;
    std::size_t function_value_inventory_topology_entries_visited = 0u;
    std::size_t
        function_value_resolution_preparation_entries_visited = 0u;
    std::size_t result_index_copy_items = 0u;
    std::size_t result_index_sort_items = 0u;
    std::size_t result_index_materialized_items = 0u;
};

[[nodiscard]] constexpr bool
guarded_aot_inventory_complete(
    const ControlFlowAnalysisResult& analysis) noexcept {
    return !analysis.function_budget_exhausted &&
           analysis.termination_reason ==
               ControlFlowAnalysisTerminationReason::None &&
           !analysis.raw_stored_code_inventory_truncated &&
           !analysis.guarded_code_inventory_candidate_budget_exhausted &&
           !analysis.guarded_code_inventory_walk.truncated() &&
           !analysis.candidate_inventory_truncated &&
           !analysis.returned_table_scan_truncated &&
           analysis.guarded_code_shape_budget_exceeded_candidates == 0u &&
           analysis.guarded_aot_entry_rejections.empty();
}

struct ControlFlowAnalysisProgress {
    // Owned snapshots are safe for asynchronous UI/logger queues.
    std::string phase;
    std::size_t iteration = 0u;
    std::size_t seeds = 0u;
    std::size_t instructions = 0u;
    std::size_t contexts = 0u;
    std::size_t resolutions = 0u;
    std::size_t candidate_contract_iteration = 0u;
    std::size_t candidate_contract_iteration_budget = 0u;
    std::size_t round_seed_baseline = 0u;
    std::size_t round_added_seeds = 0u;
    std::size_t round_seed_facts_added = 0u;
    std::size_t round_seed_targets_changed = 0u;
    std::size_t round_decode_targets = 0u;
    std::size_t round_metadata_targets = 0u;
    std::size_t round_full_cpu_fallbacks = 0u;
    PersistentAnalysisBypassReason persistent_analysis_bypass_reason =
        PersistentAnalysisBypassReason::None;
    std::size_t recursive_snapshot_epochs = 0u;
    std::size_t recursive_final_materializations = 0u;
    RecursiveAnalysisPhysicalWork recursive_physical_work;
    std::size_t runtime_copy_instruction_visits = 0u;
    std::size_t runtime_copy_result_entries_visited = 0u;
    std::size_t runtime_copy_result_entries_rebuilt = 0u;
    std::size_t local_control_flow_instruction_visits = 0u;
    std::size_t local_control_flow_result_entries_visited = 0u;
    std::size_t local_control_flow_result_entries_rebuilt = 0u;
    std::size_t dispatch_index_entries_visited = 0u;
    std::size_t dispatch_index_entries_rebuilt = 0u;
    std::size_t jump_table_instruction_visits = 0u;
    std::size_t jump_table_result_entries_visited = 0u;
    std::size_t jump_table_result_entries_rebuilt = 0u;
    std::size_t function_boundary_entries_visited = 0u;
    std::size_t function_boundary_entries_rebuilt = 0u;
    std::size_t function_edge_family_entries_visited = 0u;
    std::size_t function_edge_family_entries_rebuilt = 0u;
    std::size_t function_edge_state_encode_items = 0u;
    std::size_t function_edge_state_copy_items = 0u;
    std::size_t function_edge_state_exact_compare_items = 0u;
    std::size_t result_index_copy_items = 0u;
    std::size_t result_index_sort_items = 0u;
    std::size_t result_index_materialized_items = 0u;
    bool growing_workset = false;
    bool function_value_active = false;
    std::string function_value_subphase;
    std::size_t function_value_subphase_planned = 0u;
    std::size_t function_value_subphase_processed = 0u;
    std::size_t function_value_subphase_queued = 0u;
    std::size_t function_value_subphase_iterations = 0u;
    std::size_t function_value_functions = 0u;
    std::size_t function_value_blocks = 0u;
    std::size_t function_value_iterations = 0u;
    std::size_t function_value_summarized_functions = 0u;
    std::size_t function_value_pending = 0u;
    std::size_t function_value_active_workers = 0u;
    std::size_t function_value_executor_running_workers = 0u;
    std::size_t function_value_executor_waiting_workers = 0u;
    std::size_t function_value_executor_idle_workers = 0u;
    std::size_t function_value_executor_queued_work = 0u;
    std::size_t function_value_executor_memory_blocked_work = 0u;
    std::size_t function_value_executor_continuations = 0u;
    std::size_t function_value_analysis_memory_capacity_bytes = 0u;
    std::size_t function_value_analysis_memory_used_bytes = 0u;
    std::size_t function_value_analysis_memory_peak_bytes = 0u;
    std::size_t function_value_logical_evaluations = 0u;
    std::size_t function_value_physical_evaluations = 0u;
    std::size_t function_value_active_evaluation_requests = 0u;
    std::uint64_t function_value_evaluation_request_nanoseconds = 0u;
    std::uint64_t
        function_value_maximum_evaluation_request_nanoseconds = 0u;
    std::size_t function_value_cache_key_builds = 0u;
    std::size_t function_value_active_cache_key_builds = 0u;
    std::uint64_t function_value_cache_key_build_nanoseconds = 0u;
    std::uint64_t
        function_value_maximum_cache_key_build_nanoseconds = 0u;
    std::size_t function_value_cache_waits = 0u;
    std::size_t function_value_active_cache_waits = 0u;
    std::uint64_t function_value_cache_wait_nanoseconds = 0u;
    std::uint64_t function_value_maximum_cache_wait_nanoseconds = 0u;
    std::size_t function_value_cache_replays = 0u;
    std::size_t function_value_active_cache_replays = 0u;
    std::uint64_t function_value_cache_replay_nanoseconds = 0u;
    std::uint64_t function_value_maximum_cache_replay_nanoseconds = 0u;
    std::size_t function_value_active_physical_evaluations = 0u;
    std::uint64_t function_value_physical_evaluation_nanoseconds = 0u;
    std::uint64_t
        function_value_maximum_physical_evaluation_nanoseconds = 0u;
    std::size_t function_value_cache_commits = 0u;
    std::size_t function_value_active_cache_commits = 0u;
    std::uint64_t function_value_cache_commit_nanoseconds = 0u;
    std::uint64_t function_value_maximum_cache_commit_nanoseconds = 0u;
    std::size_t
        function_value_session_cache_replay_fallback_recomputes = 0u;
    std::size_t
        function_value_session_cache_diagnostic_bypass_evaluations = 0u;
    std::size_t function_value_multi_root_context_requests = 0u;
    std::size_t function_value_multi_root_unique_contexts = 0u;
    std::size_t function_value_multi_root_ready_reuses = 0u;
    std::size_t function_value_multi_root_in_flight_reuses = 0u;
    std::size_t function_value_multi_root_provenance_links = 0u;
    std::size_t function_value_multi_root_retained_contexts = 0u;
    std::size_t
        function_value_multi_root_retained_payload_bytes = 0u;
    std::size_t function_value_multi_root_evictions = 0u;
    std::size_t function_value_resolution_functions_total = 0u;
    std::size_t function_value_resolution_functions_started = 0u;
    std::size_t function_value_resolution_functions_ready = 0u;
    std::size_t function_value_resolution_functions_committed = 0u;
    std::size_t function_value_resolution_head_of_line_index = 0u;
    std::size_t
        function_value_resolution_head_of_line_elapsed_milliseconds = 0u;
    std::size_t function_value_configured_workers = 1u;
    std::size_t function_value_session_cache_lookups = 0u;
    std::size_t function_value_session_cache_ready_hits = 0u;
    std::size_t
        function_value_session_cache_in_flight_coalesces = 0u;
    std::size_t function_value_session_cache_hits = 0u;
    std::size_t function_value_session_cache_misses = 0u;
    std::size_t function_value_session_cache_evictions = 0u;
    std::size_t function_value_session_cache_entries = 0u;
    std::size_t
        function_value_session_cache_retained_payload_bytes = 0u;
    std::size_t function_value_session_cache_miss_cold = 0u;
    std::size_t function_value_session_cache_miss_evicted = 0u;
    std::size_t
        function_value_session_cache_miss_oversize_or_no_exact_replay =
            0u;
    std::size_t
        function_value_session_cache_miss_function_shape_changed = 0u;
    std::size_t
        function_value_session_cache_miss_projected_ingress_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_summary_dependency_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_abi_contract_changed = 0u;
    std::size_t
        function_value_session_cache_miss_resolution_lens_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_inventory_sink_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_isolation_partition_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_contextual_summary_changed =
            0u;
    std::size_t
        function_value_session_cache_miss_tail_ingress_changed = 0u;
    EvaluationLensTelemetry function_value_evaluation_lenses;
    std::size_t function_value_program_graph_builds = 0u;
    std::size_t function_value_program_graph_reuses = 0u;
    std::size_t function_value_program_graph_functions_built = 0u;
    std::size_t function_value_program_graph_functions_reused = 0u;
    std::size_t function_value_caller_scc_invalidations = 0u;
    std::size_t function_value_abi_contract_epoch_reuses = 0u;
    std::size_t function_value_summary_state_reuses = 0u;
    std::size_t function_value_analysis_epochs_published = 0u;
    std::size_t function_value_analysis_epochs_discarded = 0u;
    std::size_t function_value_incremental_epochs_started = 0u;
    std::size_t function_value_resolution_root_artifacts_total = 0u;
    std::size_t function_value_resolution_root_artifacts_reused = 0u;
    std::size_t function_value_resolution_root_artifacts_recomputed = 0u;
    std::size_t function_value_resolution_root_artifacts_retained = 0u;
    std::size_t function_value_resolution_epoch_retained_bytes = 0u;
    ResolutionRetentionLimitReason
        function_value_resolution_retention_limit_reason =
            ResolutionRetentionLimitReason::None;
    std::size_t function_value_dirty_sccs = 0u;
    std::size_t function_value_dirty_functions = 0u;
    std::size_t function_value_dirty_inventory_sinks = 0u;
    std::size_t function_value_full_cpu_recompute_fallbacks = 0u;
    PersistentAnalysisBypassReason
        function_value_persistent_analysis_bypass_reason =
            PersistentAnalysisBypassReason::None;
    std::size_t function_value_program_delta_entries_visited = 0u;
    std::size_t function_value_function_edge_full_scans = 0u;
    std::size_t function_value_function_edge_full_sorts = 0u;
    std::size_t function_value_candidate_call_edge_full_scans = 0u;
    std::size_t function_value_candidate_call_edge_full_sorts = 0u;
    std::size_t function_value_candidate_tail_edge_full_scans = 0u;
    std::size_t function_value_candidate_tail_edge_full_sorts = 0u;
    std::size_t function_value_graph_blocks_built = 0u;
    std::size_t function_value_graph_blocks_reused = 0u;
    std::size_t function_value_graph_sccs_built = 0u;
    std::size_t function_value_graph_sccs_reused = 0u;
    std::size_t function_value_resolution_dependency_nodes_built = 0u;
    std::size_t function_value_resolution_dependency_nodes_reused = 0u;
    std::size_t function_value_resolution_dependency_sccs_built = 0u;
    std::size_t function_value_resolution_dependency_sccs_reused = 0u;
    std::size_t function_value_abi_contract_entries_visited = 0u;
    std::size_t function_value_abi_contract_entries_rebuilt = 0u;
    std::size_t function_value_summary_candidate_entries_visited = 0u;
    std::size_t function_value_summary_candidate_entries_rebuilt = 0u;
    std::size_t function_value_inventory_topology_entries_visited = 0u;
    std::size_t
        function_value_resolution_preparation_entries_visited = 0u;
    std::size_t function_value_final_materialized_blocks = 0u;
    std::size_t function_value_final_materialized_functions = 0u;
    std::optional<ContextualReturnD1Telemetry> function_value_contextual_return;
};

using ControlFlowAnalysisProgressCallback =
    std::function<void(const ControlFlowAnalysisProgress& progress)>;

inline constexpr std::size_t
    maximum_persistent_function_analysis_epoch_blob_bytes =
        768u * 1024u * 1024u;

using PersistentFunctionAnalysisEpochPublishCallback =
    std::function<void(std::span<const std::uint8_t> blob)>;

struct ControlFlowAnalysisOptions {
    // Detailed cache-miss history is diagnostic work and remains independent
    // from live progress and execution limits.
    bool detailed_cache_miss_telemetry = false;
    // Limits are enforced by the analyzer itself. Progress observers remain
    // observational and their exceptions can never request cancellation.
    std::size_t maximum_fixpoint_iterations =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximum_instructions =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximum_contexts =
        std::numeric_limits<std::size_t>::max();
    // Optional non-owning child budget for FVA resolution results.  When the
    // child is parent-accounted, only retained result bytes are charged to
    // the enclosing executor; a temporarily full parent evicts/recomputes
    // pure finalized roots instead of changing the logical result budget.
    AnalysisMemoryBudget* pre_reserved_function_value_ready_budget =
        nullptr;
    // Optional non-owning parent-accounted arena for the two exact-replay
    // FVA caches.  Cache pressure may only evict completed aliases; it never
    // prunes a candidate, lowers a bounded analysis limit, or changes the
    // canonical fixed point.
    AnalysisMemoryBudget* function_value_cache_memory_budget = nullptr;
    // Optional exact-image FunctionValue epoch. Import is provisional and
    // transactional: any stale, malformed, incompatible or oversized blob is
    // only a cache miss and the complete analysis remains authoritative.
    std::span<const std::uint8_t>
        persistent_function_analysis_epoch_import_blob;
    std::string_view
        persistent_function_analysis_epoch_implementation_identity;
    std::size_t maximum_persistent_function_analysis_epoch_blob_bytes =
        katana::analysis::
            maximum_persistent_function_analysis_epoch_blob_bytes;
    // Invoked only for a complete, safely exportable terminal epoch.
    // Non-resource callback failures are observational cache failures and do
    // not fail the analysis; std::bad_alloc preserves normal resource failure.
    PersistentFunctionAnalysisEpochPublishCallback
        persistent_function_analysis_epoch_publish_callback;
};

[[nodiscard]] const char*
analysis_directive_diagnostic_status_name(AnalysisDirectiveDiagnosticStatus status) noexcept;
[[nodiscard]] const char*
guarded_aot_entry_origin_name(GuardedAotEntryOrigin origin) noexcept;
[[nodiscard]] const char*
guarded_aot_entry_rejection_reason_name(
    GuardedAotEntryRejectionReason reason) noexcept;
[[nodiscard]] const char*
exact_guard_rejection_reason_name(ExactGuardRejectionReason reason) noexcept;

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides = nullptr);

[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(const katana::io::ExecutableImage& image,
                     const AnalysisOverrides* overrides,
                     const ControlFlowAnalysisProgressCallback& progress_callback);

// Detailed miss-reason history performs extra component hashing and is
// intentionally independent from basic live progress.
[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    bool detailed_cache_miss_telemetry);

// Bounded analysis is a product-work contract. A reached limit returns a
// typed partial result through termination_reason; it is never signaled by a
// progress callback.
[[nodiscard]] ControlFlowAnalysisResult
analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides,
    const ControlFlowAnalysisProgressCallback& progress_callback,
    const ControlFlowAnalysisOptions& options);

} // namespace katana::analysis
