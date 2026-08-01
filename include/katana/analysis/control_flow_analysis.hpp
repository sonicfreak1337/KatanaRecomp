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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

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
    std::vector<AnalysisDirectiveDiagnostic> directive_diagnostics;
    std::vector<SymbolicAddress> symbolic_addresses;
};

[[nodiscard]] constexpr bool
guarded_aot_inventory_complete(
    const ControlFlowAnalysisResult& analysis) noexcept {
    return !analysis.function_budget_exhausted &&
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
};

using ControlFlowAnalysisProgressCallback =
    std::function<void(const ControlFlowAnalysisProgress& progress)>;

[[nodiscard]] const char*
analysis_directive_diagnostic_status_name(AnalysisDirectiveDiagnosticStatus status) noexcept;
[[nodiscard]] const char*
guarded_aot_entry_origin_name(GuardedAotEntryOrigin origin) noexcept;
[[nodiscard]] const char*
guarded_aot_entry_rejection_reason_name(
    GuardedAotEntryRejectionReason reason) noexcept;

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

} // namespace katana::analysis
