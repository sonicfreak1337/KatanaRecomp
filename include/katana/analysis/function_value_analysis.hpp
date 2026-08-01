#pragma once

#include "katana/analysis/abi.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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

inline constexpr std::uint32_t evaluation_lens_schema_version = 1u;
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
    std::vector<std::uint32_t> values;

    bool operator==(const FunctionMemoryValueSummary&) const = default;
};

struct FunctionValueSummary {
    std::uint32_t function_address = 0u;
    std::vector<FunctionRegisterValueSummary> registers;
    bool memory_complete = false;
    std::vector<FunctionMemoryValueSummary> memory_values;
    // Bounded top for payload-free aliases whose exact storage identity was
    // widened away inside this function (stack=1, memory=2).
    std::uint8_t inventory_unresolved_saved_stack_alias_sources = 0u;
    bool inventory_unresolved_saved_stack_alias_tracks_current_epoch = false;
    bool inventory_unresolved_stack_callback_loss = false;
    bool inventory_stack_callback_loss_identity_truncated = false;

    bool operator==(const FunctionValueSummary&) const = default;
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

// A finite code address stored through a non-stack 32-bit memory operation
// where either the value or destination retains known guest-call argument
// provenance.  The destination may remain symbolic (for example VBR-relative);
// this is only native-inventory evidence and never a concrete dispatch edge.
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
    bool inventory_candidate_values_truncated = false;
    bool abi_stack_base_unresolved = false;

    [[nodiscard]] constexpr bool truncated() const noexcept {
        return pending_inventory_region_count != 0u ||
               inventory_region_block_limited_regions != 0u ||
               forwarded_store_context_limited_functions != 0u ||
               contextual_return_context_limited_functions != 0u ||
               contextual_return_evaluation_limited_functions != 0u ||
               abi_stack_argument_projection_truncated_functions != 0u ||
               local_fixpoint_limited_evaluations != 0u ||
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
    std::vector<FunctionValueSummary> summaries;
    std::vector<InterproceduralTargetResolution> resolutions;
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
    // Physical interpreter executions intentionally performed outside the
    // session cache while the opt-in stack diagnostic is active. Keeping
    // this separate preserves the exact physical-work identity:
    // misses + replay fallbacks + diagnostic bypasses.
    std::size_t cache_diagnostic_bypass_evaluations = 0u;
    // Run-local physical-work fanout. A unique context invokes the persistent
    // session cache once; ready/in-flight subscribers replay that pinned
    // artifact without another interpreter execution.
    std::size_t multi_root_context_requests = 0u;
    std::size_t multi_root_unique_contexts = 0u;
    std::size_t multi_root_ready_reuses = 0u;
    std::size_t multi_root_in_flight_reuses = 0u;
    std::size_t multi_root_provenance_links = 0u;
    std::size_t multi_root_retained_contexts = 0u;
    std::size_t multi_root_retained_payload_bytes = 0u;
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
