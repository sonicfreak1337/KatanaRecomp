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
#include <unordered_map>
#include <unordered_set>

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
          bound_image_revision_(image.analysis_revision()) {
        statistics_.work_budget = maximum_total_instructions;
    }

    // Analysis entrypoints may deliberately retain this cache across rounds.
    // Rebinding is fail-safe for both an in-place image mutation and a
    // different image instance; neither may inherit old shape proofs.
    void bind(const katana::io::ExecutableImage& image) {
        const auto identity = image.analysis_instance_identity();
        const auto revision = image.analysis_revision();
        if (image_ == &image &&
            bound_image_identity_ == identity &&
            bound_image_revision_ == revision)
            return;
        image_ = &image;
        bound_image_identity_ = identity;
        bound_image_revision_ = revision;
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
        if (const auto cached = results_.find(address); cached != results_.end())
            return cached->second;
        const auto validation = validate_decode_candidate(*image_, address);
        if (!validation.valid()) {
            return remember(address,
                            GuardedNativeEntryShapeStatus::OutsideImage);
        }
        const auto canonical_address = validation.resolved_address;
        if (const auto cached = results_.find(canonical_address); cached != results_.end()) {
            results_.insert_or_assign(address, cached->second);
            return cached->second;
        }
        const auto result = validate(canonical_address);
        static_cast<void>(remember(canonical_address, result));
        results_.insert_or_assign(address, result);
        return result;
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
    std::size_t coordinator_session_lookups = 0u;
    std::size_t coordinator_session_entries = 0u;
    std::size_t coordinator_physical_computations = 0u;
    bool coordinator_collision_safe = false;
    bool coordinator_failure_pinned = false;
    bool throwing_observer_semantics_preserved = false;
};

// Deterministic retail-free stress probe for the cache-observability
// contract. It exercises every lookup outcome and every primary miss reason
// without exposing the private evaluation artifact type to tests.
[[nodiscard]] FunctionEvaluationCacheTelemetryProbe
probe_function_evaluation_cache_telemetry_for_testing();

class FunctionValueAnalysisSession {
  public:
    static constexpr std::size_t
        default_maximum_resolution_dependency_nodes = 65'536u;
    static constexpr std::size_t
        default_maximum_resolution_root_artifacts = 16'384u;
    static constexpr std::size_t
        default_maximum_resolution_epoch_retained_bytes =
            512u * 1024u * 1024u;

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
            default_maximum_resolution_epoch_retained_bytes);
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

    // The next real analysis invocation consumes this exact producer journal.
    // It is bound to image identity/revision and the expected published epoch;
    // mismatches become a typed full persistent-state bypass.
    void stage_next_function_program_delta(FunctionProgramDelta delta);

    // Strong fail-closed contract: the next real invocation reads no
    // persistent graph, ABI, summary, candidate, evaluation or root state.
    void bypass_all_persistent_analysis_state_once(
        PersistentAnalysisBypassReason reason);

    // Compatibility test hook; now aliases the strong full-state bypass.
    void force_full_cpu_recompute_once();

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
