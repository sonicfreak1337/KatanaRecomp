#pragma once

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace katana::analysis {
class AnalysisMemoryBudget;
}

namespace katana::analysis::detail {

enum class GuardedNativeEntryShapeStatus : std::uint8_t {
    Valid,
    StructurallyInvalid,
    OutsideImage,
    ShapeBudgetExceeded
};

struct GuardedNativeEntryShapeStatistics {
    std::size_t work = 0u;
    std::size_t work_budget = 0u;
    std::size_t valid = 0u;
    std::size_t structurally_invalid = 0u;
    std::size_t outside_image = 0u;
    std::size_t shape_budget_exceeded = 0u;
};

class GuardedNativeEntryShapeCache {
  public:
    explicit GuardedNativeEntryShapeCache(const katana::io::ExecutableImage& image)
        : image_(&image),
          bound_image_identity_(image.analysis_instance_identity()),
          bound_image_revision_(image.analysis_revision()),
          bound_image_immutable_generation_(image.immutable_generation()) {
        statistics_.work_budget = maximum_total_instructions;
    }

    // Analysis entrypoints may deliberately retain this cache across rounds.
    // Rebinding is fail-safe for both an in-place image mutation and a
    // different image instance; neither may inherit old shape proofs.
    void bind(const katana::io::ExecutableImage& image) {
        const auto identity = image.analysis_instance_identity();
        const auto revision = image.analysis_revision();
        const auto immutable_generation = image.immutable_generation();
        if (image_ == &image &&
            bound_image_identity_ == identity &&
            bound_image_immutable_generation_ == immutable_generation) {
            // Root-set mutations advance analysis_revision without changing
            // any bytes or shape proof. Keep the immutable shape shards and
            // refresh only the diagnostic revision binding.
            bound_image_revision_ = revision;
            return;
        }
        image_ = &image;
        bound_image_identity_ = identity;
        bound_image_revision_ = revision;
        bound_image_immutable_generation_ = immutable_generation;
        results_.clear();
        statistics_ = {};
        statistics_.work_budget = maximum_total_instructions;
    }

    void clear() noexcept {
        results_.clear();
        statistics_ = {};
        statistics_.work_budget = maximum_total_instructions;
    }

    [[nodiscard]] GuardedNativeEntryShapeStatus
    classify(const std::uint32_t address) {
        // Direct users of the cache do not pass the image again. Detect
        // in-place mutations here as well as at the analysis entrypoint.
        bind(*image_);
        const auto validation = validate_decode_candidate(*image_, address);
        if (!validation.valid()) {
            return remember(address,
                            GuardedNativeEntryShapeStatus::OutsideImage);
        }
        const auto canonical_address = validation.resolved_address;
        // A physical delay slot is owned by the immediately preceding
        // control-transfer instruction. Decoding from the slot in isolation
        // can still find a perfectly ordinary return and therefore used to
        // manufacture a plausible-looking guarded callback entry. Only an
        // independent, explicit normal-entry contract may authorize that
        // dual-context shape; callers handle that stronger evidence before
        // consulting this cache.
        if (prove_sh4_physical_delay_slot(
                *image_, canonical_address).has_value()) {
            const auto result = remember(
                canonical_address,
                GuardedNativeEntryShapeStatus::StructurallyInvalid);
            results_.insert_or_assign(address, result);
            return result;
        }
        if (const auto cached = results_.find(address); cached != results_.end())
            return cached->second;
        if (const auto cached = results_.find(canonical_address); cached != results_.end()) {
            results_.insert_or_assign(address, cached->second);
            return cached->second;
        }
        const auto result = validate(canonical_address);
        static_cast<void>(remember(canonical_address, result));
        results_.insert_or_assign(address, result);
        return result;
    }

    [[nodiscard]] bool
    is_physical_delay_slot(const std::uint32_t address) {
        bind(*image_);
        const auto validation = validate_decode_candidate(*image_, address);
        return validation.valid() &&
               prove_sh4_physical_delay_slot(
                   *image_, validation.resolved_address).has_value();
    }

    [[nodiscard]] const GuardedNativeEntryShapeStatistics&
    statistics() const noexcept {
        return statistics_;
    }

  private:
    static constexpr std::size_t maximum_instructions = 4'096u;
    // Prevent invalid data candidates from expanding validation work beyond the
    // worst-case work admitted entries could have consumed.  This cache lives
    // across the outer control-flow fixpoint, so the limit is global to the
    // complete analysis rather than resetting on each iteration.
    static constexpr std::size_t maximum_total_instructions =
        maximum_instructions * 1'024u;

    struct DecodeResult {
        GuardedNativeEntryShapeStatus status =
            GuardedNativeEntryShapeStatus::StructurallyInvalid;
        std::optional<katana::sh4::DecodedInstruction> instruction;
    };

    [[nodiscard]] GuardedNativeEntryShapeStatus
    remember(const std::uint32_t address,
             const GuardedNativeEntryShapeStatus status) {
        const auto [iterator, inserted] = results_.try_emplace(address, status);
        if (!inserted) return iterator->second;
        switch (status) {
        case GuardedNativeEntryShapeStatus::Valid:
            ++statistics_.valid;
            break;
        case GuardedNativeEntryShapeStatus::StructurallyInvalid:
            ++statistics_.structurally_invalid;
            break;
        case GuardedNativeEntryShapeStatus::OutsideImage:
            ++statistics_.outside_image;
            break;
        case GuardedNativeEntryShapeStatus::ShapeBudgetExceeded:
            ++statistics_.shape_budget_exceeded;
            break;
        }
        return status;
    }

    [[nodiscard]] DecodeResult
    decode_at(const std::uint32_t address,
               std::size_t& candidate_work) {
        const auto validation = validate_decode_candidate(*image_, address);
        if (!validation.valid() || validation.segment == nullptr)
            return {GuardedNativeEntryShapeStatus::OutsideImage, std::nullopt};
        if (candidate_work >= maximum_instructions ||
            statistics_.work >= statistics_.work_budget)
            return {GuardedNativeEntryShapeStatus::ShapeBudgetExceeded,
                    std::nullopt};
        ++candidate_work;
        ++statistics_.work;
        const auto offset = validation.segment->byte_offset(validation.resolved_address);
        if (!offset.has_value() || *offset > validation.segment->bytes.size() ||
            validation.segment->bytes.size() - *offset < 2u)
            return {GuardedNativeEntryShapeStatus::OutsideImage, std::nullopt};
        const auto instruction = katana::sh4::decode(
            katana::io::read_u16_le(validation.segment->bytes, *offset));
        if (!instruction.is_known())
            return {GuardedNativeEntryShapeStatus::StructurallyInvalid,
                    std::nullopt};
        return {GuardedNativeEntryShapeStatus::Valid, instruction};
    }

    [[nodiscard]] GuardedNativeEntryShapeStatus
    validate(const std::uint32_t entry_address) {
        std::deque<std::uint32_t> pending{entry_address};
        std::unordered_set<std::uint32_t> visited;
        visited.reserve(maximum_instructions);
        std::size_t candidate_work = 0u;
        const auto enqueue_fallthrough =
            [&pending](const std::uint32_t address, const bool has_delay_slot) {
                const auto distance = has_delay_slot ? 4u : 2u;
                if (address > std::numeric_limits<std::uint32_t>::max() - distance)
                    return false;
                pending.push_back(address + distance);
                return true;
            };

        while (!pending.empty()) {
            const auto address = pending.front();
            pending.pop_front();
            if (!visited.insert(address).second) continue;
            if (address != entry_address) {
                const auto cached = results_.find(address);
                if (cached != results_.end()) {
                    if (cached->second ==
                        GuardedNativeEntryShapeStatus::Valid)
                        continue;
                    return cached->second;
                }
            }

            const auto decoded = decode_at(address, candidate_work);
            if (!decoded.instruction.has_value()) return decoded.status;
            const auto& instruction = *decoded.instruction;
            if (instruction.has_delay_slot) {
                if (address > std::numeric_limits<std::uint32_t>::max() - 2u)
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                const auto delay = decode_at(address + 2u, candidate_work);
                if (!delay.instruction.has_value()) return delay.status;
                if (delay.instruction->changes_control_flow())
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
            }

            switch (instruction.control_flow) {
            case katana::sh4::ControlFlowKind::None:
                if (!enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                break;
            case katana::sh4::ControlFlowKind::ConditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(instruction, address);
                if (!target.has_value() ||
                    !enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
                pending.push_back(*target);
                break;
            }
            case katana::sh4::ControlFlowKind::Call:
            case katana::sh4::ControlFlowKind::IndirectCall:
                // A candidate entry owns its local continuation, not the
                // independently validated native entry of a callee.
                if (!enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                break;
            case katana::sh4::ControlFlowKind::UnconditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(instruction, address);
                if (!target.has_value())
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
                pending.push_back(*target);
                break;
            }
            case katana::sh4::ControlFlowKind::Return:
            case katana::sh4::ControlFlowKind::IndirectBranch:
            case katana::sh4::ControlFlowKind::Trap:
            case katana::sh4::ControlFlowKind::ExceptionReturn:
            case katana::sh4::ControlFlowKind::Halt:
                break;
            }
        }
        // A successful walk proves every ordinary CFG node reached from this
        // entry.  Cache those suffixes as well; delay-slot-only decodes were
        // never inserted into visited and therefore never become standalone
        // entry proofs.  Large families of tiny wrappers and shared tails no
        // longer repeat the same 4K walk for every candidate.
        for (const auto address : visited) {
            if (address != entry_address)
                results_.try_emplace(
                    address, GuardedNativeEntryShapeStatus::Valid);
        }
        return GuardedNativeEntryShapeStatus::Valid;
    }

    const katana::io::ExecutableImage* image_ = nullptr;
    std::uint64_t bound_image_identity_ = 0u;
    std::uint64_t bound_image_revision_ = 0u;
    std::uint64_t bound_image_immutable_generation_ = 0u;
    std::unordered_map<std::uint32_t, GuardedNativeEntryShapeStatus> results_;
    GuardedNativeEntryShapeStatistics statistics_;
};

// Exact, analysis-scoped memoization for the expensive per-function abstract
// interpreter. A control-flow analysis keeps one instance alive across every
// candidate-contract and outer decode/seed round; public one-shot entrypoints
// create a short-lived instance.
//
// Cache limits affect reuse only. Eviction must not change canonical output,
// diagnostics, logical FIFO order, or any analysis budget.
enum class FunctionEvaluationCacheMissReason : std::uint8_t {
    Cold,
    Evicted,
    OversizeOrNoExactReplay,
    FunctionShapeChanged,
    ProjectedIngressChanged,
    SummaryDependencyChanged,
    AbiContractChanged,
    ResolutionLensChanged,
    InventorySinkChanged,
    IsolationPartitionChanged,
    ContextualSummaryChanged,
    TailIngressChanged,
    Count,
};

inline constexpr auto function_evaluation_cache_miss_reason_count =
    static_cast<std::size_t>(
        FunctionEvaluationCacheMissReason::Count);

enum class FunctionEvaluationCacheLookupOutcome : std::uint8_t {
    ReadyHit,
    InFlightCoalesce,
    Miss,
};

enum class FunctionProgramDeltaKind : std::uint8_t {
    Unknown,
    Unchanged,
    Exact,
};

struct FunctionProgramLineDelta final {
    std::uint32_t address = 0u;
    // nullopt is a deletion. Otherwise value.address must equal address.
    std::optional<katana::sh4::DisassemblyLine> value;
};

struct FunctionProgramBoundaryDelta final {
    std::uint32_t entry_address = 0u;
    // nullopt is a deletion. Otherwise value.entry_address must match.
    std::optional<FunctionBoundary> value;
};

// Each entry replaces the complete family for one instruction site. An empty
// vector therefore means deletion and cannot be confused with "unchanged".
struct FunctionProgramEdgeSiteDelta final {
    std::uint32_t instruction_address = 0u;
    std::vector<ResolvedControlFlowEdge> values;
};

struct FunctionProgramDelta final {
    FunctionProgramDeltaKind kind = FunctionProgramDeltaKind::Unknown;
    FunctionValueResultMaterialization result_materialization =
        FunctionValueResultMaterialization::TerminalFull;
    std::uint64_t expected_published_epoch_version = 0u;
    std::uint64_t image_identity = 0u;
    std::uint64_t image_revision = 0u;
    // Root-set mutations advance image_revision while retaining the
    // authenticated byte/layout generation. A matching nonzero generation
    // enables the root-only warm path; legacy callers without it must still
    // provide the exact revision and therefore remain fail-closed across a
    // root-only mutation.
    std::uint64_t image_immutable_generation = 0u;
    std::vector<FunctionProgramLineDelta> changed_lines;
    std::vector<FunctionProgramBoundaryDelta> changed_boundaries;
    std::vector<FunctionProgramEdgeSiteDelta> changed_semantic_edge_sites;
    std::vector<FunctionProgramEdgeSiteDelta>
        changed_candidate_call_sites;
    std::vector<FunctionProgramEdgeSiteDelta>
        changed_candidate_tail_sites;
};

struct FunctionEvaluationCacheDecision final {
    std::uint32_t function_entry = 0u;
    EvaluationLens lens = EvaluationLens::FullState;
    bool full_state_fallback = false;
    std::uint64_t avoided_evaluation_nanoseconds = 0u;
    FunctionEvaluationCacheLookupOutcome outcome =
        FunctionEvaluationCacheLookupOutcome::Miss;
    std::optional<FunctionEvaluationCacheMissReason> miss_reason;
};

// Test/performance-gate observer for exact invalidation-closure evidence.
// It is empty in product sessions and therefore does not retain per-key
// histories or add synchronization to the normal hot path. A supplied
// observer may run concurrently and must treat the decision as ephemeral.
// Miss decisions are delivered after any retainability reclassification, so
// their one primary reason matches the aggregate session ledger exactly.
using FunctionEvaluationCacheDecisionObserver =
    std::function<void(const FunctionEvaluationCacheDecision&)>;

struct FunctionValueAnalysisSessionStatistics {
    std::size_t lookups = 0u;
    std::size_t ready_hits = 0u;
    std::size_t in_flight_coalesces = 0u;
    // Compatibility aggregate for existing diagnostics. It is always the
    // exact sum of ready_hits and in_flight_coalesces.
    std::size_t hits = 0u;
    std::size_t misses = 0u;
    std::size_t evictions = 0u;
    std::size_t entries = 0u;
    // Deterministic admission budget for retained entry owners, key storage
    // and evaluation payloads. This is deliberately not process RSS and does
    // not claim allocator/container/control-block overhead.
    std::size_t retained_payload_bytes = 0u;
    std::array<std::size_t,
               function_evaluation_cache_miss_reason_count>
        miss_reasons{};
    EvaluationLensTelemetry evaluation_lenses;
    // KR-4976 program/session observability. Build/reuse counters describe
    // only atomically published product-analysis epochs. Discarded counts
    // failed staging attempts (including exceptions), none of which makes a
    // graph, ABI contract, or summary appear reusable.
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
    std::size_t resolution_root_artifacts_reused = 0u;
    std::size_t resolution_root_artifacts_recomputed = 0u;
    // Latest successfully published epoch, not cumulative event counters.
    // A limit drops the whole optional retention set, so retained roots and
    // bytes are both zero while the typed run-local reason stays observable.
    std::size_t resolution_root_artifacts_retained = 0u;
    std::size_t resolution_epoch_retained_bytes = 0u;
    ResolutionRetentionLimitReason resolution_retention_limit_reason =
        ResolutionRetentionLimitReason::None;
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

    [[nodiscard]] std::size_t classified_misses() const noexcept {
        std::size_t total = 0u;
        for (const auto count : miss_reasons) total += count;
        return total;
    }

    [[nodiscard]] bool balanced() const noexcept {
        return lookups == ready_hits + in_flight_coalesces + misses &&
               hits == ready_hits + in_flight_coalesces &&
               misses == classified_misses();
    }
};

struct FunctionEvaluationCacheTelemetryProbe final {
    FunctionValueAnalysisSessionStatistics statistics;
    FunctionValueAnalysisSessionStatistics observer_statistics;
    std::size_t physical_computations = 0u;
    std::vector<FunctionEvaluationCacheDecision> decisions;
    // A heap-empty artifact must account its inline owner once and only once.
    // The exact/short limits exercise the same measured retained-byte value as
    // the production admission path instead of a parallel test estimate.
    std::size_t inline_only_artifact_bytes = 0u;
    std::size_t inline_only_artifact_owner_bytes = 0u;
    std::size_t controlled_artifact_bytes = 0u;
    std::size_t controlled_entry_retained_payload_bytes = 0u;
    std::size_t exact_limit_entries = 0u;
    std::size_t exact_limit_retained_payload_bytes = 0u;
    std::size_t one_byte_short_entries = 0u;
    std::size_t one_byte_short_retained_payload_bytes = 0u;
    std::size_t in_flight_waits = 0u;
    std::uint64_t in_flight_wait_nanoseconds = 0u;
    std::uint64_t maximum_in_flight_wait_nanoseconds = 0u;
    std::size_t bounded_context_history_entries = 0u;
    std::size_t bounded_context_history_limit = 0u;
    std::size_t bounded_absent_history_entries = 0u;
    std::size_t bounded_absent_history_accounted_bytes = 0u;
    std::size_t bounded_absent_history_byte_limit = 0u;
    bool bounded_exact_replay_available = false;
    bool unbounded_exact_replay_available = false;
    bool unbounded_exact_replay_preserved = false;
    std::size_t coordinator_requests = 0u;
    std::size_t coordinator_producers = 0u;
    std::size_t coordinator_ready_reuses = 0u;
    std::size_t coordinator_in_flight_reuses = 0u;
    std::size_t coordinator_entries = 0u;
    std::size_t coordinator_retained_payload_bytes = 0u;
    std::size_t coordinator_evictions = 0u;
    std::size_t coordinator_session_lookups = 0u;
    std::size_t coordinator_session_entries = 0u;
    std::size_t coordinator_physical_computations = 0u;
    bool coordinator_collision_safe = false;
    bool coordinator_failure_pinned = false;
    bool coordinator_ready_admission_fallback_recomputed = false;
    bool coordinator_concurrent_admission_fallback_independent = false;
    bool throwing_observer_semantics_preserved = false;
};

// Deterministic retail-free stress probe for the cache-observability
// contract. It exercises every lookup outcome and every primary miss reason
// without exposing the private evaluation artifact type to tests.
[[nodiscard]] FunctionEvaluationCacheTelemetryProbe
probe_function_evaluation_cache_telemetry_for_testing();

// Narrow fault-observation hook for the parallel resolution lifetime
// regressions. Product sessions leave every callback empty.
struct ResolutionExecutionObserverForTesting final {
    std::function<void(std::size_t)> job_started;
    std::function<void(std::size_t)> job_completed;
    std::function<void(std::size_t)> commit;
};

// Narrow, run-local snapshot of one Contextual-Return fixpoint scheduler
// invocation. All counters here exclude the stable GuardedInventory harvest.
// This is test-only evidence: it intentionally carries counters and stable
// addresses, never AbstractState payloads, cache keys, artifacts, or canonical
// result data. Product sessions leave the observer empty.
struct ContextualReturnSchedulerDiagnosticsForTesting final {
    std::size_t root_index = 0u;
    std::uint32_t root_address = 0u;
    // Zero means that this scheduler did not know which lane exhausted the
    // shared ResolutionRoot logical budget.
    std::uint32_t limiting_contextual_function_address = 0u;
    std::size_t root_lane_creations = 0u;
    std::size_t descendant_lane_creations = 0u;
    // Newly admitted immutable Full-State semantic lanes. Exact subscriber
    // replays of an already admitted lane do not consume this budget.
    std::size_t fixpoint_scheduler_logical_admissions = 0u;
    // SemanticLane groups first created by this scheduler invocation; a reuse
    // counts every exact item beyond the first new producer for its group,
    // including a request to an already admitted lane.
    std::size_t semantic_lane_creations = 0u;
    std::size_t semantic_lane_reuses = 0u;
    // Exact items whose canonical semantic artifact was restored for their
    // own provenance capsule. This includes a group's representative item.
    std::size_t exact_subscriber_replays = 0u;
    // Physical evaluate_function calls, including calls that subsequently
    // failed. A successful request without such a call is a cache reuse.
    std::size_t fixpoint_scheduler_physical_evaluations = 0u;
    std::size_t fixpoint_scheduler_successful_request_reuses = 0u;
    std::size_t fixpoint_scheduler_failed_requests_without_physical_evaluation =
        0u;
    // Bounded cardinality of pairs of authoritative cache-key digests for
    // alpha-normalized requests. This is digest accounting, not collision-safe
    // state equality; degraded accounting reports dropped/unavailable keys.
    std::size_t alpha_normalized_request_key_digest_cardinality = 0u;
    std::size_t alpha_normalized_request_key_digest_dropped = 0u;
    bool alpha_normalized_request_key_digest_cardinality_degraded = false;
    std::size_t alpha_normalization_fallbacks = 0u;
    std::size_t semantic_lane_widenings = 0u;
    std::size_t provenance_only_lane_widenings = 0u;
    // A failed observer-only classification never interrupts the canonical
    // lane commit; dropped records make that degradation explicit.
    std::size_t lane_widening_classification_dropped = 0u;
    bool lane_widening_classification_degraded = false;
    std::size_t requeues_initial_root_seed = 0u;
    std::size_t requeues_new_lane = 0u;
    std::size_t requeues_input_widening = 0u;
    std::size_t requeues_summary_change = 0u;
    std::size_t requeues_forward_edge_insert_or_widen = 0u;
    std::size_t requeues_stale_dependency = 0u;
    // Version-stale Jacobi snapshots discarded before publication. This counts
    // the stale decision even when its subsequent requeue is queue-deduped;
    // a graph-only invalidation does not contribute.
    std::size_t stale_snapshot_discards = 0u;
    // Only actual scheduler enqueue causes are represented above. In
    // particular, evidence-layout and cache-reuse causes are not observed.
    bool contextual_context_budget_exhausted = false;
    bool contextual_evaluation_budget_exhausted = false;
    // Composite Contextual-Return logical-budget status. It may be true while
    // neither contextual sub-budget is exhausted (for example a shared
    // ResolutionRoot budget was exhausted by a forwarded sibling).
    bool composite_logical_budget_exhausted = false;
    // The RAII publisher sets this before callback delivery when the
    // scheduler scope leaves through exception unwinding.
    bool invocation_aborted_by_exception = false;
};

using ContextualReturnSchedulerDiagnosticsObserverForTesting =
    std::function<void(const ContextualReturnSchedulerDiagnosticsForTesting&)>;

enum class ContextualReturnJacobiFaultHookPointForTesting : std::uint8_t {
    BeforeEvaluation,
    BeforeStaleFreeze,
};

// Test-only Jacobi fault/observation event. Product sessions leave the
// callback empty; an event accepted at BeforeEvaluation becomes item.error
// and skips that worker evaluation. BeforeStaleFreeze is observation only.
struct ContextualReturnJacobiFaultEventForTesting final {
    ContextualReturnJacobiFaultHookPointForTesting point =
        ContextualReturnJacobiFaultHookPointForTesting::BeforeEvaluation;
    std::size_t root_index = 0u;
    std::size_t batch_index = 0u;
    std::size_t batch_size = 0u;
    std::size_t lane_id = 0u;
    std::uint32_t function_address = 0u;
};

struct ContextualReturnJacobiFaultHookForTesting final {
    std::function<bool(const ContextualReturnJacobiFaultEventForTesting&)>
        callback;
    // Zero preserves the production scheduler width. Tests may select a
    // deterministic Jacobi snapshot width without changing worker parallelism.
    std::size_t maximum_batch_size = 0u;
};

enum class PersistentFunctionAnalysisEpochImportStatus : std::uint8_t {
    Imported,
    Empty,
    SchemaMismatch,
    ImplementationMismatch,
    ImageMismatch,
    ProgramMismatch,
    Incomplete,
    Corrupt,
    ResourceLimit,
};

struct PersistentFunctionAnalysisEpochImportResult final {
    PersistentFunctionAnalysisEpochImportStatus status =
        PersistentFunctionAnalysisEpochImportStatus::Empty;
    std::size_t artifact_bytes = 0u;
    std::size_t program_functions = 0u;
    std::size_t resolution_roots = 0u;

    [[nodiscard]] bool imported() const noexcept {
        return status ==
               PersistentFunctionAnalysisEpochImportStatus::Imported;
    }
};

class FunctionValueAnalysisSession {
  public:
    static constexpr std::size_t
        default_maximum_resolution_dependency_nodes = 65'536u;
    static constexpr std::size_t
        default_maximum_resolution_root_artifacts = 16'384u;
    static constexpr std::size_t
        default_maximum_resolution_epoch_retained_bytes =
            512u * 1024u * 1024u;
    static constexpr std::size_t
        default_maximum_persistent_epoch_blob_bytes =
            768u * 1024u * 1024u;

    explicit FunctionValueAnalysisSession(
        std::size_t maximum_entries = 16'384u,
        std::size_t maximum_retained_payload_bytes =
            1'024u * 1024u * 1024u,
        bool detailed_telemetry = false,
        FunctionEvaluationCacheDecisionObserver decision_observer = {},
        std::size_t maximum_resolution_dependency_nodes =
            default_maximum_resolution_dependency_nodes,
        std::size_t maximum_resolution_root_artifacts =
            default_maximum_resolution_root_artifacts,
        std::size_t maximum_resolution_epoch_retained_bytes =
            default_maximum_resolution_epoch_retained_bytes,
        AnalysisMemoryBudget* pre_reserved_resolution_ready_budget =
            nullptr,
        // Optional parent-accounted cache arena. Cache retention is always
        // optional: a full parent only evicts exact-replay aliases and never
        // changes logical FVA limits or canonical output.
        AnalysisMemoryBudget* retained_cache_memory_budget = nullptr);
    ~FunctionValueAnalysisSession();

    FunctionValueAnalysisSession(FunctionValueAnalysisSession&&) noexcept;
    FunctionValueAnalysisSession& operator=(
        FunctionValueAnalysisSession&&) noexcept;

    FunctionValueAnalysisSession(const FunctionValueAnalysisSession&) =
        delete;
    FunctionValueAnalysisSession& operator=(
        const FunctionValueAnalysisSession&) = delete;

    [[nodiscard]] FunctionValueAnalysisSessionStatistics
    statistics() const;

    [[nodiscard]] std::uint64_t published_epoch_version() const;

    // Opaque, cross-process baseline for the expensive immutable program and
    // resolution-owner domains. The component identity is supplied by the
    // product cache layer and is part of the authenticated artifact envelope.
    // An empty export means that no complete, bounded epoch is publishable.
    [[nodiscard]] std::vector<std::uint8_t>
    export_persistent_epoch_shards(
        const katana::io::ExecutableImage& image,
        std::string_view implementation_identity,
        std::size_t maximum_blob_bytes =
            default_maximum_persistent_epoch_blob_bytes) const;

    // Import is transactional. It validates the current image and complete
    // program input before publishing an immutable baseline and stages the
    // matching Unchanged journal for exactly one subsequent analysis call.
    // Missing semantic owner state is never represented by an empty result:
    // the imported presentation may be consumed directly only when complete;
    // otherwise the ordinary analysis path recomputes every missing owner.
    [[nodiscard]] PersistentFunctionAnalysisEpochImportResult
    import_persistent_epoch_shards(
        const katana::io::ExecutableImage& image,
        std::span<const katana::sh4::DisassemblyLine> lines,
        std::span<const FunctionBoundary> function_boundaries,
        std::span<const ResolvedControlFlowEdge> resolved_edges,
        std::span<const std::uint8_t> blob,
        std::string_view implementation_identity,
        FunctionValueResultMaterialization result_materialization =
            FunctionValueResultMaterialization::TerminalFull,
        std::size_t maximum_blob_bytes =
            default_maximum_persistent_epoch_blob_bytes);

    // The next real analysis invocation consumes this exact producer journal.
    // It is bound to image identity, the authenticated immutable generation
    // (or an exact legacy revision), and the expected published epoch;
    // mismatches become a typed full persistent-state bypass.
    void stage_next_function_program_delta(FunctionProgramDelta delta);

    // Strong fail-closed contract: the next real invocation reads no
    // persistent graph, ABI, summary, candidate, evaluation or root state.
    void bypass_all_persistent_analysis_state_once(
        PersistentAnalysisBypassReason reason);

    // Fixpoint orchestrators may discover several independent reasons for the
    // same not-yet-consumed full-state bypass. Their semantic effect is
    // identical, so preserve the first diagnostic reason instead of treating
    // the later request as a producer-journal overwrite. Direct producers keep
    // using the strict one-shot API above.
    void ensure_all_persistent_analysis_state_bypassed_once(
        PersistentAnalysisBypassReason reason);

    // Compatibility test hook; now aliases the strong full-state bypass.
    void force_full_cpu_recompute_once();

    void set_resolution_execution_observer_for_testing(
        ResolutionExecutionObserverForTesting observer);

    void set_contextual_return_scheduler_diagnostics_observer_for_testing(
        ContextualReturnSchedulerDiagnosticsObserverForTesting observer);

    void set_contextual_return_jacobi_fault_hook_for_testing(
        ContextualReturnJacobiFaultHookForTesting hook);

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;

    friend FunctionValueAnalysisResult
    analyze_function_values_with_guarded_entry_cache(
        const katana::io::ExecutableImage& image,
        std::span<const katana::sh4::DisassemblyLine> lines,
        std::span<const FunctionBoundary> function_boundaries,
        std::span<const ResolvedControlFlowEdge> resolved_edges,
        const FunctionValueAnalysisProgressCallback& progress_callback,
        GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
        FunctionValueAnalysisSession& session,
        const AbiContractObserver& abi_contract_observer);
    friend FunctionValueAnalysisResult
    analyze_function_values_with_guarded_entry_cache_attempt(
        const katana::io::ExecutableImage& image,
        std::span<const katana::sh4::DisassemblyLine> lines,
        std::span<const FunctionBoundary> function_boundaries,
        std::span<const ResolvedControlFlowEdge> resolved_edges,
        const FunctionValueAnalysisProgressCallback& progress_callback,
        GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
        FunctionValueAnalysisSession& session,
        const AbiContractObserver& abi_contract_observer);
};

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionBoundary> function_boundaries,
    std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
    const AbiContractObserver& abi_contract_observer = {});

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionBoundary> function_boundaries,
    std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
    FunctionValueAnalysisSession& session,
    const AbiContractObserver& abi_contract_observer = {});

} // namespace katana::analysis::detail
