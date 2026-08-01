#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/instruction.hpp"
#include "guarded_native_entry_shape.hpp"
#include "snapshot_pointer_candidates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis {
namespace {

std::atomic_size_t function_value_progress_callback_activations = 0u;
std::atomic_size_t function_value_progress_pulse_threads_started = 0u;
std::atomic_size_t function_value_detailed_cache_sessions_started = 0u;

bool analyzer_stack_diagnostics_enabled() noexcept {
    const auto* const value =
        std::getenv("CODEX_ANALYZER_STACK_DIAGNOSTICS");
    return value != nullptr && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

bool analyzer_fixpoint_trace_enabled() noexcept {
    // "1" keeps the bounded loss capsules useful for ordinary diagnostics.
    // The substantially noisier fixpoint stream is opt-in so redirected
    // stderr cannot dominate the analysis it is meant to measure.
    const auto* const value =
        std::getenv("CODEX_ANALYZER_STACK_DIAGNOSTICS");
    return value != nullptr &&
           (std::strcmp(value, "trace") == 0 ||
            std::strcmp(value, "verbose") == 0);
}

void emit_analyzer_fixpoint_trace(const char* const phase,
                                  const std::size_t iteration,
                                  const std::uint32_t function,
                                  const std::uint32_t block,
                                  const std::size_t pending) {
    if (!analyzer_fixpoint_trace_enabled()) return;
    const bool local = std::strcmp(phase, "local") == 0;
    const bool power_of_two =
        iteration != 0u && (iteration & (iteration - 1u)) == 0u;
    if ((!local && iteration > 16u &&
         !power_of_two && iteration % 128u != 0u) ||
        (local && iteration < 64u))
        return;
    constexpr std::size_t maximum_fixpoint_trace_lines = 16'384u;
    static std::atomic_size_t emitted_lines = 0u;
    const auto line =
        emitted_lines.fetch_add(1u, std::memory_order_relaxed);
    if (line >= maximum_fixpoint_trace_lines) return;
    std::fprintf(stderr,
                 "KATANA_ANALYZER_FIXPOINT phase=%s iteration=%zu "
                 "function=0x%08X block=0x%08X pending=%zu\n",
                 phase,
                 iteration,
                 static_cast<unsigned int>(function),
                 static_cast<unsigned int>(block),
                 pending);
}

constexpr std::size_t maximum_detailed_analyzer_diagnostics = 64u;

using DetailedAnalyzerDiagnosticKey =
    std::tuple<std::uint8_t,
               std::uint32_t,
               std::uint32_t,
               std::uint32_t,
               std::uint8_t>;

struct DetailedAnalyzerDiagnosticState {
    std::mutex mutex;
    std::set<DetailedAnalyzerDiagnosticKey> emitted;
    std::map<std::uint8_t, std::size_t> emitted_by_kind;
    std::uint64_t epoch = 0u;
};

DetailedAnalyzerDiagnosticState& detailed_analyzer_diagnostic_state() {
    static DetailedAnalyzerDiagnosticState state;
    return state;
}

void begin_detailed_analyzer_diagnostic_epoch() {
    if (!analyzer_stack_diagnostics_enabled()) return;
    auto& state = detailed_analyzer_diagnostic_state();
    const std::lock_guard lock(state.mutex);
    state.emitted.clear();
    state.emitted_by_kind.clear();
    ++state.epoch;
    std::fprintf(stderr,
                 "KATANA_ANALYZER_DIAGNOSTIC_EPOCH id=%llu\n",
                 static_cast<unsigned long long>(state.epoch));
}

void emit_bounded_analyzer_diagnostic(
    const std::uint8_t kind,
    const std::uint32_t owner,
    const std::uint32_t site,
    const std::uint32_t target,
    const std::uint8_t reason,
    const std::function<void()>& emit) {
    if (!analyzer_stack_diagnostics_enabled()) return;
    auto& state = detailed_analyzer_diagnostic_state();
    const std::lock_guard lock(state.mutex);
    auto& emitted_for_kind = state.emitted_by_kind[kind];
    if (emitted_for_kind >= maximum_detailed_analyzer_diagnostics ||
        !state.emitted.emplace(kind, owner, site, target, reason).second)
        return;
    ++emitted_for_kind;
    emit();
}

void emit_analyzer_stack_diagnostic(const char* const kind,
                                    const std::uint32_t owner,
                                    const std::uint32_t site,
                                    const std::uint32_t target) {
    if (!analyzer_stack_diagnostics_enabled()) return;
    std::fprintf(stderr,
                 "KATANA_ANALYZER_STACK_LOSS kind=%s owner=0x%08X "
                 "site=0x%08X target=0x%08X\n",
                 kind,
                 static_cast<unsigned int>(owner),
                 static_cast<unsigned int>(site),
                 static_cast<unsigned int>(target));
}

void emit_contextual_return_limit_diagnostic(
    const char* const reason,
    const std::uint8_t reason_id,
    const std::uint32_t owner,
    const std::uint32_t current,
    const std::uint32_t target,
    const std::size_t context_count,
    const std::size_t evaluation_count,
    const std::size_t pending_count) {
    emit_bounded_analyzer_diagnostic(
        2u,
        owner,
        current,
        target,
        reason_id,
        [&] {
            std::fprintf(
                stderr,
                "KATANA_ANALYZER_CONTEXT_LIMIT reason=%s owner=0x%08X "
                "current=0x%08X target=0x%08X contexts=%zu "
                "evaluations=%zu pending=%zu\n",
                reason,
                static_cast<unsigned int>(owner),
                static_cast<unsigned int>(current),
                static_cast<unsigned int>(target),
                context_count,
                evaluation_count,
                pending_count);
        });
}

constexpr std::size_t maximum_summary_values = 8u;
constexpr std::size_t maximum_guarded_code_inventory = 1'024u;
constexpr std::size_t maximum_raw_stored_code_candidates =
    maximum_guarded_code_inventory * 4u;
constexpr std::size_t reserved_returned_table_targets = 256u;
constexpr std::int32_t maximum_stack_distance = 65'536;
constexpr std::size_t maximum_fixpoint_iterations = 65'536u;
constexpr std::size_t maximum_local_fixpoint_iterations = 65'536u;
// ABI stack arguments are four-byte slots in the already bounded
// [0, maximum_stack_distance] domain. The projection/read-set budget covers
// that complete semantic domain; the smaller flow-state cap below is only an
// implementation guard for transient local taints.
constexpr std::size_t maximum_abi_stack_argument_slots =
    static_cast<std::size_t>(maximum_stack_distance) / 4u + 1u;
constexpr std::size_t maximum_abi_persistent_flow_stack_slots = 256u;
static_assert(maximum_abi_persistent_flow_stack_slots <
              maximum_abi_stack_argument_slots);
constexpr std::size_t maximum_forwarded_store_contexts =
    maximum_guarded_code_inventory;
// Semantic contexts, their distinct isolated roots and re-evaluations are
// independently bounded. Any loss is reported through the existing forwarding
// truncation contract and blocks a product export.
constexpr std::size_t maximum_forwarded_store_context_root_call_sites =
    64u;
constexpr std::size_t maximum_forwarded_store_context_evaluations =
    64u;
constexpr std::size_t maximum_contextual_return_evaluations =
    maximum_fixpoint_iterations;
constexpr std::size_t maximum_inventory_stack_coordinates = 64u;
constexpr std::size_t maximum_inventory_regions = maximum_guarded_code_inventory;
constexpr std::size_t maximum_inventory_region_blocks = 256u;
constexpr std::size_t maximum_memory_values = 256u;
constexpr std::size_t maximum_parallel_resolution_jobs = 64u;
constexpr std::size_t minimum_parallel_resolution_functions = 2u;
constexpr std::size_t maximum_abi_stack_read_top_chain = 16u;
constexpr std::size_t maximum_evaluation_inventory_replay_bytes =
    256u * 1024u * 1024u;

enum class AbiStackReadTopReason : std::uint8_t {
    None,
    InvalidSlot,
    SlotBudget,
    LocalStackCoordinate,
    CalleeSetIncomplete,
    EmptyCalleeSet,
    ReadMapUnavailable,
    CalleeMissing,
    CalleeTop,
    CallerStackCoordinate,
    ComposeRange,
    MissingBlock,
    MissingDelay,
    IndirectNoIngress,
    ExternalSuccessorNoIngress,
    FixpointSlotBudget
};

[[nodiscard]] const char* abi_stack_read_top_reason_name(
    const AbiStackReadTopReason reason) noexcept {
    switch (reason) {
    case AbiStackReadTopReason::None: return "none";
    case AbiStackReadTopReason::InvalidSlot: return "invalid-slot";
    case AbiStackReadTopReason::SlotBudget: return "slot-budget";
    case AbiStackReadTopReason::LocalStackCoordinate:
        return "local-stack-coordinate";
    case AbiStackReadTopReason::CalleeSetIncomplete:
        return "callee-set-incomplete";
    case AbiStackReadTopReason::EmptyCalleeSet:
        return "empty-callee-set";
    case AbiStackReadTopReason::ReadMapUnavailable:
        return "read-map-unavailable";
    case AbiStackReadTopReason::CalleeMissing: return "callee-missing";
    case AbiStackReadTopReason::CalleeTop: return "callee-top";
    case AbiStackReadTopReason::CallerStackCoordinate:
        return "caller-stack-coordinate";
    case AbiStackReadTopReason::ComposeRange: return "compose-range";
    case AbiStackReadTopReason::MissingBlock: return "missing-block";
    case AbiStackReadTopReason::MissingDelay: return "missing-delay";
    case AbiStackReadTopReason::IndirectNoIngress:
        return "indirect-no-ingress";
    case AbiStackReadTopReason::ExternalSuccessorNoIngress:
        return "external-successor-no-ingress";
    case AbiStackReadTopReason::FixpointSlotBudget:
        return "fixpoint-slot-budget";
    }
    return "unknown";
}

struct AbiStackReadTopFrame {
    AbiStackReadTopReason reason = AbiStackReadTopReason::None;
    std::uint32_t owner = 0u;
    std::uint32_t site = 0u;
    std::uint32_t target = 0u;
    bool contract_present = false;
    bool contract_complete = false;
    bool ingress_present = false;
    bool ingress_guarded = false;
    bool ingress_complete = false;
    bool residual_indirect = false;
    bool external_successor = false;
};

// A complete set contains every non-negative, word-aligned stack argument
// slot which a callee can read before it is definitely overwritten. An
// incomplete set is conservative Top and must retain the legacy full
// projection; it is never equivalent to an empty set.
struct AbiStackArgumentReadSet {
    std::vector<std::int32_t> slots;
    bool complete = true;
    // Diagnostic-only provenance. It is deliberately excluded from semantic
    // equality so enabling diagnostics cannot alter fixed-point convergence.
    std::vector<AbiStackReadTopFrame> top_chain;
    bool top_chain_truncated = false;

    bool operator==(const AbiStackArgumentReadSet& other) const {
        return slots == other.slots && complete == other.complete;
    }
};

using AbiStackArgumentReadMap =
    std::unordered_map<std::uint32_t, AbiStackArgumentReadSet>;

void make_abi_stack_argument_reads_unknown(
    AbiStackArgumentReadSet& reads) {
    reads.slots.clear();
    reads.complete = false;
}

void set_abi_stack_read_top_reason(
    AbiStackReadTopReason* const destination,
    const AbiStackReadTopReason reason) {
    if (destination != nullptr &&
        *destination == AbiStackReadTopReason::None)
        *destination = reason;
}

void record_abi_stack_read_top(
    AbiStackArgumentReadSet& reads,
    const AbiStackReadTopFrame frame,
    const AbiStackArgumentReadSet* const parent = nullptr) {
    if (!analyzer_stack_diagnostics_enabled() ||
        frame.reason == AbiStackReadTopReason::None ||
        !reads.top_chain.empty())
        return;
    reads.top_chain.push_back(frame);
    if (parent == nullptr) return;
    for (const auto& parent_frame : parent->top_chain) {
        if (reads.top_chain.size() >= maximum_abi_stack_read_top_chain) {
            reads.top_chain_truncated = true;
            break;
        }
        if (parent_frame.owner == frame.owner) {
            reads.top_chain_truncated = true;
            break;
        }
        reads.top_chain.push_back(parent_frame);
    }
    reads.top_chain_truncated =
        reads.top_chain_truncated || parent->top_chain_truncated;
}

void copy_abi_stack_read_top(
    AbiStackArgumentReadSet& destination,
    const AbiStackArgumentReadSet& source) {
    if (!analyzer_stack_diagnostics_enabled() ||
        !destination.top_chain.empty())
        return;
    destination.top_chain = source.top_chain;
    destination.top_chain_truncated = source.top_chain_truncated;
}

bool insert_abi_stack_argument_read(AbiStackArgumentReadSet& reads,
                                    const std::int64_t slot,
                                    AbiStackReadTopReason* const
                                        top_reason = nullptr) {
    if (!reads.complete) return false;
    if (slot < 0 || slot > maximum_stack_distance || (slot & 3) != 0) {
        set_abi_stack_read_top_reason(
            top_reason, AbiStackReadTopReason::InvalidSlot);
        make_abi_stack_argument_reads_unknown(reads);
        return false;
    }
    const auto value = static_cast<std::int32_t>(slot);
    const auto position =
        std::lower_bound(reads.slots.begin(), reads.slots.end(), value);
    if (position != reads.slots.end() && *position == value) return false;
    if (reads.slots.size() >= maximum_abi_stack_argument_slots) {
        set_abi_stack_read_top_reason(
            top_reason, AbiStackReadTopReason::SlotBudget);
        make_abi_stack_argument_reads_unknown(reads);
        return false;
    }
    reads.slots.insert(position, value);
    return true;
}

bool merge_abi_stack_argument_reads(
    AbiStackArgumentReadSet& destination,
    const AbiStackArgumentReadSet& source) {
    if (!destination.complete) return false;
    if (!source.complete) {
        make_abi_stack_argument_reads_unknown(destination);
        copy_abi_stack_read_top(destination, source);
        return true;
    }
    const auto previous = destination;
    for (const auto slot : source.slots) {
        static_cast<void>(
            insert_abi_stack_argument_read(destination, slot));
        if (!destination.complete) break;
    }
    return destination != previous;
}

std::vector<std::vector<std::uint32_t>>
strong_components(const std::span<const FunctionInfo> functions) {
    std::unordered_map<std::uint32_t, const FunctionInfo*> by_address;
    by_address.reserve(functions.size());
    for (const auto& function : functions)
        by_address.emplace(function.entry_address, &function);
    std::unordered_map<std::uint32_t, std::size_t> index;
    std::unordered_map<std::uint32_t, std::size_t> lowlink;
    std::unordered_set<std::uint32_t> on_stack;
    std::vector<std::uint32_t> stack;
    std::vector<std::vector<std::uint32_t>> components;
    index.reserve(functions.size());
    lowlink.reserve(functions.size());
    on_stack.reserve(functions.size());
    stack.reserve(functions.size());
    components.reserve(functions.size());
    std::size_t next_index = 0u;
    std::function<void(std::uint32_t)> visit = [&](const std::uint32_t address) {
        index.emplace(address, next_index);
        lowlink.emplace(address, next_index++);
        stack.push_back(address);
        on_stack.insert(address);
        const auto found = by_address.find(address);
        if (found != by_address.end()) {
            for (const auto callee : found->second->direct_callees) {
                if (!by_address.contains(callee)) continue;
                if (!index.contains(callee)) {
                    visit(callee);
                    lowlink[address] = std::min(lowlink[address], lowlink[callee]);
                } else if (on_stack.contains(callee)) {
                    lowlink[address] = std::min(lowlink[address], index[callee]);
                }
            }
        }
        if (lowlink[address] != index[address]) return;
        auto& component = components.emplace_back();
        for (;;) {
            const auto member = stack.back();
            stack.pop_back();
            on_stack.erase(member);
            component.push_back(member);
            if (member == address) break;
        }
        std::sort(component.begin(), component.end());
    };
    for (const auto& function : functions) {
        if (!index.contains(function.entry_address)) visit(function.entry_address);
    }
    std::reverse(components.begin(), components.end());
    return components;
}

struct InventorySavedStackSlot {
    std::int32_t relative_slot = 0;
    std::vector<std::uint32_t> inventory_code_pointer_values;
    std::vector<std::uint32_t>
        inventory_pc_relative_code_literal_values;
    bool inventory_code_pointer_values_truncated = false;
    bool inventory_pc_relative_code_literal_values_truncated = false;
    bool contextual_candidate_dependency = false;
    std::set<std::uint32_t> call_sites;
    std::set<std::uint32_t> callees;

    bool operator==(const InventorySavedStackSlot&) const = default;
};

struct InventorySavedStackEpoch {
    // A pending epoch is attached only to a detached value that is known to
    // have denoted a current stack coordinate when it was stored. Its slots
    // are candidate-only and already relative to that saved pointer.
    bool present = false;
    bool unresolved = false;
    // True only while the captured coordinates still refer to the currently
    // active AbstractState stack namespace. A stack switch detaches every
    // surviving reference before the new epoch starts.
    bool tracks_current_epoch = false;
    // Sticky evidence that finite callback candidates were discarded when
    // this epoch reached its terminal unresolved top. A consumed top with no
    // candidate loss need not poison unrelated inventory sinks.
    bool candidate_payload_lost = false;
    std::vector<InventorySavedStackSlot> slots;

    bool operator==(const InventorySavedStackEpoch&) const = default;
};

struct AbstractValue {
    bool known = false;
    bool guarded = false;
    bool complete = false;
    // Inventory-only must-provenance: the value is derived from the
    // architectural stack pointer on every joined path. This is narrower than
    // "may alias stack" and lets guarded code-pointer inventory distinguish an
    // actual stack spill from a mixed stack/object-pointer join.
    bool inventory_stack_derived = false;
    // Inventory-only evidence that the value crossed a native-code argument
    // boundary as a finite, decode-valid address.  Generic call-site
    // provenance is deliberately not strong enough for this purpose.
    bool inventory_code_pointer = false;
    // Inventory-only evidence that this value came from a 32-bit PC-relative
    // image literal and already denoted a decode-valid native-code candidate.
    // It is not a code-pointer proof by itself; a real call/tail ABI boundary
    // must still promote it.
    bool inventory_pc_relative_code_literal = false;
    std::vector<std::uint32_t> inventory_code_pointer_values;
    std::vector<std::uint32_t> inventory_pc_relative_code_literal_values;
    bool inventory_code_pointer_values_truncated = false;
    bool inventory_pc_relative_code_literal_values_truncated = false;
    // Context-only candidate-return taint. It never proves a dispatch edge or
    // a code pointer; it only keeps the bounded helper slice attached to the
    // ABI values that originated at a guarded candidate call.
    bool contextual_candidate_dependency = false;
    // Value-scoped fail-closed provenance. Unlike the state-wide unresolved
    // stack pool, this bit dies when the value is overwritten and therefore
    // cannot poison an unrelated later inventory sink.
    bool inventory_stack_callback_loss_unresolved = false;
    // Inventory-only suspended-stack payload. It cannot make this value known
    // and cannot establish a semantic memory or control-flow fact.
    InventorySavedStackEpoch inventory_saved_stack_epoch;
    std::vector<std::uint32_t> values;
    std::set<std::uint32_t> call_sites;
    std::set<std::uint32_t> callees;

    bool operator==(const AbstractValue&) const = default;
};

constexpr std::uint8_t unresolved_saved_stack_alias_source_stack = 1u;
constexpr std::uint8_t unresolved_saved_stack_alias_source_memory = 2u;

struct AbstractState {
    std::array<AbstractValue, 16u> registers;
    std::array<std::optional<std::int32_t>, 16u> stack_offsets;
    // A separate coordinate system for the guarded inventory.  Writable
    // captured literals may advance this proof, but never the authoritative
    // stack offsets used by ordinary CFG and memory reasoning.
    std::array<std::optional<std::int32_t>, 16u> inventory_stack_offsets;
    // FPSCR.SZ-dependent FMOV stack updates can have more than one concrete
    // coordinate. These candidates remain inventory-only: they may retain
    // guarded callback provenance, but never establish semantic memory or CFG
    // facts. A non-empty set is definite only because merge_state drops it
    // whenever any incoming path lacks a stack coordinate.
    std::array<std::vector<std::int32_t>, 16u>
        inventory_stack_offset_candidates;
    std::array<bool, 16u> stack_may_alias = [] {
        std::array<bool, 16u> result{};
        result.fill(true);
        return result;
    }();
    // This narrower provenance is consumed only by the guarded native-code
    // inventory observers.  In particular it must never make ordinary stack,
    // memory or control-flow reasoning less conservative.
    std::array<bool, 16u> inventory_stack_may_alias = [] {
        std::array<bool, 16u> result{};
        result.fill(true);
        return result;
    }();
    // Internal, inventory-only proof that a register still denotes VBR plus a
    // constant displacement.  It is deliberately not part of the public value
    // summaries and must never create a fixed control-flow edge.
    std::array<bool, 16u> inventory_vbr_relative{};
    // Inventory-only provenance for a pointer/reference path seeded by a
    // finite PC-relative storage address. It is neither a non-stack proof nor
    // code-pointer provenance for a loaded value; it only permits direct
    // PC-relative callback-literal stores as guarded inventory candidates.
    std::array<bool, 16u> inventory_fixed_storage_reference{};
    std::map<std::int32_t, AbstractValue> stack_values;
    std::map<std::uint32_t, AbstractValue> memory_values;
    // A payload-free saved-SP alias whose exact bounded cell identity was
    // widened away. Unlike callback loss, this is harmless until the current
    // stack epoch later receives a relevant callback candidate. Source bits
    // allow unknown stack/memory loads to materialize the latent value again.
    std::uint8_t inventory_unresolved_saved_stack_alias_sources = 0u;
    bool inventory_unresolved_saved_stack_alias_tracks_current_epoch = false;
    // Ephemeral callee-local watcher for exact saved-SP aliases that were
    // intentionally omitted by a complete ABI read projection. It upgrades
    // to real loss on mutation, but is never exported through a function
    // summary: the caller still owns the exact alias.
    bool inventory_current_stack_epoch_alias_watcher = false;
    // The watched alias can outlive a temporary stack switch. Keep that
    // possibility separate from the currently active epoch so writes on an
    // unrelated resumed stack do not immediately become loss. Restoring any
    // saved epoch rearms the current watcher conservatively.
    bool inventory_detached_stack_epoch_alias_watcher = false;
    bool inventory_unresolved_stack_callback_loss = false;
    // Sticky state-level evidence that an exact register/slot/cell identity
    // carrying the loss marker exceeded its bounded domain. This is reported
    // through the existing candidate-truncation product gate.
    bool inventory_stack_callback_loss_identity_truncated = false;

    AbstractState() {
        registers[15u].inventory_stack_derived = true;
        inventory_stack_offsets[15u] = 0;
    }

    AbstractValue& operator[](const std::size_t index) {
        return registers[index];
    }
    const AbstractValue& operator[](const std::size_t index) const {
        return registers[index];
    }
    auto begin() noexcept {
        return registers.begin();
    }
    auto end() noexcept {
        return registers.end();
    }
    auto begin() const noexcept {
        return registers.begin();
    }
    auto end() const noexcept {
        return registers.end();
    }
    constexpr std::size_t size() const noexcept {
        return registers.size();
    }

    bool operator==(const AbstractState&) const = default;
};

struct InventoryStackOffsetLossDiagnostic {
    const AbstractState& state;
    std::uint32_t site = 0u;
    std::optional<std::int32_t> before;

    ~InventoryStackOffsetLossDiagnostic() {
        if (!analyzer_stack_diagnostics_enabled() || !before.has_value() ||
            state.inventory_stack_offsets[15u].has_value() ||
            !state.inventory_stack_offset_candidates[15u].empty())
            return;
        std::fprintf(stderr,
                     "KATANA_ANALYZER_INVENTORY_SP_LOSS site=0x%08X "
                     "before=%d derived=%u\n",
                     static_cast<unsigned int>(site),
                     *before,
                     state[15u].inventory_stack_derived);
    }
};

bool merge_inventory_candidate_values(
    std::vector<std::uint32_t>& destination,
    bool& destination_truncated,
    std::span<const std::uint32_t> source,
    bool source_truncated);

bool merge_value(AbstractValue& destination, const AbstractValue& source);
bool merge_state(AbstractState& destination,
                 const AbstractState& source,
                 bool may_merge_stack_values);
[[nodiscard]] bool has_saved_stack_epoch(const AbstractValue& value);
[[nodiscard]] bool carries_unresolved_stack_callback(
    const AbstractValue& value);
[[nodiscard]] bool carries_stack_callback_payload(
    const AbstractValue& value);
[[nodiscard]] bool has_latent_saved_stack_alias(
    const AbstractValue& value);
[[nodiscard]] bool has_active_inventory_stack_payload(
    const AbstractState& state);
bool add_unresolved_saved_stack_alias(
    AbstractState& state,
    std::uint8_t sources,
    bool tracks_current_epoch);
void materialize_unresolved_saved_stack_alias(
    AbstractValue& value,
    const AbstractState& state,
    std::uint8_t source);
[[nodiscard]] bool has_non_epoch_abstract_fact(
    const AbstractValue& value);
void collapse_payload_free_stack_aliases(AbstractState& state);

[[nodiscard]] bool same_saved_stack_epoch_shape(
    const InventorySavedStackEpoch& left,
    const InventorySavedStackEpoch& right) {
    if (left.present != right.present ||
        left.unresolved != right.unresolved ||
        left.tracks_current_epoch != right.tracks_current_epoch ||
        left.candidate_payload_lost !=
            right.candidate_payload_lost ||
        left.slots.size() != right.slots.size())
        return false;
    for (std::size_t index = 0u; index < left.slots.size(); ++index) {
        if (left.slots[index].relative_slot !=
            right.slots[index].relative_slot)
            return false;
    }
    return true;
}

void mark_inventory_saved_stack_epoch_unresolved(
    InventorySavedStackEpoch& epoch,
    const bool candidate_payload_lost = false) {
    // Unresolved is the terminal, absorbing top of this candidate-only
    // domain. Retaining finite slots after reaching top lets translated
    // loop-carried snapshots keep changing forever even though a restore
    // already fails closed.
    epoch.present = true;
    epoch.unresolved = true;
    epoch.candidate_payload_lost =
        epoch.candidate_payload_lost ||
        candidate_payload_lost ||
        !epoch.slots.empty();
    epoch.slots.clear();
}

[[nodiscard]] bool same_saved_stack_slot_payload(
    const InventorySavedStackSlot& left,
    const InventorySavedStackSlot& right) {
    return left.inventory_code_pointer_values ==
               right.inventory_code_pointer_values &&
           left.inventory_pc_relative_code_literal_values ==
               right.inventory_pc_relative_code_literal_values &&
           left.inventory_code_pointer_values_truncated ==
               right.inventory_code_pointer_values_truncated &&
           left.inventory_pc_relative_code_literal_values_truncated ==
               right.inventory_pc_relative_code_literal_values_truncated &&
           left.contextual_candidate_dependency ==
               right.contextual_candidate_dependency &&
           left.call_sites == right.call_sites &&
           left.callees == right.callees;
}

[[nodiscard]] bool is_nonzero_uniform_saved_stack_epoch_translation(
    const InventorySavedStackEpoch& destination,
    const InventorySavedStackEpoch& source) {
    if (!destination.present || !source.present ||
        destination.unresolved || source.unresolved ||
        destination.tracks_current_epoch !=
            source.tracks_current_epoch ||
        destination.slots.empty() ||
        destination.slots.size() != source.slots.size())
        return false;
    const auto translation =
        static_cast<std::int64_t>(source.slots.front().relative_slot) -
        static_cast<std::int64_t>(
            destination.slots.front().relative_slot);
    if (translation == 0)
        return false;
    for (std::size_t index = 0u;
         index < destination.slots.size();
         ++index) {
        if (!same_saved_stack_slot_payload(destination.slots[index],
                                           source.slots[index]) ||
            static_cast<std::int64_t>(
                source.slots[index].relative_slot) -
                    static_cast<std::int64_t>(
                        destination.slots[index].relative_slot) !=
                translation)
            return false;
    }
    return true;
}

bool merge_inventory_saved_stack_epoch(
    InventorySavedStackEpoch& destination,
    const InventorySavedStackEpoch& source,
    const bool joining_alternatives) {
    const auto original = destination;
    const bool incompatible_presence =
        joining_alternatives &&
        destination.present != source.present &&
        (destination.present || source.present);
    const bool incompatible_epoch =
        joining_alternatives &&
        destination.tracks_current_epoch !=
            source.tracks_current_epoch &&
        (destination.present || source.present);
    const bool translated_alternative =
        joining_alternatives &&
        is_nonzero_uniform_saved_stack_epoch_translation(
            destination, source);
    destination.present = destination.present || source.present;
    destination.tracks_current_epoch =
        destination.tracks_current_epoch ||
        source.tracks_current_epoch;
    destination.candidate_payload_lost =
        destination.candidate_payload_lost ||
        source.candidate_payload_lost;
    if (incompatible_presence || incompatible_epoch ||
        translated_alternative || destination.unresolved ||
        source.unresolved) {
        mark_inventory_saved_stack_epoch_unresolved(
            destination,
            source.candidate_payload_lost ||
                !source.slots.empty());
        return destination != original;
    }
    for (const auto& source_slot : source.slots) {
        const auto position = std::lower_bound(
            destination.slots.begin(),
            destination.slots.end(),
            source_slot.relative_slot,
            [](const auto& slot, const auto relative_slot) {
                return slot.relative_slot < relative_slot;
            });
        if (position == destination.slots.end() ||
            position->relative_slot != source_slot.relative_slot) {
            if (destination.slots.size() >=
                maximum_guarded_code_inventory) {
                mark_inventory_saved_stack_epoch_unresolved(
                    destination);
                return destination != original;
            }
            destination.slots.insert(position, source_slot);
            continue;
        }
        auto& target_slot = *position;
        static_cast<void>(merge_inventory_candidate_values(
            target_slot.inventory_code_pointer_values,
            target_slot.inventory_code_pointer_values_truncated,
            source_slot.inventory_code_pointer_values,
            source_slot.inventory_code_pointer_values_truncated));
        static_cast<void>(merge_inventory_candidate_values(
            target_slot.inventory_pc_relative_code_literal_values,
            target_slot
                .inventory_pc_relative_code_literal_values_truncated,
            source_slot.inventory_pc_relative_code_literal_values,
            source_slot
                .inventory_pc_relative_code_literal_values_truncated));
        target_slot.contextual_candidate_dependency =
            target_slot.contextual_candidate_dependency ||
            source_slot.contextual_candidate_dependency;
        target_slot.call_sites.insert(source_slot.call_sites.begin(),
                                      source_slot.call_sites.end());
        target_slot.callees.insert(source_slot.callees.begin(),
                                   source_slot.callees.end());
    }
    return destination != original;
}

[[nodiscard]] bool same_forwarded_value_shape(const AbstractValue& left,
                                              const AbstractValue& right) {
    return left.known == right.known && left.guarded == right.guarded &&
           left.complete == right.complete &&
           left.inventory_stack_derived == right.inventory_stack_derived &&
           left.contextual_candidate_dependency ==
               right.contextual_candidate_dependency &&
           left.inventory_stack_callback_loss_unresolved ==
               right.inventory_stack_callback_loss_unresolved &&
           left.inventory_saved_stack_epoch.candidate_payload_lost ==
               right.inventory_saved_stack_epoch.candidate_payload_lost &&
           same_saved_stack_epoch_shape(
               left.inventory_saved_stack_epoch,
               right.inventory_saved_stack_epoch) &&
           left.values == right.values;
}

[[nodiscard]] bool same_forwarded_store_shape(const AbstractState& left,
                                              const AbstractState& right) {
    if (left.stack_offsets != right.stack_offsets ||
        left.inventory_stack_offsets != right.inventory_stack_offsets ||
        left.inventory_stack_offset_candidates !=
            right.inventory_stack_offset_candidates ||
        left.stack_may_alias != right.stack_may_alias ||
        left.inventory_stack_may_alias != right.inventory_stack_may_alias ||
        left.inventory_vbr_relative != right.inventory_vbr_relative ||
        left.inventory_fixed_storage_reference !=
            right.inventory_fixed_storage_reference ||
        left.inventory_unresolved_saved_stack_alias_sources !=
            right.inventory_unresolved_saved_stack_alias_sources ||
        left.inventory_unresolved_saved_stack_alias_tracks_current_epoch !=
            right
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        left.inventory_current_stack_epoch_alias_watcher !=
            right.inventory_current_stack_epoch_alias_watcher ||
        left.inventory_detached_stack_epoch_alias_watcher !=
            right.inventory_detached_stack_epoch_alias_watcher ||
        left.inventory_unresolved_stack_callback_loss !=
            right.inventory_unresolved_stack_callback_loss ||
        left.inventory_stack_callback_loss_identity_truncated !=
            right.inventory_stack_callback_loss_identity_truncated ||
        left.stack_values.size() != right.stack_values.size() ||
        left.memory_values.size() != right.memory_values.size())
        return false;
    for (std::size_t index = 0u; index < left.registers.size(); ++index) {
        if (!same_forwarded_value_shape(left.registers[index],
                                        right.registers[index]))
            return false;
    }
    const auto equal_values = [](const auto& first, const auto& second) {
        auto first_it = first.begin();
        auto second_it = second.begin();
        while (first_it != first.end()) {
            if (first_it->first != second_it->first ||
                !same_forwarded_value_shape(first_it->second,
                                             second_it->second))
                return false;
            ++first_it;
            ++second_it;
        }
        return true;
    };
    return equal_values(left.stack_values, right.stack_values) &&
           equal_values(left.memory_values, right.memory_values);
}

void emit_forwarded_cap_diagnostic(
    const ForwardedStoreContextLimitReason reason,
    const std::uint32_t owner,
    const std::uint32_t target,
    const std::uint32_t exemplar_root_call_site,
    const bool tail,
    const bool isolated,
    const std::size_t context_count,
    const std::size_t same_target_context_count,
    const std::size_t root_call_site_count,
    const std::size_t evaluation_count,
    const bool live_mask_known,
    const std::uint16_t live_mask,
    const AbiStackArgumentReadSet* const required_stack_reads,
    const AbstractState* const existing,
    const AbstractState* const incoming) {
    auto scalar_mask = std::uint32_t{0u};
    auto stack_derived_mask = std::uint32_t{0u};
    auto stack_offset_mask = std::uint32_t{0u};
    auto inventory_offset_mask = std::uint32_t{0u};
    auto stack_alias_mask = std::uint32_t{0u};
    auto inventory_alias_mask = std::uint32_t{0u};
    auto vbr_mask = std::uint32_t{0u};
    auto fixed_mask = std::uint32_t{0u};
    const auto shape_pair = existing != nullptr && incoming != nullptr;
    if (shape_pair) {
        for (std::size_t index = 0u;
             index < existing->registers.size();
             ++index) {
            const auto& left = existing->registers[index];
            const auto& right = incoming->registers[index];
            const auto bit = std::uint32_t{1u} << index;
            if (left.known != right.known ||
                left.guarded != right.guarded ||
                left.complete != right.complete ||
                left.contextual_candidate_dependency !=
                    right.contextual_candidate_dependency ||
                left.inventory_stack_callback_loss_unresolved !=
                    right.inventory_stack_callback_loss_unresolved ||
                left.inventory_saved_stack_epoch
                        .candidate_payload_lost !=
                    right.inventory_saved_stack_epoch
                        .candidate_payload_lost ||
                left.values != right.values)
                scalar_mask |= bit;
            if (left.inventory_stack_derived !=
                right.inventory_stack_derived)
                stack_derived_mask |= bit;
            if (existing->stack_offsets[index] !=
                incoming->stack_offsets[index])
                stack_offset_mask |= bit;
            if (existing->inventory_stack_offsets[index] !=
                    incoming->inventory_stack_offsets[index] ||
                existing->inventory_stack_offset_candidates[index] !=
                    incoming->inventory_stack_offset_candidates[index])
                inventory_offset_mask |= bit;
            if (existing->stack_may_alias[index] !=
                incoming->stack_may_alias[index])
                stack_alias_mask |= bit;
            if (existing->inventory_stack_may_alias[index] !=
                incoming->inventory_stack_may_alias[index])
                inventory_alias_mask |= bit;
            if (existing->inventory_vbr_relative[index] !=
                incoming->inventory_vbr_relative[index])
                vbr_mask |= bit;
            if (existing->inventory_fixed_storage_reference[index] !=
                incoming->inventory_fixed_storage_reference[index])
                fixed_mask |= bit;
        }
    }
    const auto same_map_shape = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        auto left_it = left.begin();
        auto right_it = right.begin();
        while (left_it != left.end()) {
            if (left_it->first != right_it->first ||
                !same_forwarded_value_shape(left_it->second,
                                             right_it->second))
                return false;
            ++left_it;
            ++right_it;
        }
        return true;
    };
    const auto stack_map_equal =
        shape_pair &&
        same_map_shape(existing->stack_values, incoming->stack_values);
    const auto memory_map_equal =
        shape_pair &&
        same_map_shape(existing->memory_values, incoming->memory_values);
    const auto loss_equal =
        shape_pair &&
        existing->inventory_unresolved_saved_stack_alias_sources ==
                incoming->inventory_unresolved_saved_stack_alias_sources &&
        existing
                ->inventory_unresolved_saved_stack_alias_tracks_current_epoch ==
            incoming
                ->inventory_unresolved_saved_stack_alias_tracks_current_epoch &&
        existing->inventory_current_stack_epoch_alias_watcher ==
            incoming->inventory_current_stack_epoch_alias_watcher &&
        existing->inventory_detached_stack_epoch_alias_watcher ==
            incoming->inventory_detached_stack_epoch_alias_watcher &&
        existing->inventory_unresolved_stack_callback_loss ==
                incoming->inventory_unresolved_stack_callback_loss &&
        existing->inventory_stack_callback_loss_identity_truncated ==
            incoming->inventory_stack_callback_loss_identity_truncated;
    const auto live_stack_map_known =
        shape_pair && required_stack_reads != nullptr &&
        required_stack_reads->complete;
    auto live_stack_map_equal = live_stack_map_known;
    if (live_stack_map_known) {
        for (const auto slot : required_stack_reads->slots) {
            const auto left = existing->stack_values.find(slot);
            const auto right = incoming->stack_values.find(slot);
            if ((left == existing->stack_values.end()) !=
                    (right == incoming->stack_values.end()) ||
                (left != existing->stack_values.end() &&
                 !same_forwarded_value_shape(left->second,
                                             right->second))) {
                live_stack_map_equal = false;
                break;
            }
        }
    }
    const auto reason_name = [&] {
        switch (reason) {
        case ForwardedStoreContextLimitReason::RootCallSites:
            return "roots";
        case ForwardedStoreContextLimitReason::ContextCount:
            return "contexts";
        case ForwardedStoreContextLimitReason::ReevaluationCount:
            return "reevaluations";
        }
        return "unknown";
    }();
    emit_bounded_analyzer_diagnostic(
        1u,
        owner,
        exemplar_root_call_site,
        target,
        static_cast<std::uint8_t>(reason),
        [&] {
            std::fprintf(
                stderr,
                "KATANA_ANALYZER_FORWARD_CAP reason=%s owner=0x%08X "
                "target=0x%08X root=0x%08X tail=%u isolated=%u "
                "contexts=%zu same_target_contexts=%zu roots=%zu evals=%zu "
                "live_mask_known=%u live_mask=0x%04X readset_present=%u "
                "readset_complete=%u readset_count=%zu readset=[",
                reason_name,
                static_cast<unsigned int>(owner),
                static_cast<unsigned int>(target),
                static_cast<unsigned int>(exemplar_root_call_site),
                static_cast<unsigned int>(tail),
                static_cast<unsigned int>(isolated),
                context_count,
                same_target_context_count,
                root_call_site_count,
                evaluation_count,
                static_cast<unsigned int>(live_mask_known),
                static_cast<unsigned int>(live_mask),
                static_cast<unsigned int>(
                    required_stack_reads != nullptr),
                static_cast<unsigned int>(
                    required_stack_reads != nullptr &&
                    required_stack_reads->complete),
                required_stack_reads == nullptr
                    ? 0u
                    : required_stack_reads->slots.size());
            if (required_stack_reads != nullptr) {
                for (std::size_t index = 0u;
                     index < required_stack_reads->slots.size();
                     ++index)
                    std::fprintf(stderr,
                                 "%s%d",
                                 index == 0u ? "" : ",",
                                 static_cast<int>(
                                     required_stack_reads->slots[index]));
            }
            std::fprintf(
                stderr,
                "] shape_pair=%u scalar_mismatch=0x%04X "
                "live_scalar_mismatch=0x%04X derived_mismatch=0x%04X "
                "live_derived_mismatch=0x%04X "
                "stack_offsets_mismatch=0x%04X "
                "inventory_offsets_mismatch=0x%04X "
                "aliases_mismatch=0x%04X inventory_aliases_mismatch=0x%04X "
                "vbr_mismatch=0x%04X fixed_mismatch=0x%04X "
                "stack_map_eq=%d live_stack_map_known=%u "
                "live_stack_map_eq=%d memory_map_eq=%d loss_eq=%d "
                "stack_map_sizes=%zu/%zu memory_map_sizes=%zu/%zu\n",
                static_cast<unsigned int>(shape_pair),
                static_cast<unsigned int>(scalar_mask),
                static_cast<unsigned int>(scalar_mask & live_mask),
                static_cast<unsigned int>(stack_derived_mask),
                static_cast<unsigned int>(stack_derived_mask & live_mask),
                static_cast<unsigned int>(stack_offset_mask),
                static_cast<unsigned int>(inventory_offset_mask),
                static_cast<unsigned int>(stack_alias_mask),
                static_cast<unsigned int>(inventory_alias_mask),
                static_cast<unsigned int>(vbr_mask),
                static_cast<unsigned int>(fixed_mask),
                shape_pair ? static_cast<int>(stack_map_equal) : -1,
                static_cast<unsigned int>(live_stack_map_known),
                live_stack_map_known
                    ? static_cast<int>(live_stack_map_equal)
                    : -1,
                shape_pair ? static_cast<int>(memory_map_equal) : -1,
                shape_pair ? static_cast<int>(loss_equal) : -1,
                shape_pair ? existing->stack_values.size() : 0u,
                shape_pair ? incoming->stack_values.size() : 0u,
                shape_pair ? existing->memory_values.size() : 0u,
                shape_pair ? incoming->memory_values.size() : 0u);
        });
}

bool merge_forwarded_inventory_payload(AbstractState& destination,
                                       const AbstractState& source) {
    if (!same_forwarded_store_shape(destination, source))
        throw std::logic_error(
            "Forwarded-Store-Kontexte besitzen unterschiedliche Semantik.");
    bool changed = false;
    const auto alias_sources =
        static_cast<std::uint8_t>(
            destination.inventory_unresolved_saved_stack_alias_sources |
            source.inventory_unresolved_saved_stack_alias_sources);
    if (alias_sources !=
        destination.inventory_unresolved_saved_stack_alias_sources) {
        destination.inventory_unresolved_saved_stack_alias_sources =
            alias_sources;
        changed = true;
    }
    if (source
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch &&
        !destination
             .inventory_unresolved_saved_stack_alias_tracks_current_epoch) {
        destination
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch =
            true;
        changed = true;
    }
    if (source.inventory_current_stack_epoch_alias_watcher &&
        !destination.inventory_current_stack_epoch_alias_watcher) {
        destination.inventory_current_stack_epoch_alias_watcher = true;
        changed = true;
    }
    if (source.inventory_detached_stack_epoch_alias_watcher &&
        !destination.inventory_detached_stack_epoch_alias_watcher) {
        destination.inventory_detached_stack_epoch_alias_watcher = true;
        changed = true;
    }
    if (source.inventory_unresolved_stack_callback_loss &&
        !destination.inventory_unresolved_stack_callback_loss) {
        destination.inventory_unresolved_stack_callback_loss = true;
        changed = true;
    }
    if (source.inventory_stack_callback_loss_identity_truncated &&
        !destination.inventory_stack_callback_loss_identity_truncated) {
        destination.inventory_stack_callback_loss_identity_truncated =
            true;
        changed = true;
    }
    const auto merge_value_payload = [&changed](AbstractValue& target,
                                                 const AbstractValue& input) {
        if (input.inventory_stack_callback_loss_unresolved &&
            !target.inventory_stack_callback_loss_unresolved) {
            target.inventory_stack_callback_loss_unresolved = true;
            changed = true;
        }
        changed =
            merge_inventory_candidate_values(
                target.inventory_code_pointer_values,
                target.inventory_code_pointer_values_truncated,
                input.inventory_code_pointer_values,
                input.inventory_code_pointer_values_truncated) ||
            changed;
        changed =
            merge_inventory_candidate_values(
                target.inventory_pc_relative_code_literal_values,
                target.inventory_pc_relative_code_literal_values_truncated,
                input.inventory_pc_relative_code_literal_values,
                input.inventory_pc_relative_code_literal_values_truncated) ||
            changed;
        changed =
            merge_inventory_saved_stack_epoch(
                target.inventory_saved_stack_epoch,
                input.inventory_saved_stack_epoch,
                false) ||
            changed;
        const auto inventory_code_pointer =
            !target.inventory_code_pointer_values.empty();
        const auto inventory_pc_relative_code_literal =
            !target.inventory_pc_relative_code_literal_values.empty();
        if (target.inventory_code_pointer != inventory_code_pointer) {
            target.inventory_code_pointer = inventory_code_pointer;
            changed = true;
        }
        if (target.inventory_pc_relative_code_literal !=
            inventory_pc_relative_code_literal) {
            target.inventory_pc_relative_code_literal =
                inventory_pc_relative_code_literal;
            changed = true;
        }
        const auto call_site_count = target.call_sites.size();
        const auto callee_count = target.callees.size();
        target.call_sites.insert(input.call_sites.begin(), input.call_sites.end());
        target.callees.insert(input.callees.begin(), input.callees.end());
        changed = changed || target.call_sites.size() != call_site_count ||
                  target.callees.size() != callee_count;
    };
    for (std::size_t index = 0u; index < destination.registers.size(); ++index)
        merge_value_payload(destination.registers[index], source.registers[index]);
    for (auto& [slot, value] : destination.stack_values)
        merge_value_payload(value, source.stack_values.at(slot));
    for (auto& [address, value] : destination.memory_values)
        merge_value_payload(value, source.memory_values.at(address));
    return changed;
}

struct FunctionEvaluation {
    FunctionValueSummary summary;
    bool local_fixpoint_budget_exhausted = false;
    std::vector<InterproceduralTargetResolution> resolutions;
    struct CallArguments {
        std::uint32_t call_site = 0u;
        std::uint32_t callee = 0u;
        AbstractState state;
    };
    std::vector<CallArguments> call_arguments;
    struct InventoryTransfer {
        std::uint32_t transfer_site = 0u;
        std::uint32_t target = 0u;
        AbstractState state;
        bool guarded = true;
        bool complete = false;
    };
    std::vector<InventoryTransfer> inventory_transfers;
};

struct IndirectCalleeCandidates {
    std::vector<std::uint32_t> targets;
    bool guarded = false;
    bool complete = true;
    bool requires_code_pointer = false;
    bool observes_abi_arguments = false;
};
using TailIngressMap =
    std::unordered_map<std::uint32_t, IndirectCalleeCandidates>;

struct CandidateInput {
    AbstractState state;
    std::set<std::uint32_t> expected_call_sites;
    std::map<std::uint32_t, AbstractState> observations;
    bool unknown_ingress = false;
};

void normalize(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void merge_normalized(std::vector<std::uint32_t>& destination,
                      std::vector<std::uint32_t> source) {
    normalize(source);
    if (source.empty()) return;
    if (destination.empty()) {
        destination = std::move(source);
        return;
    }
    const auto previous_size = destination.size();
    destination.reserve(previous_size + source.size());
    destination.insert(destination.end(),
                       std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
    std::inplace_merge(
        destination.begin(),
        destination.begin() + static_cast<std::ptrdiff_t>(previous_size),
        destination.end());
    destination.erase(
        std::unique(destination.begin(), destination.end()),
        destination.end());
}

void normalize_stack_coordinates(std::vector<std::int32_t>& coordinates) {
    std::sort(coordinates.begin(), coordinates.end());
    coordinates.erase(
        std::unique(coordinates.begin(), coordinates.end()),
        coordinates.end());
}

[[nodiscard]] bool insert_inventory_stack_coordinate(
    std::vector<std::int32_t>& coordinates,
    const std::int64_t coordinate) {
    if (coordinate < -maximum_stack_distance ||
        coordinate > maximum_stack_distance)
        return false;
    const auto value = static_cast<std::int32_t>(coordinate);
    const auto position =
        std::lower_bound(coordinates.begin(), coordinates.end(), value);
    if (position != coordinates.end() && *position == value)
        return true;
    if (coordinates.size() >= maximum_inventory_stack_coordinates)
        return false;
    coordinates.insert(position, value);
    return true;
}

[[nodiscard]] std::vector<std::int32_t>
inventory_stack_coordinates(const AbstractState& state,
                            const std::uint8_t register_index) {
    if (state.inventory_stack_offsets[register_index].has_value())
        return {*state.inventory_stack_offsets[register_index]};
    return state.inventory_stack_offset_candidates[register_index];
}

bool set_inventory_stack_coordinates(
    AbstractState& state,
    const std::uint8_t register_index,
    std::vector<std::int32_t> coordinates) {
    if (std::any_of(coordinates.begin(),
                    coordinates.end(),
                    [](const auto coordinate) {
                        return coordinate < -maximum_stack_distance ||
                               coordinate > maximum_stack_distance;
                    }))
        coordinates.clear();
    normalize_stack_coordinates(coordinates);
    if (coordinates.size() > maximum_inventory_stack_coordinates)
        coordinates.clear();
    const auto old_exact = state.inventory_stack_offsets[register_index];
    const auto old_candidates =
        state.inventory_stack_offset_candidates[register_index];
    if (coordinates.size() == 1u) {
        state.inventory_stack_offsets[register_index] =
            coordinates.front();
        state.inventory_stack_offset_candidates[register_index].clear();
    } else {
        state.inventory_stack_offsets[register_index].reset();
        state.inventory_stack_offset_candidates[register_index] =
            std::move(coordinates);
    }
    return old_exact != state.inventory_stack_offsets[register_index] ||
           old_candidates !=
               state.inventory_stack_offset_candidates[register_index];
}

void clear_inventory_stack_coordinates(
    AbstractState& state,
    const std::uint8_t register_index) {
    state.inventory_stack_offsets[register_index].reset();
    state.inventory_stack_offset_candidates[register_index].clear();
}

[[nodiscard]] bool has_current_inventory_stack_lineage(
    const AbstractState& state,
    const std::uint8_t register_index) {
    return state[register_index].inventory_stack_derived ||
           state.stack_offsets[register_index].has_value() ||
           !inventory_stack_coordinates(
                state, register_index)
                .empty();
}

void synchronize_inventory_provenance(AbstractValue& value) {
    const auto normalize_domain = [&](std::vector<std::uint32_t>& provenance,
                                      bool& truncated) {
        normalize(provenance);
        if (value.known) {
            std::erase_if(provenance, [&](const auto candidate) {
                return !std::binary_search(
                    value.values.begin(), value.values.end(), candidate);
            });
        }
        if (provenance.size() > maximum_guarded_code_inventory) {
            provenance.resize(maximum_guarded_code_inventory);
            truncated = true;
        }
    };
    normalize_domain(value.inventory_code_pointer_values,
                     value.inventory_code_pointer_values_truncated);
    normalize_domain(value.inventory_pc_relative_code_literal_values,
                     value.inventory_pc_relative_code_literal_values_truncated);
    value.inventory_code_pointer = !value.inventory_code_pointer_values.empty();
    value.inventory_pc_relative_code_literal =
        !value.inventory_pc_relative_code_literal_values.empty();
}

[[nodiscard]] bool inventory_candidate_values_truncated(
    const AbstractValue& value) {
    if (value.inventory_code_pointer_values_truncated ||
        value.inventory_pc_relative_code_literal_values_truncated)
        return true;
    return std::any_of(
        value.inventory_saved_stack_epoch.slots.begin(),
        value.inventory_saved_stack_epoch.slots.end(),
        [](const auto& slot) {
            return slot.inventory_code_pointer_values_truncated ||
                   slot
                       .inventory_pc_relative_code_literal_values_truncated;
        });
}

[[nodiscard]] bool inventory_candidate_values_truncated(
    const AbstractState& state) {
    if (state.inventory_stack_callback_loss_identity_truncated)
        return true;
    if (std::any_of(state.begin(), state.end(), [](const auto& value) {
            return inventory_candidate_values_truncated(value);
        }))
        return true;
    for (const auto& [slot, value] : state.stack_values) {
        static_cast<void>(slot);
        if (inventory_candidate_values_truncated(value)) return true;
    }
    for (const auto& [address, value] : state.memory_values) {
        static_cast<void>(address);
        if (inventory_candidate_values_truncated(value)) return true;
    }
    return false;
}

[[nodiscard]] bool has_inventory_candidate_values(const AbstractValue& value) {
    return !value.inventory_code_pointer_values.empty() ||
           !value.inventory_pc_relative_code_literal_values.empty();
}

[[nodiscard]] bool has_finite_inventory_candidate_values(
    const AbstractValue& value) {
    return has_inventory_candidate_values(value) &&
           !inventory_candidate_values_truncated(value);
}

[[nodiscard]] bool has_forwarded_inventory_payload(
    const AbstractState& state) {
    if (state.inventory_unresolved_saved_stack_alias_sources != 0u ||
        state.inventory_current_stack_epoch_alias_watcher ||
        state.inventory_detached_stack_epoch_alias_watcher ||
        state.inventory_unresolved_stack_callback_loss ||
        state.inventory_stack_callback_loss_identity_truncated)
        return true;
    const auto relevant = [](const AbstractValue& value) {
        return has_inventory_candidate_values(value) ||
               inventory_candidate_values_truncated(value) ||
               value.contextual_candidate_dependency ||
               value.inventory_stack_callback_loss_unresolved ||
               value.inventory_saved_stack_epoch.present ||
               value.inventory_saved_stack_epoch.unresolved;
    };
    if (std::any_of(state.begin(), state.end(), relevant))
        return true;
    if (std::any_of(
            state.stack_values.begin(),
            state.stack_values.end(),
            [&](const auto& slot) { return relevant(slot.second); }))
        return true;
    return std::any_of(
        state.memory_values.begin(),
        state.memory_values.end(),
        [&](const auto& value) { return relevant(value.second); });
}

[[nodiscard]] bool has_stack_callback_memory_payload(
    const AbstractState& state) {
    return std::any_of(
        state.memory_values.begin(),
        state.memory_values.end(),
        [](const auto& stored) {
            return has_saved_stack_epoch(stored.second) ||
                   carries_stack_callback_payload(stored.second);
        });
}

[[nodiscard]] bool has_inventory_code_pointer_value(const AbstractValue& value,
                                                     const std::uint32_t candidate) {
    return std::binary_search(value.inventory_code_pointer_values.begin(),
                              value.inventory_code_pointer_values.end(),
                              candidate);
}

[[nodiscard]] bool has_inventory_pc_relative_code_literal_value(
    const AbstractValue& value, const std::uint32_t candidate) {
    return std::binary_search(value.inventory_pc_relative_code_literal_values.begin(),
                              value.inventory_pc_relative_code_literal_values.end(),
                              candidate);
}

void mark_inventory_code_pointer_values(AbstractValue& value,
                                        const std::span<const std::uint32_t> candidates) {
    value.inventory_code_pointer_values.insert(value.inventory_code_pointer_values.end(),
                                               candidates.begin(),
                                               candidates.end());
    synchronize_inventory_provenance(value);
}

bool merge_inventory_candidate_values(
    std::vector<std::uint32_t>& destination,
    bool& destination_truncated,
    const std::span<const std::uint32_t> source,
    const bool source_truncated) {
    const auto previous = destination;
    const auto previous_truncated = destination_truncated;
    destination.insert(destination.end(), source.begin(), source.end());
    normalize(destination);
    if (destination.size() > maximum_guarded_code_inventory) {
        destination.resize(maximum_guarded_code_inventory);
        destination_truncated = true;
    }
    destination_truncated = destination_truncated || source_truncated;
    return destination != previous ||
           destination_truncated != previous_truncated;
}

[[nodiscard]] std::optional<IndirectCalleeCandidates>
find_tail_ingress(const TailIngressMap& baseline,
                  const TailIngressMap* const local,
                  const std::uint32_t transfer_site) {
    const auto baseline_ingress = baseline.find(transfer_site);
    if (local != nullptr) {
        const auto local_ingress = local->find(transfer_site);
        if (local_ingress != local->end()) return local_ingress->second;
    }
    if (baseline_ingress == baseline.end()) return std::nullopt;
    return baseline_ingress->second;
}

} // namespace

std::vector<std::uint32_t>
detail::guarded_code_inventory_priority_order(
    const std::span<const GuardedCodeInventoryPriorityTarget> candidates,
    const std::size_t returned_table_reserve) {
    std::array<std::vector<std::uint32_t>, 4u> by_kind;
    for (const auto& candidate : candidates) {
        by_kind[static_cast<std::size_t>(candidate.kind)].push_back(
            candidate.target_address);
    }
    for (auto& targets : by_kind) normalize(targets);
    const auto& complete_stored =
        by_kind[static_cast<std::size_t>(
            GuardedCodeInventoryPriorityKind::CompleteStored)];
    const auto& incomplete_stored =
        by_kind[static_cast<std::size_t>(
            GuardedCodeInventoryPriorityKind::IncompleteStored)];
    const auto& complete_returned =
        by_kind[static_cast<std::size_t>(
            GuardedCodeInventoryPriorityKind::CompleteReturnedTable)];
    const auto& truncated_returned =
        by_kind[static_cast<std::size_t>(
            GuardedCodeInventoryPriorityKind::TruncatedReturnedTable)];
    std::vector<std::uint32_t> ordered;
    ordered.reserve(candidates.size());
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(candidates.size());
    const auto append = [&](const std::span<const std::uint32_t> targets,
                            const std::size_t limit =
                                std::numeric_limits<std::size_t>::max()) {
        std::size_t inserted = 0u;
        for (const auto target : targets) {
            if (inserted >= limit) break;
            if (!seen.insert(target).second) continue;
            ordered.push_back(target);
            ++inserted;
        }
    };
    append(complete_returned, returned_table_reserve);
    append(complete_stored);
    append(complete_returned);
    append(incomplete_stored);
    append(truncated_returned);
    return ordered;
}

namespace {

class GuardedCodeInventoryCollector {
  public:
    explicit GuardedCodeInventoryCollector(
        const bool defer_stored_admission = false,
        detail::GuardedNativeEntryShapeCache* const shape_cache = nullptr)
        : shape_cache_(shape_cache),
          defer_stored_admission_(defer_stored_admission) {}

    // A persistent evaluation artifact forwards its first logical execution
    // into the caller's collector while retaining the exact, canonical
    // sequence of collect operations for later cache hits. This avoids
    // changing the bounded top-K collector algebra on a miss and makes a hit
    // replay precisely the same state transitions.
    void begin_exact_replay_capture(
        GuardedCodeInventoryCollector& destination) {
        if (replay_destination_ != nullptr ||
            captures_exact_replay_)
            throw std::logic_error(
                "Guarded-Code-Inventar-Replay wurde doppelt gestartet.");
        replay_destination_ = &destination;
        captures_exact_replay_ = true;
    }

    void finish_exact_replay_capture() noexcept {
        replay_destination_ = nullptr;
    }

    [[nodiscard]] bool exact_replay_available() const noexcept {
        return !captures_exact_replay_ ||
               exact_replay_available_;
    }

    void mark_stored_candidates_incomplete() {
        complete_stored_targets_.clear();
        incomplete_stored_targets_.clear();
        for (auto& [target, candidate] : stored_candidates_) {
            candidate.complete = false;
            incomplete_stored_targets_.insert(target);
        }
        for (auto& candidate : replay_stored_candidates_)
            candidate.complete = false;
    }

    const std::optional<JumpTableAnalysis>& stored_snapshot_table(
        const katana::io::ExecutableImage& image,
        const std::uint32_t evidence_address,
        const std::uint32_t table_address) {
        const auto cached = stored_snapshot_tables_.find(table_address);
        if (cached != stored_snapshot_tables_.end()) return cached->second;
        constexpr detail::SnapshotPointerCandidateScanPolicy scan_policy{
            .minimum_entries = 4u,
            .maximum_scanned_slots = 64u,
            .maximum_skipped_slots = 8u,
            .maximum_consecutive_skipped_slots = 3u,
            .treat_null_as_reserved = true,
            .reject_truncated_scan = false,
        };
        auto result = detail::analyze_snapshot_pointer_candidates(
            image,
            evidence_address,
            table_address,
            JumpTableDispatchKind::Call,
            scan_policy);
        return stored_snapshot_tables_
            .emplace(table_address, std::move(result))
            .first->second;
    }

    void collect(std::vector<StoredCodeAddressCandidate> candidates) {
        if (candidates.empty()) return;
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& left, const auto& right) {
                      if (left.target_address != right.target_address)
                          return left.target_address < right.target_address;
                      return left.store_instruction_addresses <
                             right.store_instruction_addresses;
        });
        if (replay_destination_ != nullptr) {
            record_exact_replay(candidates);
            replay_destination_->collect(std::move(candidates));
            return;
        }
        for (auto& candidate : candidates) {
            const auto complete_evidence = candidate.complete;
            collect_stored_candidate(std::move(candidate),
                                     complete_evidence);
        }
    }

    void collect(std::vector<ReturnedCodeAddressTableCandidate> candidates) {
        if (candidates.empty()) return;
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& left, const auto& right) {
                      if (left.table_address != right.table_address)
                          return left.table_address < right.table_address;
                      return left.load_instruction_addresses <
                             right.load_instruction_addresses;
                  });
        if (replay_destination_ != nullptr) {
            record_exact_replay(candidates);
            replay_destination_->collect(std::move(candidates));
            return;
        }
        for (auto& candidate : candidates)
            collect_returned_candidate(std::move(candidate));
    }

    void replay_into(GuardedCodeInventoryCollector& destination) && {
        if (!defer_stored_admission_ || destination.defer_stored_admission_)
            throw std::logic_error(
                "Guarded-Code-Inventar besitzt einen ungueltigen Replay-Vertrag.");
        for (auto& [target, candidate] : stored_candidates_) {
            destination.collect_stored_candidate(
                std::move(candidate),
                complete_stored_targets_.contains(target));
        }
        for (auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            destination.collect_returned_candidate(std::move(candidate));
        }
        destination.candidate_inventory_truncated_ =
            destination.candidate_inventory_truncated_ ||
            candidate_inventory_truncated_;
        destination.candidate_budget_exhausted_ =
            destination.candidate_budget_exhausted_ || candidate_budget_exhausted_;
        destination.raw_stored_candidates_truncated_ =
            destination.raw_stored_candidates_truncated_ ||
            raw_stored_candidates_truncated_;
        destination.table_scan_truncated_ =
            destination.table_scan_truncated_ || table_scan_truncated_;
    }

    void replay_deferred_copy_into(
        GuardedCodeInventoryCollector& destination) const {
        if (!defer_stored_admission_ ||
            !destination.defer_stored_admission_)
            throw std::logic_error(
                "Guarded-Code-Inventar besitzt einen ungueltigen "
                "Deferred-Replay-Vertrag.");
        if (captures_exact_replay_) {
            if (!exact_replay_available_)
                throw std::logic_error(
                    "Exaktes Guarded-Code-Inventar-Replay ist nicht "
                    "verfuegbar.");
            replay_exact_copy_into(destination);
            return;
        }
        for (const auto& [target, candidate] : stored_candidates_) {
            destination.collect_stored_candidate(
                candidate,
                complete_stored_targets_.contains(target));
        }
        for (const auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            destination.collect_returned_candidate(candidate);
        }
        destination.candidate_inventory_truncated_ =
            destination.candidate_inventory_truncated_ ||
            candidate_inventory_truncated_;
        destination.candidate_budget_exhausted_ =
            destination.candidate_budget_exhausted_ ||
            candidate_budget_exhausted_;
        destination.raw_stored_candidates_truncated_ =
            destination.raw_stored_candidates_truncated_ ||
            raw_stored_candidates_truncated_;
        destination.table_scan_truncated_ =
            destination.table_scan_truncated_ ||
            table_scan_truncated_;
    }

    void replay_deferred_into(
        GuardedCodeInventoryCollector& destination) && {
        if (!defer_stored_admission_ ||
            !destination.defer_stored_admission_)
            throw std::logic_error(
                "Guarded-Code-Inventar besitzt einen ungueltigen "
                "Deferred-Move-Replay-Vertrag.");
        if (captures_exact_replay_) {
            if (!exact_replay_available_)
                throw std::logic_error(
                    "Exaktes Guarded-Code-Inventar-Replay ist nicht "
                    "verfuegbar.");
            replay_exact_move_into(destination);
            return;
        }
        for (auto& [target, candidate] : stored_candidates_) {
            destination.collect_stored_candidate(
                std::move(candidate),
                complete_stored_targets_.contains(target));
        }
        for (auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            destination.collect_returned_candidate(
                std::move(candidate));
        }
        destination.candidate_inventory_truncated_ =
            destination.candidate_inventory_truncated_ ||
            candidate_inventory_truncated_;
        destination.candidate_budget_exhausted_ =
            destination.candidate_budget_exhausted_ ||
            candidate_budget_exhausted_;
        destination.raw_stored_candidates_truncated_ =
            destination.raw_stored_candidates_truncated_ ||
            raw_stored_candidates_truncated_;
        destination.table_scan_truncated_ =
            destination.table_scan_truncated_ ||
            table_scan_truncated_;
    }

    // Snapshot-table scans are evaluation-local accelerators. They are never
    // replayed and would otherwise make a persistent evaluation artifact keep
    // a second copy of unrelated table-analysis state alive.
    void discard_transient_caches() {
        stored_snapshot_tables_.clear();
        stored_snapshot_tables_.rehash(0u);
    }

    // Heap retained by this collector. The containing cache artifact accounts
    // for the inline collector object exactly once; only allocations owned by
    // its containers belong here.
    [[nodiscard]] std::size_t retained_heap_bytes() const noexcept {
        std::size_t bytes = 0u;
        for (const auto& [target, candidate] : stored_candidates_) {
            static_cast<void>(target);
            bytes += sizeof(std::pair<const std::uint32_t,
                                      StoredCodeAddressCandidate>) +
                     3u * sizeof(void*);
            bytes += candidate.store_instruction_addresses.capacity() *
                     sizeof(std::uint32_t);
            bytes += candidate.evidence_call_sites.capacity() *
                     sizeof(std::uint32_t);
            bytes += candidate.evidence_callees.capacity() *
                     sizeof(std::uint32_t);
        }
        for (const auto& [table, candidate] : returned_tables_) {
            static_cast<void>(table);
            bytes += sizeof(std::pair<
                         const std::uint32_t,
                         ReturnedCodeAddressTableCandidate>) +
                     3u * sizeof(void*);
            bytes += candidate.target_addresses.capacity() *
                     sizeof(std::uint32_t);
            bytes += candidate.load_instruction_addresses.capacity() *
                     sizeof(std::uint32_t);
            bytes += candidate.evidence_call_sites.capacity() *
                     sizeof(std::uint32_t);
            bytes += candidate.evidence_callees.capacity() *
                     sizeof(std::uint32_t);
        }
        bytes += (complete_stored_targets_.size() +
                  incomplete_stored_targets_.size() +
                  admitted_targets_.size()) *
                 (sizeof(std::uint32_t) + 3u * sizeof(void*));
        bytes += replay_events_.capacity() *
                 sizeof(ExactReplayEvent);
        bytes += replay_stored_candidates_.capacity() *
                 sizeof(StoredCodeAddressCandidate);
        for (const auto& candidate :
             replay_stored_candidates_) {
            bytes +=
                (candidate.store_instruction_addresses.capacity() +
                 candidate.evidence_call_sites.capacity() +
                 candidate.evidence_callees.capacity()) *
                sizeof(std::uint32_t);
        }
        bytes += replay_returned_candidates_.capacity() *
                 sizeof(ReturnedCodeAddressTableCandidate);
        for (const auto& candidate :
             replay_returned_candidates_) {
            bytes +=
                (candidate.target_addresses.capacity() +
                 candidate.load_instruction_addresses.capacity() +
                 candidate.evidence_call_sites.capacity() +
                 candidate.evidence_callees.capacity()) *
                sizeof(std::uint32_t);
        }
        return bytes;
    }

    GuardedCodeInventory finish() {
        if (defer_stored_admission_)
            throw std::logic_error(
                "Deferred Guarded-Code-Inventar muss vor Finish zusammengefuehrt werden.");
        GuardedCodeInventory inventory;
        for (auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            normalize(candidate.target_addresses);
        }

        // Keep both evidence families represented.  A small concrete-table
        // reserve prevents broad forwarded stores from evicting the Sonic-like
        // method-table case, while admitting complete stores before the
        // remaining table population guarantees that a table flood cannot
        // erase every direct callback proof.
        std::vector<detail::GuardedCodeInventoryPriorityTarget>
            priority_candidates;
        priority_candidates.reserve(stored_candidates_.size());
        for (const auto& [target, candidate] : stored_candidates_) {
            static_cast<void>(candidate);
            priority_candidates.push_back({
                target,
                complete_stored_targets_.contains(target)
                    ? detail::GuardedCodeInventoryPriorityKind::
                          CompleteStored
                    : detail::GuardedCodeInventoryPriorityKind::
                          IncompleteStored});
        }
        for (const auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            for (const auto target : candidate.target_addresses)
                priority_candidates.push_back({
                    target,
                    candidate.scan_truncated
                        ? detail::GuardedCodeInventoryPriorityKind::
                              TruncatedReturnedTable
                        : detail::GuardedCodeInventoryPriorityKind::
                              CompleteReturnedTable});
        }
        const auto priority_order =
            detail::guarded_code_inventory_priority_order(
                priority_candidates,
                reserved_returned_table_targets);
        for (const auto target : priority_order)
            static_cast<void>(admit_candidate(target));

        inventory.stored_code_addresses.reserve(stored_candidates_.size());
        for (auto& [target, candidate] : stored_candidates_) {
            if (!admitted_targets_.contains(target)) continue;
            normalize(candidate.store_instruction_addresses);
            normalize(candidate.evidence_call_sites);
            normalize(candidate.evidence_callees);
            inventory.stored_code_addresses.push_back(std::move(candidate));
        }
        inventory.returned_code_address_tables.reserve(returned_tables_.size());
        for (auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            std::erase_if(candidate.target_addresses,
                          [&](const auto target) {
                              return !admitted_targets_.contains(target);
                          });
            if (candidate.target_addresses.empty()) continue;
            normalize(candidate.load_instruction_addresses);
            normalize(candidate.evidence_call_sites);
            normalize(candidate.evidence_callees);
            inventory.returned_code_address_tables.push_back(
                std::move(candidate));
        }
        inventory.raw_stored_candidate_budget =
            maximum_raw_stored_code_candidates;
        inventory.raw_stored_candidate_count = stored_candidates_.size();
        inventory.raw_stored_candidates_truncated =
            raw_stored_candidates_truncated_;
        inventory.candidate_budget = maximum_guarded_code_inventory;
        inventory.candidate_count = admitted_targets_.size();
        inventory.candidate_budget_exhausted = candidate_budget_exhausted_;
        inventory.candidate_inventory_truncated =
            candidate_inventory_truncated_;
        if (shape_cache_ != nullptr) {
            const auto& shape = shape_cache_->statistics();
            inventory.shape_validation_work = shape.work;
            inventory.shape_validation_work_budget = shape.work_budget;
            inventory.shape_budget_exceeded_candidates =
                shape.shape_budget_exceeded;
            inventory.candidate_inventory_truncated =
                inventory.candidate_inventory_truncated ||
                shape.shape_budget_exceeded != 0u;
        }
        inventory.table_scan_truncated = table_scan_truncated_;
        return inventory;
    }

  private:
    struct ExactReplayEvent {
        bool returned_table = false;
        std::size_t begin = 0u;
        std::size_t count = 0u;
    };

    [[nodiscard]] static std::size_t replay_bytes(
        const StoredCodeAddressCandidate& candidate) noexcept {
        return sizeof(candidate) +
               (candidate.store_instruction_addresses.size() +
                candidate.evidence_call_sites.size() +
                candidate.evidence_callees.size()) *
                   sizeof(std::uint32_t);
    }

    [[nodiscard]] static std::size_t replay_bytes(
        const ReturnedCodeAddressTableCandidate& candidate) noexcept {
        return sizeof(candidate) +
               (candidate.target_addresses.size() +
                candidate.load_instruction_addresses.size() +
                candidate.evidence_call_sites.size() +
                candidate.evidence_callees.size()) *
                   sizeof(std::uint32_t);
    }

    template <typename Candidate>
    void record_exact_replay(
        const std::vector<Candidate>& candidates) {
        if (!exact_replay_available_) return;
        std::size_t additional =
            sizeof(ExactReplayEvent);
        for (const auto& candidate : candidates) {
            const auto candidate_bytes =
                replay_bytes(candidate);
            if (additional >
                maximum_evaluation_inventory_replay_bytes -
                    std::min(
                        maximum_evaluation_inventory_replay_bytes,
                        candidate_bytes)) {
                abandon_exact_replay();
                return;
            }
            additional += candidate_bytes;
        }
        if (exact_replay_bytes_ >
            maximum_evaluation_inventory_replay_bytes -
                std::min(
                    maximum_evaluation_inventory_replay_bytes,
                    additional)) {
            abandon_exact_replay();
            return;
        }
        if constexpr (
            std::is_same_v<
                Candidate,
                StoredCodeAddressCandidate>) {
            const auto begin =
                replay_stored_candidates_.size();
            replay_stored_candidates_.insert(
                replay_stored_candidates_.end(),
                candidates.begin(),
                candidates.end());
            replay_events_.push_back(
                {false, begin, candidates.size()});
        } else {
            const auto begin =
                replay_returned_candidates_.size();
            replay_returned_candidates_.insert(
                replay_returned_candidates_.end(),
                candidates.begin(),
                candidates.end());
            replay_events_.push_back(
                {true, begin, candidates.size()});
        }
        exact_replay_bytes_ += additional;
    }

    void abandon_exact_replay() noexcept {
        exact_replay_available_ = false;
        replay_events_ = {};
        replay_stored_candidates_ = {};
        replay_returned_candidates_ = {};
        exact_replay_bytes_ = 0u;
    }

    void replay_exact_copy_into(
        GuardedCodeInventoryCollector& destination) const {
        for (const auto& event : replay_events_) {
            if (event.returned_table) {
                destination.collect(
                    std::vector<ReturnedCodeAddressTableCandidate>{
                        replay_returned_candidates_.begin() +
                            static_cast<std::ptrdiff_t>(
                                event.begin),
                        replay_returned_candidates_.begin() +
                            static_cast<std::ptrdiff_t>(
                                event.begin + event.count)});
            } else {
                destination.collect(
                    std::vector<StoredCodeAddressCandidate>{
                        replay_stored_candidates_.begin() +
                            static_cast<std::ptrdiff_t>(
                                event.begin),
                        replay_stored_candidates_.begin() +
                            static_cast<std::ptrdiff_t>(
                                event.begin + event.count)});
            }
        }
    }

    void replay_exact_move_into(
        GuardedCodeInventoryCollector& destination) {
        for (const auto& event : replay_events_) {
            if (event.returned_table) {
                std::vector<ReturnedCodeAddressTableCandidate>
                    candidates;
                candidates.reserve(event.count);
                for (std::size_t index = 0u;
                     index < event.count;
                     ++index) {
                    candidates.push_back(std::move(
                        replay_returned_candidates_[
                            event.begin + index]));
                }
                destination.collect(std::move(candidates));
            } else {
                std::vector<StoredCodeAddressCandidate>
                    candidates;
                candidates.reserve(event.count);
                for (std::size_t index = 0u;
                     index < event.count;
                     ++index) {
                    candidates.push_back(std::move(
                        replay_stored_candidates_[
                            event.begin + index]));
                }
                destination.collect(std::move(candidates));
            }
        }
    }

    bool admissible_shape(const std::uint32_t target) {
        if (shape_cache_ == nullptr) return true;
        const auto status = shape_cache_->classify(target);
        if (status ==
            detail::GuardedNativeEntryShapeStatus::ShapeBudgetExceeded)
            candidate_inventory_truncated_ = true;
        return status == detail::GuardedNativeEntryShapeStatus::Valid;
    }

    void collect_stored_candidate(StoredCodeAddressCandidate candidate,
                                  const bool complete_evidence) {
        candidate.guarded = true;
        normalize(candidate.store_instruction_addresses);
        normalize(candidate.evidence_call_sites);
        normalize(candidate.evidence_callees);
        const auto target = candidate.target_address;
        const auto existing = stored_candidates_.find(target);
        if (existing == stored_candidates_.end() &&
            stored_candidates_.size() >=
                maximum_raw_stored_code_candidates) {
            raw_stored_candidates_truncated_ = true;
            candidate_inventory_truncated_ = true;
            auto worst_target = std::optional<std::uint32_t>{};
            if (complete_evidence) {
                if (!incomplete_stored_targets_.empty()) {
                    worst_target =
                        *incomplete_stored_targets_.rbegin();
                } else {
                    worst_target =
                        stored_candidates_.rbegin()->first;
                    if (target >= *worst_target) return;
                }
            } else {
                if (incomplete_stored_targets_.empty() ||
                    target >=
                        *incomplete_stored_targets_.rbegin())
                    return;
                worst_target =
                    *incomplete_stored_targets_.rbegin();
            }
            complete_stored_targets_.erase(*worst_target);
            incomplete_stored_targets_.erase(*worst_target);
            stored_candidates_.erase(*worst_target);
        }
        const auto [stored, inserted] =
            stored_candidates_.try_emplace(target, std::move(candidate));
        if (complete_evidence) {
            complete_stored_targets_.insert(target);
            incomplete_stored_targets_.erase(target);
        } else if (inserted) {
            incomplete_stored_targets_.insert(target);
        }
        if (inserted) return;
        auto& destination = stored->second;
        destination.complete = destination.complete && candidate.complete;
        destination.guarded = true;
        merge_normalized(destination.store_instruction_addresses,
                         std::move(candidate.store_instruction_addresses));
        merge_normalized(destination.evidence_call_sites,
                         std::move(candidate.evidence_call_sites));
        merge_normalized(destination.evidence_callees,
                         std::move(candidate.evidence_callees));
    }

    void collect_returned_candidate(ReturnedCodeAddressTableCandidate candidate) {
        table_scan_truncated_ = table_scan_truncated_ || candidate.scan_truncated;
        normalize(candidate.target_addresses);
        normalize(candidate.load_instruction_addresses);
        normalize(candidate.evidence_call_sites);
        normalize(candidate.evidence_callees);
        if (candidate.target_addresses.empty()) return;
        const auto [stored, inserted] =
            returned_tables_.try_emplace(candidate.table_address, std::move(candidate));
        if (inserted) return;
        auto& destination = stored->second;
        merge_normalized(destination.target_addresses,
                         std::move(candidate.target_addresses));
        merge_normalized(destination.load_instruction_addresses,
                         std::move(candidate.load_instruction_addresses));
        merge_normalized(destination.evidence_call_sites,
                         std::move(candidate.evidence_call_sites));
        merge_normalized(destination.evidence_callees,
                         std::move(candidate.evidence_callees));
        destination.scan_truncated =
            destination.scan_truncated || candidate.scan_truncated;
    }

    bool admit_candidate(const std::uint32_t target) {
        if (admitted_targets_.contains(target)) return true;
        if (admitted_targets_.size() >= maximum_guarded_code_inventory) {
            candidate_budget_exhausted_ = true;
            candidate_inventory_truncated_ = true;
            return false;
        }
        // Do not spend structural-walk work on a target that could not be
        // admitted anyway.  This keeps resource exhaustion deterministic and
        // independent of broad low-priority candidate tails.
        if (!admissible_shape(target)) return false;
        admitted_targets_.insert(target);
        return true;
    }

    std::set<std::uint32_t> admitted_targets_;
    std::map<std::uint32_t, StoredCodeAddressCandidate> stored_candidates_;
    std::set<std::uint32_t> complete_stored_targets_;
    std::set<std::uint32_t> incomplete_stored_targets_;
    std::map<std::uint32_t, ReturnedCodeAddressTableCandidate> returned_tables_;
    std::unordered_map<std::uint32_t, std::optional<JumpTableAnalysis>>
        stored_snapshot_tables_;
    GuardedCodeInventoryCollector* replay_destination_ = nullptr;
    std::vector<ExactReplayEvent> replay_events_;
    std::vector<StoredCodeAddressCandidate>
        replay_stored_candidates_;
    std::vector<ReturnedCodeAddressTableCandidate>
        replay_returned_candidates_;
    detail::GuardedNativeEntryShapeCache* shape_cache_ = nullptr;
    bool defer_stored_admission_ = false;
    bool captures_exact_replay_ = false;
    bool exact_replay_available_ = true;
    std::size_t exact_replay_bytes_ = 0u;
    bool raw_stored_candidates_truncated_ = false;
    bool candidate_budget_exhausted_ = false;
    bool candidate_inventory_truncated_ = false;
    bool table_scan_truncated_ = false;
};

void make_unknown(AbstractValue& value) {
    value.known = false;
    value.guarded = false;
    value.complete = false;
    value.inventory_stack_derived = false;
    value.inventory_code_pointer = false;
    value.inventory_pc_relative_code_literal = false;
    value.inventory_code_pointer_values.clear();
    value.inventory_pc_relative_code_literal_values.clear();
    value.inventory_code_pointer_values_truncated = false;
    value.inventory_pc_relative_code_literal_values_truncated = false;
    value.contextual_candidate_dependency = false;
    value.inventory_stack_callback_loss_unresolved = false;
    value.inventory_saved_stack_epoch = {};
    value.values.clear();
    value.call_sites.clear();
    value.callees.clear();
}

void make_unknown_preserving_provenance(AbstractValue& value) {
    const auto inventory_stack_derived = value.inventory_stack_derived;
    const auto contextual_candidate_dependency = value.contextual_candidate_dependency;
    const auto inventory_stack_callback_loss_unresolved =
        value.inventory_stack_callback_loss_unresolved;
    auto inventory_saved_stack_epoch =
        std::move(value.inventory_saved_stack_epoch);
    auto inventory_code_pointer_values =
        std::move(value.inventory_code_pointer_values);
    auto inventory_pc_relative_code_literal_values =
        std::move(value.inventory_pc_relative_code_literal_values);
    const auto inventory_code_pointer_values_truncated =
        value.inventory_code_pointer_values_truncated;
    const auto inventory_pc_relative_code_literal_values_truncated =
        value.inventory_pc_relative_code_literal_values_truncated;
    auto call_sites = std::move(value.call_sites);
    auto callees = std::move(value.callees);
    make_unknown(value);
    value.inventory_stack_derived = inventory_stack_derived;
    value.inventory_code_pointer_values =
        std::move(inventory_code_pointer_values);
    value.inventory_pc_relative_code_literal_values =
        std::move(inventory_pc_relative_code_literal_values);
    value.inventory_code_pointer_values_truncated =
        inventory_code_pointer_values_truncated;
    value.inventory_pc_relative_code_literal_values_truncated =
        inventory_pc_relative_code_literal_values_truncated;
    value.inventory_code_pointer =
        !value.inventory_code_pointer_values.empty();
    value.inventory_pc_relative_code_literal =
        !value.inventory_pc_relative_code_literal_values.empty();
    value.contextual_candidate_dependency = contextual_candidate_dependency;
    value.inventory_stack_callback_loss_unresolved =
        inventory_stack_callback_loss_unresolved;
    value.inventory_saved_stack_epoch =
        std::move(inventory_saved_stack_epoch);
    value.call_sites = std::move(call_sites);
    value.callees = std::move(callees);
}

void set_values(AbstractValue& value,
                std::vector<std::uint32_t> constants) {
    make_unknown(value);
    value.known = true;
    value.guarded = false;
    value.complete = true;
    value.values = std::move(constants);
    normalize(value.values);
}

void set_value(AbstractValue& value, const std::uint32_t constant) {
    set_values(value, {constant});
}

bool merge_value(AbstractValue& destination, const AbstractValue& source) {
    const bool inventory_stack_derived =
        destination.inventory_stack_derived && source.inventory_stack_derived;
    const bool contextual_candidate_dependency =
        destination.contextual_candidate_dependency || source.contextual_candidate_dependency;
    const bool inventory_stack_callback_loss_unresolved =
        destination.inventory_stack_callback_loss_unresolved ||
        source.inventory_stack_callback_loss_unresolved;
    bool inventory_changed =
        destination.inventory_stack_callback_loss_unresolved !=
        inventory_stack_callback_loss_unresolved;
    destination.inventory_stack_callback_loss_unresolved =
        inventory_stack_callback_loss_unresolved;
    inventory_changed =
        merge_inventory_saved_stack_epoch(
            destination.inventory_saved_stack_epoch,
            source.inventory_saved_stack_epoch,
            true) ||
        inventory_changed;
    inventory_changed = merge_inventory_candidate_values(
        destination.inventory_code_pointer_values,
        destination.inventory_code_pointer_values_truncated,
        source.inventory_code_pointer_values,
        source.inventory_code_pointer_values_truncated) ||
        inventory_changed;
    inventory_changed =
        merge_inventory_candidate_values(
            destination.inventory_pc_relative_code_literal_values,
            destination.inventory_pc_relative_code_literal_values_truncated,
            source.inventory_pc_relative_code_literal_values,
            source.inventory_pc_relative_code_literal_values_truncated) ||
        inventory_changed;
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        const bool changed =
            inventory_changed || destination.known || destination.guarded ||
            destination.complete ||
            destination.inventory_stack_derived != inventory_stack_derived ||
            destination.inventory_code_pointer !=
                !destination.inventory_code_pointer_values.empty() ||
            destination.inventory_pc_relative_code_literal !=
                !destination.inventory_pc_relative_code_literal_values.empty() ||
            destination.contextual_candidate_dependency !=
                contextual_candidate_dependency ||
            !destination.values.empty() ||
            call_sites != destination.call_sites ||
            callees != destination.callees;
        destination.known = false;
        destination.guarded = false;
        destination.complete = false;
        destination.inventory_stack_derived = inventory_stack_derived;
        destination.inventory_code_pointer =
            !destination.inventory_code_pointer_values.empty();
        destination.inventory_pc_relative_code_literal =
            !destination.inventory_pc_relative_code_literal_values.empty();
        destination.contextual_candidate_dependency = contextual_candidate_dependency;
        destination.values.clear();
        destination.call_sites = std::move(call_sites);
        destination.callees = std::move(callees);
        return changed;
    }
    bool changed = inventory_changed;
    for (const auto call_site : source.call_sites)
        changed = destination.call_sites.insert(call_site).second || changed;
    for (const auto callee : source.callees)
        changed = destination.callees.insert(callee).second || changed;
    const auto guarded = destination.guarded || source.guarded;
    const auto complete = destination.complete && source.complete;
    changed = guarded != destination.guarded || complete != destination.complete || changed;
    destination.guarded = guarded;
    destination.complete = complete;
    if (destination.inventory_stack_derived != inventory_stack_derived) {
        destination.inventory_stack_derived = inventory_stack_derived;
        changed = true;
    }
    if (destination.contextual_candidate_dependency != contextual_candidate_dependency) {
        destination.contextual_candidate_dependency = contextual_candidate_dependency;
        changed = true;
    }
    auto values = destination.values;
    values.insert(values.end(), source.values.begin(), source.values.end());
    normalize(values);
    if (values.size() > maximum_summary_values) {
        make_unknown_preserving_provenance(destination);
        return true;
    }
    if (values != destination.values) {
        destination.values = std::move(values);
        changed = true;
    }
    const auto inventory_code_pointer = destination.inventory_code_pointer;
    const auto inventory_pc_relative_code_literal =
        destination.inventory_pc_relative_code_literal;
    synchronize_inventory_provenance(destination);
    changed = changed ||
              destination.inventory_code_pointer != inventory_code_pointer ||
              destination.inventory_pc_relative_code_literal !=
                  inventory_pc_relative_code_literal;
    return changed;
}

bool merge_state(AbstractState& destination,
                 const AbstractState& source,
                 const bool may_merge_stack_values = false) {
    bool changed = false;
    const bool destination_has_active_stack_payload =
        has_active_inventory_stack_payload(destination);
    const bool source_has_active_stack_payload =
        has_active_inventory_stack_payload(source);
    std::array<bool, 16u> destination_stack_lineage{};
    std::array<bool, 16u> source_stack_lineage{};
    for (std::uint8_t index = 0u;
         index < destination.size();
         ++index) {
        destination_stack_lineage[index] =
            has_current_inventory_stack_lineage(
                destination, index);
        source_stack_lineage[index] =
            has_current_inventory_stack_lineage(
                source, index);
    }
    const auto alias_sources =
        static_cast<std::uint8_t>(
            destination.inventory_unresolved_saved_stack_alias_sources |
            source.inventory_unresolved_saved_stack_alias_sources);
    if (alias_sources !=
        destination.inventory_unresolved_saved_stack_alias_sources) {
        destination.inventory_unresolved_saved_stack_alias_sources =
            alias_sources;
        changed = true;
    }
    if (source
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch &&
        !destination
             .inventory_unresolved_saved_stack_alias_tracks_current_epoch) {
        destination
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch =
            true;
        changed = true;
    }
    if (source.inventory_current_stack_epoch_alias_watcher &&
        !destination.inventory_current_stack_epoch_alias_watcher) {
        destination.inventory_current_stack_epoch_alias_watcher = true;
        changed = true;
    }
    if (source.inventory_detached_stack_epoch_alias_watcher &&
        !destination.inventory_detached_stack_epoch_alias_watcher) {
        destination.inventory_detached_stack_epoch_alias_watcher = true;
        changed = true;
    }
    if (source.inventory_unresolved_stack_callback_loss &&
        !destination.inventory_unresolved_stack_callback_loss) {
        destination.inventory_unresolved_stack_callback_loss = true;
        changed = true;
    }
    if (source.inventory_stack_callback_loss_identity_truncated &&
        !destination.inventory_stack_callback_loss_identity_truncated) {
        destination.inventory_stack_callback_loss_identity_truncated =
            true;
        changed = true;
    }
    for (std::size_t index = 0u; index < destination.size(); ++index)
        changed = merge_value(destination[index], source[index]) || changed;
    for (std::size_t index = 0u; index < destination.stack_offsets.size(); ++index) {
        const auto merged_may_alias =
            destination.stack_may_alias[index] || source.stack_may_alias[index];
        auto merged_inventory_may_alias =
            destination.inventory_stack_may_alias[index] ||
            source.inventory_stack_may_alias[index];
        const auto merged_inventory_vbr_relative =
            destination.inventory_vbr_relative[index] &&
            source.inventory_vbr_relative[index];
        const auto merged_inventory_fixed_storage_reference =
            destination.inventory_fixed_storage_reference[index] &&
            source.inventory_fixed_storage_reference[index];
        if (destination.stack_offsets[index] != source.stack_offsets[index]) {
            if (destination.stack_offsets[index].has_value()) changed = true;
            destination.stack_offsets[index].reset();
        }
        auto destination_inventory_coordinates =
            inventory_stack_coordinates(
                destination, static_cast<std::uint8_t>(index));
        const auto source_inventory_coordinates =
            inventory_stack_coordinates(
                source, static_cast<std::uint8_t>(index));
        if (!destination_inventory_coordinates.empty() &&
            !source_inventory_coordinates.empty()) {
            destination_inventory_coordinates.insert(
                destination_inventory_coordinates.end(),
                source_inventory_coordinates.begin(),
                source_inventory_coordinates.end());
            normalize_stack_coordinates(
                destination_inventory_coordinates);
            if (destination_inventory_coordinates.size() >
                maximum_inventory_stack_coordinates) {
                destination_inventory_coordinates.clear();
                merged_inventory_may_alias = true;
            }
        } else {
            destination_inventory_coordinates.clear();
        }
        changed =
            set_inventory_stack_coordinates(
                destination,
                static_cast<std::uint8_t>(index),
                std::move(destination_inventory_coordinates)) ||
            changed;
        if (destination.stack_may_alias[index] != merged_may_alias) {
            destination.stack_may_alias[index] = merged_may_alias;
            changed = true;
        }
        if (destination.inventory_stack_may_alias[index] !=
            merged_inventory_may_alias) {
            destination.inventory_stack_may_alias[index] =
                merged_inventory_may_alias;
            changed = true;
        }
        if (destination.inventory_vbr_relative[index] !=
            merged_inventory_vbr_relative) {
            destination.inventory_vbr_relative[index] =
                merged_inventory_vbr_relative;
            changed = true;
        }
        if (destination.inventory_fixed_storage_reference[index] !=
            merged_inventory_fixed_storage_reference) {
            destination.inventory_fixed_storage_reference[index] =
                merged_inventory_fixed_storage_reference;
            changed = true;
        }
        const bool stack_lineage_with_payload_was_lost =
            !has_current_inventory_stack_lineage(
                destination,
                static_cast<std::uint8_t>(index)) &&
            // Preserve path correlation: a stack alias only becomes a loss
            // witness when that same predecessor still carried stack payload.
            ((destination_stack_lineage[index] &&
              destination_has_active_stack_payload) ||
             (source_stack_lineage[index] &&
              source_has_active_stack_payload));
        if (stack_lineage_with_payload_was_lost &&
            !destination[index]
                 .inventory_stack_callback_loss_unresolved) {
            destination[index]
                .inventory_stack_callback_loss_unresolved = true;
            changed = true;
        }
    }
    for (auto slot = destination.stack_values.begin(); slot != destination.stack_values.end();) {
        const auto source_slot = source.stack_values.find(slot->first);
        if (source_slot == source.stack_values.end()) {
            if (has_latent_saved_stack_alias(slot->second)) {
                add_unresolved_saved_stack_alias(
                    destination,
                    unresolved_saved_stack_alias_source_stack,
                    slot->second.inventory_saved_stack_epoch
                        .tracks_current_epoch);
                slot->second.inventory_saved_stack_epoch = {};
                changed = true;
                if (!has_non_epoch_abstract_fact(slot->second)) {
                    slot = destination.stack_values.erase(slot);
                    continue;
                }
            }
            if (may_merge_stack_values ||
                has_saved_stack_epoch(slot->second) ||
                carries_unresolved_stack_callback(slot->second)) {
                const auto original = slot->second;
                if (has_saved_stack_epoch(slot->second))
                    mark_inventory_saved_stack_epoch_unresolved(
                        slot->second.inventory_saved_stack_epoch);
                make_unknown_preserving_provenance(slot->second);
                slot->second.guarded = true;
                slot->second.complete = false;
                changed = slot->second != original || changed;
                ++slot;
            } else {
                slot = destination.stack_values.erase(slot);
                changed = true;
            }
            continue;
        }
        const auto original = slot->second;
        static_cast<void>(
            merge_value(slot->second, source_slot->second));
        if (has_saved_stack_epoch(slot->second) ||
            carries_unresolved_stack_callback(slot->second)) {
            slot->second.guarded = true;
            slot->second.complete = false;
        }
        changed = slot->second != original || changed;
        ++slot;
    }
    if (may_merge_stack_values) {
        for (const auto& [offset, value] : source.stack_values) {
            if (destination.stack_values.contains(offset)) continue;
            auto candidate = value;
            if (has_latent_saved_stack_alias(candidate)) {
                changed =
                    add_unresolved_saved_stack_alias(
                        destination,
                        unresolved_saved_stack_alias_source_stack,
                        candidate.inventory_saved_stack_epoch
                            .tracks_current_epoch) ||
                    changed;
                candidate.inventory_saved_stack_epoch = {};
                if (!has_non_epoch_abstract_fact(candidate))
                    continue;
            }
            candidate.guarded = true;
            candidate.complete = false;
            destination.stack_values.emplace(offset, std::move(candidate));
            changed = true;
        }
    } else {
        for (const auto& [offset, value] : source.stack_values) {
        if (destination.stack_values.contains(offset) ||
            (!has_saved_stack_epoch(value) &&
                 !carries_unresolved_stack_callback(value)))
            continue;
        auto candidate = value;
        if (has_latent_saved_stack_alias(candidate)) {
            changed =
                add_unresolved_saved_stack_alias(
                    destination,
                    unresolved_saved_stack_alias_source_stack,
                    candidate.inventory_saved_stack_epoch
                        .tracks_current_epoch) ||
                changed;
            candidate.inventory_saved_stack_epoch = {};
            if (!has_non_epoch_abstract_fact(candidate))
                continue;
        }
        if (destination.stack_values.size() >=
            maximum_abi_stack_argument_slots) {
            if (carries_stack_callback_payload(candidate) &&
                !destination
                     .inventory_stack_callback_loss_identity_truncated) {
                    destination
                        .inventory_unresolved_stack_callback_loss = true;
                    destination
                        .inventory_stack_callback_loss_identity_truncated =
                        true;
                    changed = true;
                }
                continue;
            }
            if (has_saved_stack_epoch(candidate))
                mark_inventory_saved_stack_epoch_unresolved(
                    candidate.inventory_saved_stack_epoch);
            make_unknown_preserving_provenance(candidate);
            candidate.guarded = true;
            candidate.complete = false;
            destination.stack_values.emplace(offset, std::move(candidate));
            changed = true;
        }
    }
    for (auto value = destination.memory_values.begin();
         value != destination.memory_values.end();) {
        const auto source_value = source.memory_values.find(value->first);
        if (source_value == source.memory_values.end()) {
            if (has_latent_saved_stack_alias(value->second)) {
                add_unresolved_saved_stack_alias(
                    destination,
                    unresolved_saved_stack_alias_source_memory,
                    value->second.inventory_saved_stack_epoch
                        .tracks_current_epoch);
                value->second.inventory_saved_stack_epoch = {};
                changed = true;
                if (!has_non_epoch_abstract_fact(value->second)) {
                    value = destination.memory_values.erase(value);
                    continue;
                }
            }
            if (has_saved_stack_epoch(value->second) ||
                carries_unresolved_stack_callback(value->second)) {
                const auto original = value->second;
                if (has_saved_stack_epoch(value->second))
                    mark_inventory_saved_stack_epoch_unresolved(
                        value->second.inventory_saved_stack_epoch);
                make_unknown_preserving_provenance(value->second);
                value->second.guarded = true;
                value->second.complete = false;
                changed = value->second != original || changed;
                ++value;
            } else {
                value = destination.memory_values.erase(value);
                changed = true;
            }
            continue;
        }
        const auto original = value->second;
        static_cast<void>(
            merge_value(value->second, source_value->second));
        if (has_saved_stack_epoch(value->second) ||
            carries_unresolved_stack_callback(value->second)) {
            value->second.guarded = true;
            value->second.complete = false;
        }
        changed = value->second != original || changed;
        ++value;
    }
    for (const auto& [address, value] : source.memory_values) {
        if (destination.memory_values.contains(address) ||
            (!has_saved_stack_epoch(value) &&
             !carries_unresolved_stack_callback(value)))
            continue;
        auto candidate = value;
        if (has_latent_saved_stack_alias(candidate)) {
            changed =
                add_unresolved_saved_stack_alias(
                    destination,
                    unresolved_saved_stack_alias_source_memory,
                    candidate.inventory_saved_stack_epoch
                        .tracks_current_epoch) ||
                changed;
            candidate.inventory_saved_stack_epoch = {};
            if (!has_non_epoch_abstract_fact(candidate))
                continue;
        }
        if (destination.memory_values.size() >=
            maximum_memory_values) {
            if (carries_stack_callback_payload(candidate) &&
                !destination
                     .inventory_stack_callback_loss_identity_truncated) {
                destination
                    .inventory_unresolved_stack_callback_loss = true;
                destination
                    .inventory_stack_callback_loss_identity_truncated =
                    true;
                changed = true;
            }
            continue;
        }
        if (has_saved_stack_epoch(candidate))
            mark_inventory_saved_stack_epoch_unresolved(
                candidate.inventory_saved_stack_epoch);
        make_unknown_preserving_provenance(candidate);
        candidate.guarded = true;
        candidate.complete = false;
        destination.memory_values.emplace(address, std::move(candidate));
        changed = true;
    }
    return changed;
}

void coalesce_call_arguments(
    std::vector<FunctionEvaluation::CallArguments>& observations) {
    std::sort(observations.begin(),
              observations.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.call_site, left.callee) <
                         std::tie(right.call_site, right.callee);
              });
    std::vector<FunctionEvaluation::CallArguments> merged;
    merged.reserve(observations.size());
    for (auto& observation : observations) {
        if (merged.empty() ||
            merged.back().call_site != observation.call_site ||
            merged.back().callee != observation.callee) {
            merged.push_back(std::move(observation));
            continue;
        }
        // These observations feed the bounded inventory-only context walk.
        // A callback spilled in only one transient predecessor must remain
        // available as guarded/incomplete evidence after coalescing.
        static_cast<void>(
            merge_state(merged.back().state, observation.state, true));
    }
    observations = std::move(merged);
}

void coalesce_inventory_transfers(
    std::vector<FunctionEvaluation::InventoryTransfer>& observations) {
    std::sort(observations.begin(),
              observations.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.transfer_site, left.target) <
                         std::tie(right.transfer_site, right.target);
              });
    std::vector<FunctionEvaluation::InventoryTransfer> merged;
    merged.reserve(observations.size());
    for (auto& observation : observations) {
        if (merged.empty() ||
            merged.back().transfer_site !=
                observation.transfer_site ||
            merged.back().target != observation.target) {
            merged.push_back(std::move(observation));
            continue;
        }
        static_cast<void>(
            merge_state(merged.back().state,
                        observation.state,
                        true));
        merged.back().guarded =
            merged.back().guarded || observation.guarded;
        merged.back().complete =
            merged.back().complete && observation.complete;
    }
    observations = std::move(merged);
}

void coalesce_resolutions(
    std::vector<InterproceduralTargetResolution>& resolutions) {
    std::sort(
        resolutions.begin(),
        resolutions.end(),
        [](const auto& left, const auto& right) {
            if (left.instruction_address != right.instruction_address)
                return left.instruction_address <
                       right.instruction_address;
            if (left.call != right.call)
                return left.call < right.call;
            return left.targets < right.targets;
        });
    std::vector<InterproceduralTargetResolution> merged;
    merged.reserve(resolutions.size());
    for (auto& resolution : resolutions) {
        normalize(resolution.targets);
        normalize(resolution.call_sites);
        normalize(resolution.callees);
        if (merged.empty() ||
            merged.back().instruction_address !=
                resolution.instruction_address) {
            merged.push_back(std::move(resolution));
            continue;
        }
        auto& site = merged.back();
        merge_normalized(site.targets,
                         std::move(resolution.targets));
        merge_normalized(site.call_sites,
                         std::move(resolution.call_sites));
        merge_normalized(site.callees,
                         std::move(resolution.callees));
        site.complete =
            site.complete && resolution.complete;
        site.guarded =
            site.guarded || resolution.guarded ||
            !resolution.complete;
        site.evidence =
            site.targets.empty()
                ? ControlFlowEvidence::Unresolved
                : site.complete
                      ? (site.guarded
                             ? ControlFlowEvidence::GuardedComplete
                             : ControlFlowEvidence::ProvenComplete)
                      : ControlFlowEvidence::GuardedPartial;
        site.reason =
            site.targets.empty()
                ? "all-contexts-unknown"
                : site.complete
                      ? "all-contexts-complete"
                      : "merged-contexts-partial";
    }
    resolutions = std::move(merged);
}

constexpr std::uint16_t register_bit(const std::uint8_t index) {
    return static_cast<std::uint16_t>(1u << index);
}

void clear_written(AbstractState& state, const katana::sh4::DecodedInstruction& instruction) {
    const auto mask = general_register_write_mask(instruction);
    for (std::uint8_t index = 0u; index < state.size(); ++index) {
        if ((mask & register_bit(index)) != 0u) {
            make_unknown(state[index]);
            state.stack_offsets[index].reset();
            clear_inventory_stack_coordinates(state, index);
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
            state.inventory_vbr_relative[index] = false;
            state.inventory_fixed_storage_reference[index] = false;
        }
    }
}

void apply_binary(AbstractValue& destination,
                  const AbstractValue& source,
                  const katana::sh4::InstructionKind kind) {
    const bool inventory_stack_derived =
        destination.inventory_stack_derived || source.inventory_stack_derived;
    const bool contextual_candidate_dependency =
        destination.contextual_candidate_dependency || source.contextual_candidate_dependency;
    const bool inventory_stack_callback_loss_unresolved =
        destination.inventory_stack_callback_loss_unresolved ||
        source.inventory_stack_callback_loss_unresolved;
    auto transformed_saved_stack_epoch =
        destination.inventory_saved_stack_epoch;
    static_cast<void>(merge_inventory_saved_stack_epoch(
        transformed_saved_stack_epoch,
        source.inventory_saved_stack_epoch,
        true));
    if (transformed_saved_stack_epoch.present ||
        transformed_saved_stack_epoch.unresolved)
        mark_inventory_saved_stack_epoch_unresolved(
            transformed_saved_stack_epoch);
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        make_unknown(destination);
        destination.inventory_stack_derived = inventory_stack_derived;
        destination.contextual_candidate_dependency = contextual_candidate_dependency;
        destination.inventory_stack_callback_loss_unresolved =
            inventory_stack_callback_loss_unresolved;
        destination.inventory_saved_stack_epoch =
            std::move(transformed_saved_stack_epoch);
        destination.call_sites = std::move(call_sites);
        destination.callees = std::move(callees);
        return;
    }
    destination.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
    destination.callees.insert(source.callees.begin(), source.callees.end());
    std::vector<std::uint32_t> values;
    for (const auto left : destination.values) {
        for (const auto right : source.values) {
            std::uint32_t value = 0u;
            switch (kind) {
            case katana::sh4::InstructionKind::AddRegister:
                value = left + right;
                break;
            case katana::sh4::InstructionKind::SubRegister:
                value = left - right;
                break;
            case katana::sh4::InstructionKind::AndRegister:
                value = left & right;
                break;
            case katana::sh4::InstructionKind::OrRegister:
                value = left | right;
                break;
            case katana::sh4::InstructionKind::XorRegister:
                value = left ^ right;
                break;
            default:
                make_unknown(destination);
                destination.inventory_stack_derived = inventory_stack_derived;
                destination.contextual_candidate_dependency = contextual_candidate_dependency;
                destination.inventory_stack_callback_loss_unresolved =
                    inventory_stack_callback_loss_unresolved;
                destination.inventory_saved_stack_epoch =
                    std::move(transformed_saved_stack_epoch);
                return;
            }
            const auto position =
                std::lower_bound(values.begin(), values.end(), value);
            if (position != values.end() && *position == value)
                continue;
            if (values.size() >= maximum_summary_values) {
                destination.inventory_stack_derived = inventory_stack_derived;
                destination.inventory_stack_callback_loss_unresolved =
                    inventory_stack_callback_loss_unresolved;
                make_unknown_preserving_provenance(destination);
                destination.inventory_code_pointer = false;
                destination.inventory_pc_relative_code_literal = false;
                destination.inventory_code_pointer_values.clear();
                destination.inventory_pc_relative_code_literal_values.clear();
                destination.inventory_code_pointer_values_truncated = false;
                destination.inventory_pc_relative_code_literal_values_truncated = false;
                destination.contextual_candidate_dependency = contextual_candidate_dependency;
                destination.inventory_saved_stack_epoch =
                    std::move(transformed_saved_stack_epoch);
                return;
            }
            values.insert(position, value);
        }
    }
    destination.values = std::move(values);
    destination.guarded = destination.guarded || source.guarded;
    destination.complete = destination.complete && source.complete;
    destination.inventory_stack_derived = inventory_stack_derived;
    destination.inventory_code_pointer = false;
    destination.inventory_pc_relative_code_literal = false;
    destination.inventory_code_pointer_values.clear();
    destination.inventory_pc_relative_code_literal_values.clear();
    destination.inventory_code_pointer_values_truncated = false;
    destination.inventory_pc_relative_code_literal_values_truncated = false;
    destination.contextual_candidate_dependency = contextual_candidate_dependency;
    destination.inventory_stack_callback_loss_unresolved =
        inventory_stack_callback_loss_unresolved;
    destination.inventory_saved_stack_epoch =
        std::move(transformed_saved_stack_epoch);
}

template <typename Operation> void apply_unary(AbstractValue& value, Operation operation) {
    value.inventory_code_pointer = false;
    value.inventory_pc_relative_code_literal = false;
    value.inventory_code_pointer_values.clear();
    value.inventory_pc_relative_code_literal_values.clear();
    value.inventory_code_pointer_values_truncated = false;
    value.inventory_pc_relative_code_literal_values_truncated = false;
    if (value.inventory_saved_stack_epoch.present ||
        value.inventory_saved_stack_epoch.unresolved)
        mark_inventory_saved_stack_epoch_unresolved(
            value.inventory_saved_stack_epoch);
    if (!value.known) return;
    for (auto& candidate : value.values)
        candidate = operation(candidate);
    normalize(value.values);
}

struct ImageValue {
    std::uint32_t value = 0u;
    bool guarded = false;
};

std::optional<ImageValue> read_image_value(const katana::io::ExecutableImage& image,
                                           const std::uint32_t address,
                                           const std::size_t width) {
    const auto resolved = image.resolve_segment_address(address, width);
    if (!resolved.has_value()) return std::nullopt;
    const auto source_address = *resolved;
    const auto* segment = image.find_segment(source_address, width);
    if (segment == nullptr || !segment->permissions.readable) return std::nullopt;
    const auto offset = segment->byte_offset(source_address);
    if (!offset.has_value() || *offset > segment->bytes.size() ||
        width > segment->bytes.size() - *offset)
        return std::nullopt;
    std::uint32_t value = 0u;
    switch (width) {
    case 1u:
        value = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int8_t>(segment->bytes[*offset])));
        break;
    case 2u:
        value = static_cast<std::uint32_t>(static_cast<std::int32_t>(
            static_cast<std::int16_t>(katana::io::read_u16_le(segment->bytes, *offset))));
        break;
    case 4u:
        value = image.read_u32_le(source_address);
        break;
    default:
        return std::nullopt;
    }
    return ImageValue{value, segment->permissions.writable};
}

[[nodiscard]] bool has_saved_stack_epoch(const AbstractValue& value);
[[nodiscard]] bool carries_unresolved_stack_callback(
    const AbstractValue& value);
[[nodiscard]] bool carries_stack_callback_payload(
    const AbstractValue& value);
void invalidate_memory_values_conservatively(AbstractState& state);

void load_memory_values(AbstractValue& destination,
                        const AbstractState& state,
                        const std::vector<std::uint32_t>& addresses,
                        const std::size_t width,
                        const katana::io::ExecutableImage& image,
                        const AbstractValue* address_evidence = nullptr) {
    // Capture this before a same-register load mutates its destination. The
    // taint selects only the bounded contextual helper slice; it is not
    // code-pointer provenance and never proves a control-flow edge.
    const bool address_contextual_dependency =
        address_evidence != nullptr &&
        address_evidence->contextual_candidate_dependency;
    const auto materialize_latent_alias =
        [&](AbstractValue& value) {
            if (width == 4u)
                materialize_unresolved_saved_stack_alias(
                    value,
                    state,
                    unresolved_saved_stack_alias_source_memory);
        };
    const bool address_stack_callback_loss_unresolved =
        width == 4u && address_evidence != nullptr &&
        carries_unresolved_stack_callback(*address_evidence);
    const bool address_domain_complete =
        !addresses.empty() &&
        (address_evidence == nullptr ||
         (address_evidence->known &&
          address_evidence->complete));
    const auto may_load_stack_callback_payload = [&] {
        if (width != 4u) return false;
        if (!address_domain_complete) {
            return std::any_of(
                state.memory_values.begin(),
                state.memory_values.end(),
                [](const auto& stored) {
                    return carries_stack_callback_payload(
                        stored.second);
                });
        }
        return std::any_of(
            addresses.begin(),
            addresses.end(),
            [&](const auto address) {
                const auto stored =
                    state.memory_values.find(address);
                return stored != state.memory_values.end() &&
                       carries_stack_callback_payload(
                           stored->second);
            });
    }();
    const bool incomplete_address_stack_callback_loss =
        address_stack_callback_loss_unresolved ||
        (!address_domain_complete &&
         may_load_stack_callback_payload);
    if (addresses.empty()) {
        make_unknown(destination);
        destination.contextual_candidate_dependency = address_contextual_dependency;
        destination.inventory_stack_callback_loss_unresolved =
            address_stack_callback_loss_unresolved ||
            may_load_stack_callback_payload;
        materialize_latent_alias(destination);
        return;
    }
    AbstractValue loaded;
    bool first = true;
    for (const auto address : addresses) {
        AbstractValue value;
        bool forwarded_found = false;
        if (width == 4u) {
            const auto forwarded = state.memory_values.find(address);
            if (forwarded != state.memory_values.end()) {
                value = forwarded->second;
                value.guarded = true;
                forwarded_found = true;
            }
        }
        if (!forwarded_found) {
            const auto image_value = read_image_value(image, address, width);
            if (!image_value.has_value()) {
                make_unknown(destination);
                destination.contextual_candidate_dependency = address_contextual_dependency;
                destination.inventory_stack_callback_loss_unresolved =
                    address_stack_callback_loss_unresolved ||
                    may_load_stack_callback_payload;
                materialize_latent_alias(destination);
                return;
            }
            set_value(value, image_value->value);
            value.guarded = image_value->guarded;
            value.complete = !image_value->guarded;
        }
        if (first) {
            loaded = std::move(value);
            first = false;
        } else {
            static_cast<void>(merge_value(loaded, value));
        }
    }
    if (address_evidence != nullptr) {
        loaded.guarded = loaded.guarded || address_evidence->guarded;
        loaded.complete = loaded.complete && address_evidence->complete;
        // The address selects where a value is loaded from; it is not
        // provenance for the loaded contents.  Code-pointer argument
        // provenance is attached only when that value itself crosses a
        // proven ABI boundary. Context-only candidate taint is distinct:
        // it follows a dereference so the bounded helper slice retains its
        // relevant ABI dependency, without creating code-pointer evidence.
        // The loss bit below is likewise not address provenance: it records
        // that this dereference can consume stack-callback payload whose exact
        // stack or memory identity was already lost.
        loaded.contextual_candidate_dependency =
            loaded.contextual_candidate_dependency ||
            address_contextual_dependency;
    }
    loaded.inventory_stack_callback_loss_unresolved =
        loaded.inventory_stack_callback_loss_unresolved ||
        incomplete_address_stack_callback_loss;
    materialize_latent_alias(loaded);
    destination = std::move(loaded);
}

bool memory_ranges_overlap(const std::uint32_t left,
                           const std::size_t left_width,
                           const std::uint32_t right,
                           const std::size_t right_width) {
    const auto left_begin = static_cast<std::uint64_t>(left);
    const auto left_end = left_begin + left_width;
    const auto right_begin = static_cast<std::uint64_t>(right);
    const auto right_end = right_begin + right_width;
    return left_begin < right_end && right_begin < left_end;
}

void note_imprecise_memory_stack_callback_store(
    AbstractState& state,
    const std::size_t width,
    const AbstractValue& value) {
    if (width != 4u) return;
    if (carries_stack_callback_payload(value)) {
        state.inventory_unresolved_stack_callback_loss = true;
    } else if (has_latent_saved_stack_alias(value)) {
        add_unresolved_saved_stack_alias(
            state,
            unresolved_saved_stack_alias_source_memory,
            value.inventory_saved_stack_epoch.tracks_current_epoch);
    }
}

void store_memory_values(AbstractState& state,
                         const std::vector<std::uint32_t>& addresses,
                         const std::size_t width,
    const AbstractValue& value,
    const AbstractValue& address_evidence) {
    if (!address_evidence.known || !address_evidence.complete || addresses.empty()) {
        note_imprecise_memory_stack_callback_store(
            state, width, value);
        invalidate_memory_values_conservatively(state);
        return;
    }
    if (std::any_of(addresses.begin(), addresses.end(), [width](const auto address) {
            return width == 0u ||
                   address > std::numeric_limits<std::uint32_t>::max() - (width - 1u);
        })) {
        note_imprecise_memory_stack_callback_store(
            state, width, value);
        invalidate_memory_values_conservatively(state);
        return;
    }
    if (addresses.size() > 1u) {
        auto possible_store = value;
        make_unknown_preserving_provenance(possible_store);
        possible_store.guarded = true;
        possible_store.complete = false;
        const bool carries_inventory =
            has_inventory_candidate_values(possible_store) ||
            inventory_candidate_values_truncated(possible_store) ||
            possible_store.contextual_candidate_dependency ||
            carries_stack_callback_payload(possible_store) ||
            has_saved_stack_epoch(possible_store);
        for (const auto address : addresses) {
            for (auto& [existing_address, existing_value] :
                 state.memory_values) {
                if (!memory_ranges_overlap(
                        existing_address, 4u, address, width))
                    continue;
                if (width == 4u &&
                    existing_address == address &&
                    carries_inventory)
                    static_cast<void>(
                        merge_value(
                            existing_value, possible_store));
                make_unknown_preserving_provenance(
                    existing_value);
                existing_value.guarded = true;
                existing_value.complete = false;
            }
            if (width != 4u || !carries_inventory ||
                state.memory_values.contains(address))
                continue;
            state.memory_values.emplace(
                address, possible_store);
        }
        if (state.memory_values.size() > maximum_memory_values) {
            invalidate_memory_values_conservatively(state);
            if (state.memory_values.size() >
                maximum_memory_values) {
                for (const auto& [address, saved_alias_value] :
                     state.memory_values) {
                    static_cast<void>(address);
                    if (has_latent_saved_stack_alias(
                            saved_alias_value))
                        add_unresolved_saved_stack_alias(
                            state,
                            unresolved_saved_stack_alias_source_memory,
                            saved_alias_value.inventory_saved_stack_epoch
                                .tracks_current_epoch);
                }
                if (std::any_of(
                        state.memory_values.begin(),
                        state.memory_values.end(),
                        [](const auto& stored) {
                            return carries_stack_callback_payload(
                                stored.second);
                        })) {
                    state.inventory_unresolved_stack_callback_loss =
                        true;
                    state
                        .inventory_stack_callback_loss_identity_truncated =
                        true;
                }
                state.memory_values.clear();
            }
        }
        return;
    }
    for (const auto address : addresses) {
        for (auto existing = state.memory_values.begin(); existing != state.memory_values.end();) {
            if (memory_ranges_overlap(existing->first, 4u, address, width))
                existing = state.memory_values.erase(existing);
            else
                ++existing;
        }
    }
    if (width != 4u ||
        (!value.known && !has_saved_stack_epoch(value) &&
         !carries_unresolved_stack_callback(value)))
        return;
    auto stored = value;
    stored.guarded = true;
    stored.complete = stored.complete && address_evidence.complete;
    // Destination-address provenance must not become provenance of the
    // stored contents.  The source value already carries any legitimate
    // code-pointer evidence.
    for (const auto address : addresses)
        state.memory_values[address] = stored;
    if (state.memory_values.size() > maximum_memory_values) {
        invalidate_memory_values_conservatively(state);
        if (state.memory_values.size() > maximum_memory_values) {
            for (const auto& [address, saved_alias_value] :
                 state.memory_values) {
                static_cast<void>(address);
                if (has_latent_saved_stack_alias(
                        saved_alias_value))
                    add_unresolved_saved_stack_alias(
                        state,
                        unresolved_saved_stack_alias_source_memory,
                        saved_alias_value.inventory_saved_stack_epoch
                            .tracks_current_epoch);
            }
            if (std::any_of(
                    state.memory_values.begin(),
                state.memory_values.end(),
                [](const auto& stored) {
                    return carries_stack_callback_payload(
                        stored.second);
                })) {
                state.inventory_unresolved_stack_callback_loss = true;
                state
                    .inventory_stack_callback_loss_identity_truncated =
                    true;
            }
            state.memory_values.clear();
        }
    }
}

std::optional<std::int32_t> stack_slot(const AbstractState& state,
                                       const std::uint8_t base_register,
                                       const std::int32_t displacement = 0) {
    if (!state.stack_offsets[base_register].has_value()) return std::nullopt;
    const auto base = static_cast<std::int64_t>(*state.stack_offsets[base_register]);
    const auto offset = base + displacement;
    if (offset < -maximum_stack_distance || offset > maximum_stack_distance) return std::nullopt;
    return static_cast<std::int32_t>(offset);
}

[[nodiscard]] std::vector<std::int32_t>
inventory_stack_slots(const AbstractState& state,
                      const std::uint8_t base_register,
                      const std::int32_t displacement = 0) {
    if (!state[base_register].inventory_stack_derived)
        return {};
    auto slots = inventory_stack_coordinates(state, base_register);
    for (auto& slot : slots) {
        const auto displaced =
            static_cast<std::int64_t>(slot) + displacement;
        if (displaced < -maximum_stack_distance ||
            displaced > maximum_stack_distance)
            return {};
        slot = static_cast<std::int32_t>(displaced);
    }
    normalize_stack_coordinates(slots);
    return slots;
}

[[nodiscard]] std::optional<std::int32_t>
bounded_stack_displacement(const AbstractValue& value,
                           const bool require_complete) {
    if (!value.known || value.values.size() != 1u ||
        (require_complete && !value.complete))
        return std::nullopt;
    auto displacement = static_cast<std::int64_t>(value.values.front());
    if (displacement > std::numeric_limits<std::int32_t>::max())
        displacement -= (std::int64_t{1} << 32u);
    if (displacement < -maximum_stack_distance ||
        displacement > maximum_stack_distance)
        return std::nullopt;
    return static_cast<std::int32_t>(displacement);
}

[[nodiscard]] std::optional<std::int32_t>
r0_indexed_stack_slot(const AbstractState& state,
                      const std::uint8_t other_register) {
    if (other_register == 0u) return std::nullopt;
    const auto coordinate = [&](const std::uint8_t register_index)
        -> std::optional<std::int32_t> {
        return stack_slot(state, register_index);
    };
    const auto proven_non_stack = [&](const std::uint8_t register_index) {
        return !state.stack_may_alias[register_index] &&
               !state.inventory_stack_may_alias[register_index];
    };
    const auto combine = [&](const std::uint8_t stack_register,
                             const std::uint8_t displacement_register)
        -> std::optional<std::int32_t> {
        const auto base = coordinate(stack_register);
        if (!base.has_value() || coordinate(displacement_register).has_value() ||
            !proven_non_stack(displacement_register))
            return std::nullopt;
        const auto displacement = bounded_stack_displacement(
            state[displacement_register], true);
        if (!displacement.has_value()) return std::nullopt;
        const auto slot = static_cast<std::int64_t>(*base) + *displacement;
        if (slot < -maximum_stack_distance || slot > maximum_stack_distance)
            return std::nullopt;
        return static_cast<std::int32_t>(slot);
    };
    if (const auto slot = combine(other_register, 0u); slot.has_value())
        return slot;
    return combine(0u, other_register);
}

[[nodiscard]] std::vector<std::int32_t>
r0_indexed_inventory_stack_slots(const AbstractState& state,
                                 const std::uint8_t other_register,
                                 bool* const coordinate_enumeration_failed =
                                     nullptr) {
    if (coordinate_enumeration_failed != nullptr)
        *coordinate_enumeration_failed = false;
    if (other_register == 0u) return {};
    std::vector<std::int32_t> result;
    bool invalid_coordinates = false;
    const auto append = [&](const std::uint8_t stack_register,
                            const std::uint8_t displacement_register) {
        const auto base_coordinates =
            inventory_stack_slots(state, stack_register);
        if (base_coordinates.empty() ||
            !inventory_stack_coordinates(state, displacement_register)
                 .empty() ||
            state.inventory_stack_may_alias[displacement_register] ||
            !state[displacement_register].known ||
            state[displacement_register].values.empty() ||
            state[displacement_register].values.size() >
                maximum_summary_values)
            return;
        for (const auto base : base_coordinates) {
            for (const auto raw_displacement :
                 state[displacement_register].values) {
                auto displacement =
                    static_cast<std::int64_t>(raw_displacement);
                if (displacement >
                    std::numeric_limits<std::int32_t>::max())
                    displacement -= (std::int64_t{1} << 32u);
                const auto slot =
                    static_cast<std::int64_t>(base) + displacement;
                if (!insert_inventory_stack_coordinate(result, slot)) {
                    result.clear();
                    invalid_coordinates = true;
                    if (coordinate_enumeration_failed != nullptr)
                        *coordinate_enumeration_failed = true;
                    return;
                }
            }
        }
    };
    append(other_register, 0u);
    if (result.empty() && !invalid_coordinates)
        append(0u, other_register);
    return result;
}

[[nodiscard]] bool has_saved_stack_epoch(
    const AbstractValue& value) {
    return value.inventory_saved_stack_epoch.present ||
           value.inventory_saved_stack_epoch.unresolved;
}

[[nodiscard]] bool carries_unresolved_stack_callback(
    const AbstractValue& value) {
    return value.inventory_stack_callback_loss_unresolved ||
           value.inventory_saved_stack_epoch.candidate_payload_lost;
}

[[nodiscard]] bool carries_stack_callback_payload(
    const AbstractValue& value) {
    return carries_unresolved_stack_callback(value) ||
           !value.inventory_saved_stack_epoch.slots.empty();
}

[[nodiscard]] bool has_latent_saved_stack_alias(
    const AbstractValue& value) {
    return has_saved_stack_epoch(value) &&
           !carries_stack_callback_payload(value);
}

bool add_unresolved_saved_stack_alias(
    AbstractState& state,
    const std::uint8_t sources,
    const bool tracks_current_epoch) {
    if (sources == 0u) return false;
    const auto merged_sources =
        static_cast<std::uint8_t>(
            state.inventory_unresolved_saved_stack_alias_sources |
            sources);
    const auto merged_tracks_current_epoch =
        state
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        tracks_current_epoch;
    const bool changed =
        merged_sources !=
            state.inventory_unresolved_saved_stack_alias_sources ||
        merged_tracks_current_epoch !=
            state
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
    state.inventory_unresolved_saved_stack_alias_sources =
        merged_sources;
    state.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        merged_tracks_current_epoch;
    return changed;
}

void materialize_unresolved_saved_stack_alias(
    AbstractValue& value,
    const AbstractState& state,
    const std::uint8_t source) {
    if ((state.inventory_unresolved_saved_stack_alias_sources & source) ==
        0u)
        return;
    InventorySavedStackEpoch latent;
    latent.present = true;
    latent.unresolved = true;
    latent.tracks_current_epoch =
        state
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
    static_cast<void>(merge_inventory_saved_stack_epoch(
        value.inventory_saved_stack_epoch, latent, true));
    value.inventory_stack_derived = true;
    value.guarded = true;
    value.complete = false;
}

[[nodiscard]] bool has_non_epoch_abstract_fact(
    const AbstractValue& value) {
    return value.known ||
           has_inventory_candidate_values(value) ||
           inventory_candidate_values_truncated(value) ||
           value.contextual_candidate_dependency ||
           carries_unresolved_stack_callback(value);
}

void collapse_payload_free_stack_aliases(AbstractState& state) {
    for (auto value = state.stack_values.begin();
         value != state.stack_values.end();) {
        if (has_latent_saved_stack_alias(value->second)) {
            add_unresolved_saved_stack_alias(
                state,
                unresolved_saved_stack_alias_source_stack,
                value->second.inventory_saved_stack_epoch
                    .tracks_current_epoch);
            value->second.inventory_saved_stack_epoch = {};
        }
        if (carries_unresolved_stack_callback(value->second)) {
            state.inventory_unresolved_stack_callback_loss = true;
            value->second
                .inventory_stack_callback_loss_unresolved = false;
            if (value->second.inventory_saved_stack_epoch.slots.empty())
                value->second.inventory_saved_stack_epoch = {};
        }
        if (has_non_epoch_abstract_fact(value->second) ||
            has_saved_stack_epoch(value->second)) {
            ++value;
        } else {
            value = state.stack_values.erase(value);
        }
    }
}

template <std::size_t RegisterCount>
[[nodiscard]] bool memory_address_may_reference_unresolved_stack_callback(
    const AbstractState& state,
    const std::array<std::uint8_t, RegisterCount>& registers,
    const std::size_t width) {
    if (width != 4u) return false;
    if (std::any_of(
            registers.begin(),
            registers.end(),
            [&](const auto index) {
                return carries_unresolved_stack_callback(
                    state[index]);
            }))
        return true;
    if (!state.inventory_unresolved_stack_callback_loss)
        return false;
    return std::any_of(
        registers.begin(),
        registers.end(),
        [&](const auto index) {
            return state[index].inventory_stack_derived ||
                   state.inventory_stack_may_alias[index];
        });
}

[[nodiscard]] bool same_stack_callback_provenance(
    const AbstractValue& left,
    const AbstractValue& right) {
    return left.inventory_stack_callback_loss_unresolved ==
               right.inventory_stack_callback_loss_unresolved &&
           left.inventory_saved_stack_epoch ==
               right.inventory_saved_stack_epoch;
}

[[nodiscard]] bool has_active_inventory_stack_payload(
    const AbstractState& state) {
    return std::any_of(
        state.stack_values.begin(),
        state.stack_values.end(),
        [](const auto& stored) {
            return has_inventory_candidate_values(stored.second) ||
                   inventory_candidate_values_truncated(stored.second) ||
                   stored.second.contextual_candidate_dependency ||
                   carries_stack_callback_payload(stored.second);
        });
}

[[nodiscard]] AbstractValue value_with_saved_stack_epoch(
    const AbstractState& state,
    const std::uint8_t source_register,
    const std::size_t width) {
    auto stored = state[source_register];
    if (width != 4u) return stored;
    const auto coordinates =
        inventory_stack_coordinates(state, source_register);
    if (!stored.inventory_stack_derived || coordinates.empty()) {
        if (stored.inventory_stack_derived &&
            !has_saved_stack_epoch(stored)) {
            stored.inventory_saved_stack_epoch.present = true;
            stored.inventory_saved_stack_epoch.tracks_current_epoch = true;
            mark_inventory_saved_stack_epoch_unresolved(
                stored.inventory_saved_stack_epoch,
                state.inventory_unresolved_stack_callback_loss ||
                    has_active_inventory_stack_payload(state));
        }
        return stored;
    }

    InventorySavedStackEpoch captured;
    captured.present = true;
    captured.tracks_current_epoch = true;
    if (state.inventory_unresolved_stack_callback_loss)
        mark_inventory_saved_stack_epoch_unresolved(
            captured, true);
    for (const auto& [absolute_slot, value] : state.stack_values) {
        if (captured.unresolved)
            break;
        if (carries_stack_callback_payload(value)) {
            mark_inventory_saved_stack_epoch_unresolved(
                captured, true);
            break;
        }
        const bool relevant =
            has_inventory_candidate_values(value) ||
            inventory_candidate_values_truncated(value) ||
            value.contextual_candidate_dependency;
        if (!relevant && !has_saved_stack_epoch(value))
            continue;
        if (has_saved_stack_epoch(value)) {
            if (has_latent_saved_stack_alias(value))
                continue;
            mark_inventory_saved_stack_epoch_unresolved(
                captured,
                value.inventory_saved_stack_epoch
                        .candidate_payload_lost ||
                    !value.inventory_saved_stack_epoch.slots.empty());
            break;
        }
        if (!relevant) continue;
        for (const auto coordinate : coordinates) {
            const auto relative =
                static_cast<std::int64_t>(absolute_slot) -
                static_cast<std::int64_t>(coordinate);
            if (relative < -maximum_stack_distance ||
                relative > maximum_stack_distance) {
                mark_inventory_saved_stack_epoch_unresolved(
                    captured, true);
                break;
            }
            InventorySavedStackSlot slot;
            slot.relative_slot =
                static_cast<std::int32_t>(relative);
            slot.inventory_code_pointer_values =
                value.inventory_code_pointer_values;
            slot.inventory_pc_relative_code_literal_values =
                value.inventory_pc_relative_code_literal_values;
            slot.inventory_code_pointer_values_truncated =
                value.inventory_code_pointer_values_truncated;
            slot.inventory_pc_relative_code_literal_values_truncated =
                value
                    .inventory_pc_relative_code_literal_values_truncated;
            slot.contextual_candidate_dependency =
                value.contextual_candidate_dependency;
            slot.call_sites = value.call_sites;
            slot.callees = value.callees;
            InventorySavedStackEpoch one;
            one.present = true;
            one.slots.push_back(std::move(slot));
            static_cast<void>(merge_inventory_saved_stack_epoch(
                captured, one, false));
            if (captured.unresolved)
                break;
        }
    }
    stored.inventory_saved_stack_epoch = std::move(captured);
    return stored;
}

void merge_saved_stack_epoch_memory_forward(
    AbstractValue& destination,
    const AbstractState& state,
    const std::span<const std::uint32_t> addresses,
    const std::size_t width) {
    if (width != 4u) return;
    for (const auto address : addresses) {
        const auto forwarded = state.memory_values.find(address);
        if (forwarded == state.memory_values.end()) continue;
        if (has_saved_stack_epoch(forwarded->second)) {
            static_cast<void>(merge_inventory_saved_stack_epoch(
                destination.inventory_saved_stack_epoch,
                forwarded->second.inventory_saved_stack_epoch,
                false));
        }
        destination.inventory_stack_callback_loss_unresolved =
            destination.inventory_stack_callback_loss_unresolved ||
            forwarded->second
                .inventory_stack_callback_loss_unresolved;
    }
}

void invalidate_memory_values_conservatively(AbstractState& state) {
    for (auto value = state.memory_values.begin();
         value != state.memory_values.end();) {
        if (!has_saved_stack_epoch(value->second) &&
            !carries_unresolved_stack_callback(value->second)) {
            value = state.memory_values.erase(value);
            continue;
        }
        make_unknown_preserving_provenance(value->second);
        value->second.guarded = true;
        value->second.complete = false;
        ++value;
    }
}

void invalidate_stack_values_conservatively(AbstractState& state) {
    for (auto value = state.stack_values.begin();
         value != state.stack_values.end();) {
        if (!has_inventory_candidate_values(value->second) &&
            !inventory_candidate_values_truncated(value->second) &&
            !value->second.contextual_candidate_dependency &&
            !carries_unresolved_stack_callback(value->second) &&
            !has_saved_stack_epoch(value->second)) {
            value = state.stack_values.erase(value);
            continue;
        }
        make_unknown_preserving_provenance(value->second);
        value->second.guarded = true;
        value->second.complete = false;
        ++value;
    }
}

void mark_current_epoch_saved_snapshots_unresolved(
    AbstractState& state) {
    if (state
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        state.inventory_current_stack_epoch_alias_watcher)
        state.inventory_unresolved_stack_callback_loss = true;
    const auto mark = [](AbstractValue& value) {
        auto& epoch = value.inventory_saved_stack_epoch;
        if (epoch.tracks_current_epoch)
            mark_inventory_saved_stack_epoch_unresolved(
                epoch, true);
    };
    for (auto& value : state.registers)
        mark(value);
    for (auto& [slot, value] : state.stack_values) {
        static_cast<void>(slot);
        mark(value);
    }
    for (auto& [address, value] : state.memory_values) {
        static_cast<void>(address);
        mark(value);
    }
}

void load_inventory_stack_values(
    AbstractValue& destination,
    const AbstractState& state,
    const std::span<const std::int32_t> slots,
    const std::size_t width) {
    make_unknown(destination);
    if (width != 4u) return;
    if (slots.empty()) {
        materialize_unresolved_saved_stack_alias(
            destination,
            state,
            unresolved_saved_stack_alias_source_stack);
        return;
    }
    for (const auto slot : slots) {
        const auto stored = state.stack_values.find(slot);
        if (stored == state.stack_values.end()) continue;
        const auto& value = stored->second;
        static_cast<void>(merge_inventory_candidate_values(
            destination.inventory_code_pointer_values,
            destination.inventory_code_pointer_values_truncated,
            value.inventory_code_pointer_values,
            value.inventory_code_pointer_values_truncated));
        static_cast<void>(merge_inventory_candidate_values(
            destination.inventory_pc_relative_code_literal_values,
            destination.inventory_pc_relative_code_literal_values_truncated,
            value.inventory_pc_relative_code_literal_values,
            value.inventory_pc_relative_code_literal_values_truncated));
        destination.call_sites.insert(value.call_sites.begin(),
                                      value.call_sites.end());
        destination.callees.insert(value.callees.begin(),
                                   value.callees.end());
        destination.inventory_stack_callback_loss_unresolved =
            destination.inventory_stack_callback_loss_unresolved ||
            value.inventory_stack_callback_loss_unresolved;
        static_cast<void>(merge_inventory_saved_stack_epoch(
            destination.inventory_saved_stack_epoch,
            value.inventory_saved_stack_epoch,
            false));
    }
    materialize_unresolved_saved_stack_alias(
        destination,
        state,
        unresolved_saved_stack_alias_source_stack);
    destination.guarded = true;
    destination.complete = false;
    synchronize_inventory_provenance(destination);
}

void record_unresolved_stack_candidate(
    AbstractState& state,
    const std::size_t width,
    const std::uint32_t site,
    const AbstractValue& candidate_value,
    const std::uint32_t owner,
    const char* const kind) {
    if (width != 4u ||
        !has_finite_inventory_candidate_values(candidate_value))
        return;
    state.inventory_unresolved_stack_callback_loss = true;
    const auto candidate =
        !candidate_value.inventory_code_pointer_values.empty()
            ? candidate_value.inventory_code_pointer_values.front()
            : candidate_value.inventory_pc_relative_code_literal_values.front();
    emit_analyzer_stack_diagnostic(kind, owner, site, candidate);
}

void note_unresolved_stack_candidate_store(
    AbstractState& state,
    const std::uint8_t base_register,
    const std::size_t width,
    const std::uint32_t store_site,
    const AbstractValue& stored_value) {
    if (width != 4u ||
        !inventory_stack_coordinates(state, base_register).empty() ||
        (!state[base_register].inventory_stack_derived &&
         !carries_unresolved_stack_callback(
             state[base_register])) ||
        (!has_finite_inventory_candidate_values(stored_value) &&
         !carries_stack_callback_payload(stored_value)))
        return;
    if (carries_stack_callback_payload(stored_value)) {
        state.inventory_unresolved_stack_callback_loss = true;
        return;
    }
    // This is not "an unknown stack access". It records the narrower event
    // that a concrete inventory callback could not be assigned to an ABI slot.
    record_unresolved_stack_candidate(
        state,
        width,
        store_site,
        stored_value,
        static_cast<std::uint32_t>(base_register),
        "candidate-store");
}

void invalidate_stack_range(AbstractState& state,
                             const std::optional<std::int32_t> offset,
                             const bool may_alias_stack,
                             const bool preserve_guarded_inventory,
                             const std::size_t width) {
    if (!offset.has_value()) {
        if (!may_alias_stack) return;
        if (preserve_guarded_inventory) {
            for (auto& [stack_offset, value] : state.stack_values) {
                static_cast<void>(stack_offset);
                value.guarded = true;
                value.complete = false;
            }
        } else {
            invalidate_stack_values_conservatively(state);
        }
        return;
    }
    const auto begin = static_cast<std::int64_t>(*offset);
    const auto end = begin + static_cast<std::int64_t>(width);
    for (auto slot = state.stack_values.begin(); slot != state.stack_values.end();) {
        const auto slot_begin = static_cast<std::int64_t>(slot->first);
        const auto slot_end = slot_begin + 4;
        if (slot_begin < end && begin < slot_end)
            slot = state.stack_values.erase(slot);
        else
            ++slot;
    }
}

void invalidate_stack_write_alternatives(
    AbstractState& state,
    const std::span<const std::pair<std::int32_t, std::size_t>>
        alternatives,
    const bool may_alias_stack) {
    if (alternatives.empty()) {
        if (!may_alias_stack) return;
        for (auto& [slot, stored] : state.stack_values) {
            static_cast<void>(slot);
            make_unknown_preserving_provenance(stored);
            stored.guarded = true;
            stored.complete = false;
        }
        return;
    }
    for (auto stored = state.stack_values.begin();
         stored != state.stack_values.end();) {
        const auto slot_begin =
            static_cast<std::int64_t>(stored->first);
        const auto slot_end = slot_begin + 4;
        const auto overlap_count = static_cast<std::size_t>(
            std::count_if(
                alternatives.begin(),
                alternatives.end(),
                [&](const auto& alternative) {
                    const auto begin =
                        static_cast<std::int64_t>(
                            alternative.first);
                    const auto end =
                        begin +
                        static_cast<std::int64_t>(
                            alternative.second);
                    return slot_begin < end && begin < slot_end;
                }));
        if (overlap_count == alternatives.size()) {
            stored = state.stack_values.erase(stored);
            continue;
        }
        if (overlap_count != 0u) {
            make_unknown_preserving_provenance(stored->second);
            stored->second.guarded = true;
            stored->second.complete = false;
        }
        ++stored;
    }
}

void store_stack_value(AbstractState& state,
                       const std::optional<std::int32_t> offset,
                       const bool may_alias_stack,
                       const bool preserve_guarded_inventory,
                       const std::size_t width,
                       const AbstractValue& value) {
    const bool relevant_snapshot_mutation =
        width == 4u &&
        (has_inventory_candidate_values(value) ||
         inventory_candidate_values_truncated(value) ||
         value.contextual_candidate_dependency ||
         carries_stack_callback_payload(value));
    if (relevant_snapshot_mutation &&
        (offset.has_value() || may_alias_stack))
        mark_current_epoch_saved_snapshots_unresolved(state);
    if (width == 4u && !offset.has_value() && may_alias_stack) {
        if (carries_stack_callback_payload(value)) {
            state.inventory_unresolved_stack_callback_loss = true;
        } else if (has_latent_saved_stack_alias(value)) {
            add_unresolved_saved_stack_alias(
                state,
                unresolved_saved_stack_alias_source_stack,
                value.inventory_saved_stack_epoch
                    .tracks_current_epoch);
        }
    }
    invalidate_stack_range(
        state, offset, may_alias_stack, preserve_guarded_inventory, width);
    if (offset.has_value() && width == 4u &&
        (value.known || has_inventory_candidate_values(value) ||
         carries_stack_callback_payload(value) ||
         has_saved_stack_epoch(value))) {
        if (!state.stack_values.contains(*offset) &&
            state.stack_values.size() >=
                maximum_abi_stack_argument_slots &&
            (has_inventory_candidate_values(value) ||
             inventory_candidate_values_truncated(value) ||
             carries_stack_callback_payload(value))) {
            state.inventory_unresolved_stack_callback_loss = true;
            state.inventory_stack_callback_loss_identity_truncated =
                true;
            return;
        }
        if (!state.stack_values.contains(*offset) &&
            state.stack_values.size() >=
                maximum_abi_stack_argument_slots &&
            has_latent_saved_stack_alias(value)) {
            add_unresolved_saved_stack_alias(
                state,
                unresolved_saved_stack_alias_source_stack,
                value.inventory_saved_stack_epoch
                    .tracks_current_epoch);
            return;
        }
        state.stack_values[*offset] = value;
    }
}

void store_inventory_stack_value(
    AbstractState& state,
    const std::optional<std::int32_t> semantic_offset,
    const std::span<const std::int32_t> inventory_offsets,
    const std::size_t width,
    const AbstractValue& value) {
    if (semantic_offset.has_value() || inventory_offsets.empty())
        return;
    if (width == 4u &&
        (has_inventory_candidate_values(value) ||
         inventory_candidate_values_truncated(value) ||
         value.contextual_candidate_dependency ||
         carries_stack_callback_payload(value)))
        mark_current_epoch_saved_snapshots_unresolved(state);
    std::vector<std::pair<std::int32_t, std::size_t>>
        write_alternatives;
    write_alternatives.reserve(inventory_offsets.size());
    for (const auto inventory_offset : inventory_offsets)
        write_alternatives.emplace_back(inventory_offset, width);
    invalidate_stack_write_alternatives(
        state, write_alternatives, true);
    if (width != 4u ||
        (!has_finite_inventory_candidate_values(value) &&
         !carries_stack_callback_payload(value) &&
         !has_saved_stack_epoch(value)))
        return;
    auto inventory_value = value;
    // The shared slot map may feed ordinary loads.  Keep only candidate
    // provenance here so a writable frame literal can never become semantic
    // memory evidence.
    make_unknown_preserving_provenance(inventory_value);
    inventory_value.guarded = true;
    inventory_value.complete = false;
    for (const auto inventory_offset : inventory_offsets) {
        if (!state.stack_values.contains(inventory_offset) &&
            state.stack_values.size() >=
                maximum_abi_stack_argument_slots) {
            if (carries_stack_callback_payload(inventory_value)) {
                state.inventory_unresolved_stack_callback_loss = true;
                state.inventory_stack_callback_loss_identity_truncated =
                    true;
            } else if (has_latent_saved_stack_alias(
                           inventory_value)) {
                add_unresolved_saved_stack_alias(
                    state,
                    unresolved_saved_stack_alias_source_stack,
                    inventory_value.inventory_saved_stack_epoch
                        .tracks_current_epoch);
            }
            continue;
        }
        const auto [stored, inserted] =
            state.stack_values.try_emplace(
                inventory_offset, inventory_value);
        if (!inserted) {
            static_cast<void>(
                merge_value(stored->second, inventory_value));
            stored->second.guarded = true;
            stored->second.complete = false;
        }
    }
}

void load_stack_value(AbstractValue& destination,
                      const AbstractState& state,
                      const std::optional<std::int32_t> offset,
                      const std::size_t width) {
    if (!offset.has_value() || width != 4u) {
        make_unknown(destination);
        if (width == 4u)
            materialize_unresolved_saved_stack_alias(
                destination,
                state,
                unresolved_saved_stack_alias_source_stack);
        return;
    }
    const auto value = state.stack_values.find(*offset);
    if (value == state.stack_values.end()) {
        make_unknown(destination);
        materialize_unresolved_saved_stack_alias(
            destination,
            state,
            unresolved_saved_stack_alias_source_stack);
        return;
    }
    destination = value->second;
    materialize_unresolved_saved_stack_alias(
        destination,
        state,
        unresolved_saved_stack_alias_source_stack);
}

void adjust_stack_offset(AbstractState& state,
                         const std::uint8_t register_index,
                         const std::int32_t delta) {
    auto& saved_epoch =
        state[register_index].inventory_saved_stack_epoch;
    if (saved_epoch.present) {
        bool rebased = true;
        for (auto& slot : saved_epoch.slots) {
            const auto relative =
                static_cast<std::int64_t>(slot.relative_slot) - delta;
            if (relative < -maximum_stack_distance ||
                relative > maximum_stack_distance) {
                rebased = false;
                break;
            }
            slot.relative_slot =
                static_cast<std::int32_t>(relative);
        }
        if (!rebased) {
            mark_inventory_saved_stack_epoch_unresolved(
                saved_epoch);
        }
    }
    state[register_index].inventory_code_pointer = false;
    state[register_index].inventory_pc_relative_code_literal = false;
    state[register_index].inventory_code_pointer_values.clear();
    state[register_index].inventory_pc_relative_code_literal_values.clear();
    state[register_index].inventory_code_pointer_values_truncated = false;
    state[register_index].inventory_pc_relative_code_literal_values_truncated = false;
    if (state.stack_offsets[register_index].has_value()) {
        const auto adjusted = static_cast<std::int64_t>(
                                  *state.stack_offsets[register_index]) +
                              delta;
        if (adjusted < -maximum_stack_distance || adjusted > maximum_stack_distance)
            state.stack_offsets[register_index].reset();
        else
            state.stack_offsets[register_index] =
                static_cast<std::int32_t>(adjusted);
    }
    auto inventory_coordinates =
        inventory_stack_coordinates(state, register_index);
    for (auto& coordinate : inventory_coordinates) {
        const auto adjusted =
            static_cast<std::int64_t>(coordinate) + delta;
        if (adjusted < -maximum_stack_distance ||
            adjusted > maximum_stack_distance) {
            inventory_coordinates.clear();
            break;
        }
        coordinate = static_cast<std::int32_t>(adjusted);
    }
    static_cast<void>(set_inventory_stack_coordinates(
        state, register_index, std::move(inventory_coordinates)));
    if (state[register_index].known) {
        for (auto& value : state[register_index].values)
            value += static_cast<std::uint32_t>(delta);
        normalize(state[register_index].values);
    }
}

void branch_inventory_stack_position(
    AbstractState& state,
    const std::uint8_t register_index,
    const std::span<const std::int32_t> deltas) {
    const auto inventory_stack_derived =
        state[register_index].inventory_stack_derived;
    const auto old_saved_epoch =
        state[register_index].inventory_saved_stack_epoch;
    InventorySavedStackEpoch branched_saved_epoch;
    if (has_saved_stack_epoch(state[register_index])) {
        if (deltas.empty()) {
            branched_saved_epoch = old_saved_epoch;
            mark_inventory_saved_stack_epoch_unresolved(
                branched_saved_epoch);
        }
        for (const auto delta : deltas) {
            auto branch = old_saved_epoch;
            if (branch.unresolved) {
                mark_inventory_saved_stack_epoch_unresolved(
                    branch);
            }
            for (auto& slot : branch.slots) {
                const auto relative =
                    static_cast<std::int64_t>(
                        slot.relative_slot) -
                    delta;
                if (relative < -maximum_stack_distance ||
                    relative > maximum_stack_distance) {
                    mark_inventory_saved_stack_epoch_unresolved(
                        branch);
                    break;
                }
                slot.relative_slot =
                    static_cast<std::int32_t>(relative);
            }
            static_cast<void>(
                merge_inventory_saved_stack_epoch(
                    branched_saved_epoch,
                    branch,
                    false));
        }
    }
    const auto old_coordinates =
        inventory_stack_coordinates(state, register_index);
    std::vector<std::int32_t> branched_coordinates;
    branched_coordinates.reserve(old_coordinates.size() * deltas.size());
    for (const auto coordinate : old_coordinates) {
        for (const auto delta : deltas) {
            const auto adjusted =
                static_cast<std::int64_t>(coordinate) + delta;
            if (adjusted < -maximum_stack_distance ||
                adjusted > maximum_stack_distance) {
                branched_coordinates.clear();
                break;
            }
            branched_coordinates.push_back(
                static_cast<std::int32_t>(adjusted));
        }
        if (branched_coordinates.empty() && !old_coordinates.empty())
            break;
    }
    normalize_stack_coordinates(branched_coordinates);
    if (branched_coordinates.size() >
        maximum_inventory_stack_coordinates)
        branched_coordinates.clear();
    make_unknown(state[register_index]);
    state[register_index].inventory_stack_derived =
        inventory_stack_derived;
    state[register_index].inventory_saved_stack_epoch =
        std::move(branched_saved_epoch);
    state.stack_offsets[register_index].reset();
    static_cast<void>(set_inventory_stack_coordinates(
        state, register_index, std::move(branched_coordinates)));
    state.stack_may_alias[register_index] = true;
    state.inventory_stack_may_alias[register_index] = true;
    state.inventory_vbr_relative[register_index] = false;
    state.inventory_fixed_storage_reference[register_index] = false;
}

void begin_fresh_inventory_stack_epoch(AbstractState& state) {
    // A complete 32-bit replacement of r15 (for example a scheduler restoring
    // a saved context stack) starts a different symbolic stack namespace.
    // Facts and aliases from the suspended stack must neither project into the
    // resumed ABI nor become coordinates relative to the new r15.
    auto& stack_pointer = state[15u];
    const bool restored_stack_callback_loss =
        stack_pointer.inventory_stack_callback_loss_unresolved;
    auto restored_epoch =
        std::move(stack_pointer.inventory_saved_stack_epoch);
    const bool restored_saved_epoch =
        restored_epoch.present || restored_epoch.unresolved;
    state.inventory_detached_stack_epoch_alias_watcher =
        state.inventory_detached_stack_epoch_alias_watcher ||
        state.inventory_current_stack_epoch_alias_watcher ||
        (state
                 .inventory_unresolved_saved_stack_alias_sources !=
             0u &&
         state
             .inventory_unresolved_saved_stack_alias_tracks_current_epoch) ||
        std::any_of(
            state.stack_values.begin(),
            state.stack_values.end(),
            [](const auto& stored) {
                return has_latent_saved_stack_alias(stored.second);
            });
    state.inventory_current_stack_epoch_alias_watcher = false;
    stack_pointer.inventory_saved_stack_epoch = {};
    const auto detach_saved_epoch = [](AbstractValue& value) {
        value.inventory_saved_stack_epoch.tracks_current_epoch = false;
    };
    for (auto& value : state.registers)
        detach_saved_epoch(value);
    for (auto& [slot, value] : state.stack_values) {
        static_cast<void>(slot);
        detach_saved_epoch(value);
    }
    for (auto& [address, value] : state.memory_values) {
        static_cast<void>(address);
        detach_saved_epoch(value);
    }
    const bool surviving_saved_epoch_alias =
        std::any_of(
            state.registers.begin(),
            state.registers.begin() + 15,
            [](const auto& value) {
                return has_saved_stack_epoch(value);
            }) ||
        std::any_of(
            state.memory_values.begin(),
            state.memory_values.end(),
            [](const auto& stored) {
                return has_saved_stack_epoch(stored.second);
            });
    if (restored_saved_epoch &&
        (state.inventory_detached_stack_epoch_alias_watcher ||
         surviving_saved_epoch_alias))
        state.inventory_current_stack_epoch_alias_watcher = true;
    restored_epoch.tracks_current_epoch = false;
    // Irrespective of whether the epoch can be detached safely, an
    // architectural stack pointer is never a callback payload.
    stack_pointer.inventory_code_pointer = false;
    stack_pointer.inventory_pc_relative_code_literal = false;
    stack_pointer.inventory_code_pointer_values.clear();
    stack_pointer.inventory_pc_relative_code_literal_values.clear();
    stack_pointer.inventory_code_pointer_values_truncated = false;
    stack_pointer.inventory_pc_relative_code_literal_values_truncated = false;
    stack_pointer.contextual_candidate_dependency = false;
    stack_pointer.inventory_stack_callback_loss_unresolved = false;
    stack_pointer.call_sites.clear();
    stack_pointer.callees.clear();
    const bool old_stack_has_inventory_payload =
        has_active_inventory_stack_payload(state);
    const bool old_stack_alias_survives =
        std::any_of(
            state.registers.begin(),
            state.registers.begin() + 15,
            [&](const auto& value) {
                const auto index = static_cast<std::uint8_t>(
                    &value - state.registers.data());
                return state.stack_offsets[index].has_value() ||
                       !inventory_stack_coordinates(state, index).empty() ||
                       (value.inventory_stack_derived &&
                        !has_saved_stack_epoch(value));
            });
    // A definite alias of the suspended current epoch cannot be silently
    // reinterpreted relative to the resumed one. Keep the diagnostic sticky,
    // but always establish a coherent new r15 namespace.
    if (old_stack_has_inventory_payload && old_stack_alias_survives)
        state.inventory_unresolved_stack_callback_loss = true;
    state.stack_values.clear();
    state.inventory_unresolved_saved_stack_alias_sources =
        static_cast<std::uint8_t>(
            state.inventory_unresolved_saved_stack_alias_sources &
            unresolved_saved_stack_alias_source_memory);
    // Remaining unknown memory aliases now refer to the suspended epoch, not
    // to an unrelated new stack. Any saved-epoch restore can, however, refer
    // to the same suspended epoch, so rearm them conservatively then.
    state.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        restored_saved_epoch &&
        state.inventory_unresolved_saved_stack_alias_sources != 0u;
    if (restored_saved_epoch &&
        state.inventory_detached_stack_epoch_alias_watcher)
        add_unresolved_saved_stack_alias(
            state,
            unresolved_saved_stack_alias_source_stack,
            true);
    for (std::uint8_t index = 0u; index < 15u; ++index) {
        const bool was_old_stack_alias =
            state[index].inventory_stack_derived ||
            state.stack_offsets[index].has_value() ||
            !inventory_stack_coordinates(state, index).empty();
        state.stack_offsets[index].reset();
        clear_inventory_stack_coordinates(state, index);
        state[index].inventory_stack_derived = false;
        if (was_old_stack_alias) {
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
        }
    }
    state.stack_offsets[15u].reset();
    clear_inventory_stack_coordinates(state, 15u);
    static_cast<void>(set_inventory_stack_coordinates(
        state, 15u, std::vector<std::int32_t>{0}));
    stack_pointer.inventory_stack_derived = true;
    state.stack_may_alias[15u] = true;
    state.inventory_stack_may_alias[15u] = true;
    state.inventory_vbr_relative[15u] = false;
    state.inventory_fixed_storage_reference[15u] = false;

    if (restored_stack_callback_loss ||
        restored_epoch.candidate_payload_lost)
        state.inventory_unresolved_stack_callback_loss = true;
    for (const auto& slot : restored_epoch.slots) {
        AbstractValue restored;
        restored.guarded = true;
        restored.complete = false;
        restored.inventory_code_pointer_values =
            slot.inventory_code_pointer_values;
        restored.inventory_pc_relative_code_literal_values =
            slot.inventory_pc_relative_code_literal_values;
        restored.inventory_code_pointer_values_truncated =
            slot.inventory_code_pointer_values_truncated;
        restored.inventory_pc_relative_code_literal_values_truncated =
            slot.inventory_pc_relative_code_literal_values_truncated;
        restored.contextual_candidate_dependency =
            slot.contextual_candidate_dependency;
        restored.call_sites = slot.call_sites;
        restored.callees = slot.callees;
        synchronize_inventory_provenance(restored);
        if (inventory_candidate_values_truncated(restored))
            state.inventory_unresolved_stack_callback_loss = true;
        const auto [stored, inserted] =
            state.stack_values.try_emplace(
                slot.relative_slot, restored);
        if (!inserted) {
            static_cast<void>(
                merge_value(stored->second, restored));
            stored->second.guarded = true;
            stored->second.complete = false;
        }
    }
}

std::vector<std::uint32_t> displaced_addresses(const AbstractValue& base,
                                               const std::uint32_t displacement) {
    if (!base.known) return {};
    std::vector<std::uint32_t> addresses;
    addresses.reserve(base.values.size());
    for (const auto value : base.values)
        addresses.push_back(value + displacement);
    normalize(addresses);
    return addresses;
}

std::vector<std::uint32_t> indexed_addresses(const AbstractValue& left,
                                             const AbstractValue& right,
                                             const bool same_register = false) {
    if (!left.known || !right.known) return {};
    std::vector<std::uint32_t> addresses;
    if (same_register) {
        addresses.reserve(left.values.size());
        for (const auto value : left.values)
            addresses.push_back(value + value);
        normalize(addresses);
        return addresses;
    }
    for (const auto left_value : left.values) {
        for (const auto right_value : right.values) {
            addresses.push_back(left_value + right_value);
            normalize(addresses);
            if (addresses.size() > maximum_summary_values) return {};
        }
    }
    return addresses;
}

void apply_transfer(AbstractState& state,
                    const katana::sh4::DisassemblyLine& line,
                    const katana::io::ExecutableImage& image,
                    const bool preserve_guarded_stack_inventory = false) {
    [[maybe_unused]] const InventoryStackOffsetLossDiagnostic
        inventory_sp_loss_diagnostic{
            state, line.address, state.inventory_stack_offsets[15u]};
    const auto& instruction = line.instruction;
    const auto incoming_source_vbr_relative =
        state.inventory_vbr_relative[instruction.source_register];
    const auto incoming_destination_vbr_relative =
        state.inventory_vbr_relative[instruction.destination_register];
    const auto incoming_source_fixed_storage_reference =
        state.inventory_fixed_storage_reference[instruction.source_register];
    const auto incoming_destination_fixed_storage_reference =
        state.inventory_fixed_storage_reference[instruction.destination_register];
    const auto incoming_r0_fixed_storage_reference =
        state.inventory_fixed_storage_reference[0u];
    const auto written_registers = general_register_write_mask(instruction);
    for (std::uint8_t index = 0u; index < state.size(); ++index) {
        if ((written_registers & register_bit(index)) != 0u) {
            state.inventory_vbr_relative[index] = false;
            state.inventory_fixed_storage_reference[index] = false;
        }
    }
    const auto begin_unresolved_inventory_stack_epoch = [&] {
        if (has_active_inventory_stack_payload(state))
            state.inventory_unresolved_stack_callback_loss = true;
        // An unknown/non-affine transformation of the current r15 is not a
        // context restore. It remains the same stack epoch with an unknown
        // coordinate, so a later candidate access must fail closed instead of
        // being reinterpreted at a fabricated fresh offset zero.
        state.stack_offsets[15u].reset();
        clear_inventory_stack_coordinates(state, 15u);
        state[15u].inventory_stack_derived = true;
        state.stack_may_alias[15u] = true;
        state.inventory_stack_may_alias[15u] = true;
        state.inventory_vbr_relative[15u] = false;
        state.inventory_fixed_storage_reference[15u] = false;
    };
    const auto invalidate_transformed_stack_coordinate =
        [&](const std::uint8_t register_index) {
            const bool loses_current_stack_lineage =
                register_index != 15u &&
                has_active_inventory_stack_payload(state) &&
                has_current_inventory_stack_lineage(
                    state, register_index);
            state.stack_offsets[register_index].reset();
            clear_inventory_stack_coordinates(state, register_index);
            state[register_index].inventory_stack_derived = false;
            if (loses_current_stack_lineage)
                state[register_index]
                    .inventory_stack_callback_loss_unresolved =
                    true;
            if (register_index == 15u)
                begin_unresolved_inventory_stack_epoch();
        };
    const auto invalidate_banked_general_registers = [&] {
        for (std::uint8_t index = 0u; index <= 7u; ++index) {
            make_unknown(state[index]);
            state.stack_offsets[index].reset();
            clear_inventory_stack_coordinates(state, index);
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
            state.inventory_vbr_relative[index] = false;
            state.inventory_fixed_storage_reference[index] = false;
        }
    };
    switch (instruction.kind) {
    case katana::sh4::InstructionKind::Nop:
    case katana::sh4::InstructionKind::Ocbp:
    case katana::sh4::InstructionKind::Ocbwb:
    case katana::sh4::InstructionKind::Rts:
        return;
    case katana::sh4::InstructionKind::MovImmediate:
        set_value(state[instruction.destination_register],
                  static_cast<std::uint32_t>(instruction.immediate));
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = false;
        state.inventory_stack_may_alias[instruction.destination_register] = false;
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    case katana::sh4::InstructionKind::MovRegister:
        state[instruction.destination_register] = state[instruction.source_register];
        state.stack_offsets[instruction.destination_register] =
            state.stack_offsets[instruction.source_register];
        state.inventory_stack_offsets[instruction.destination_register] =
            state.inventory_stack_offsets[instruction.source_register];
        state.inventory_stack_offset_candidates
            [instruction.destination_register] =
                state.inventory_stack_offset_candidates
                    [instruction.source_register];
        state.stack_may_alias[instruction.destination_register] =
            state.stack_may_alias[instruction.source_register];
        state.inventory_stack_may_alias[instruction.destination_register] =
            state.inventory_stack_may_alias[instruction.source_register];
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_source_vbr_relative;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            incoming_source_fixed_storage_reference;
        if (instruction.destination_register == 15u &&
            instruction.source_register != 15u &&
            !state.stack_offsets[15u].has_value() &&
            inventory_stack_coordinates(state, 15u).empty())
            begin_fresh_inventory_stack_epoch(state);
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        adjust_stack_offset(state, instruction.destination_register, instruction.immediate);
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            incoming_destination_fixed_storage_reference;
        if (instruction.destination_register == 15u &&
            !state.stack_offsets[15u].has_value() &&
            inventory_stack_coordinates(state, 15u).empty())
            begin_unresolved_inventory_stack_epoch();
        return;
    case katana::sh4::InstructionKind::AddRegister:
    case katana::sh4::InstructionKind::SubRegister:
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister: {
        std::optional<std::int32_t> adjusted_stack_offset;
        const auto& source = state[instruction.source_register];
        const bool stack_arithmetic =
            instruction.kind == katana::sh4::InstructionKind::AddRegister ||
            instruction.kind == katana::sh4::InstructionKind::SubRegister;
        if (stack_arithmetic &&
            state.stack_offsets[instruction.destination_register].has_value() &&
            !state.stack_offsets[instruction.source_register].has_value() &&
            !state.stack_may_alias[instruction.source_register] &&
            !state.inventory_stack_may_alias[instruction.source_register] &&
            source.known && source.values.size() == 1u &&
            source.complete) {
            // Complete singleton deltas are valid for ordinary stack
            // reasoning.
            auto delta = static_cast<std::int64_t>(source.values.front());
            if (delta > std::numeric_limits<std::int32_t>::max())
                delta -= (std::int64_t{1} << 32u);
            if (instruction.kind ==
                katana::sh4::InstructionKind::SubRegister)
                delta = -delta;
            const auto adjusted =
                static_cast<std::int64_t>(
                    *state.stack_offsets[instruction.destination_register]) +
                delta;
            if (adjusted >= -maximum_stack_distance &&
                adjusted <= maximum_stack_distance)
                adjusted_stack_offset =
                    static_cast<std::int32_t>(adjusted);
        }
        std::vector<std::int32_t> adjusted_inventory_stack_coordinates;
        const auto destination_inventory_coordinates =
            inventory_stack_coordinates(
                state, instruction.destination_register);
        const auto source_inventory_coordinates =
            inventory_stack_coordinates(
                state, instruction.source_register);
        const auto destination_value =
            state[instruction.destination_register];
        bool adjusted_coordinates_valid = true;
        const auto append_adjusted_coordinates =
            [&](const std::span<const std::int32_t> base_coordinates,
                const AbstractValue& displacement_value,
                const bool subtract_displacement) {
                if (!displacement_value.known ||
                    displacement_value.values.empty() ||
                    displacement_value.values.size() >
                        maximum_summary_values)
                    return;
                for (const auto base_coordinate : base_coordinates) {
                    for (const auto raw_displacement :
                         displacement_value.values) {
                        auto displacement =
                            static_cast<std::int64_t>(raw_displacement);
                        if (displacement >
                            std::numeric_limits<std::int32_t>::max())
                            displacement -= (std::int64_t{1} << 32u);
                        if (subtract_displacement)
                            displacement = -displacement;
                        const auto adjusted =
                            static_cast<std::int64_t>(base_coordinate) +
                            displacement;
                        if (!insert_inventory_stack_coordinate(
                                adjusted_inventory_stack_coordinates,
                                adjusted)) {
                            adjusted_inventory_stack_coordinates.clear();
                            adjusted_coordinates_valid = false;
                            if (std::any_of(
                                    state.stack_values.begin(),
                                    state.stack_values.end(),
                                    [](const auto& stored) {
                                        return has_finite_inventory_candidate_values(
                                            stored.second);
                                    }))
                                state.inventory_unresolved_stack_callback_loss =
                                    true;
                            return;
                        }
                    }
                }
            };
        if (stack_arithmetic &&
            !destination_inventory_coordinates.empty() &&
            source_inventory_coordinates.empty() &&
            !state.stack_may_alias[instruction.source_register] &&
            !state.inventory_stack_may_alias
                [instruction.source_register]) {
            append_adjusted_coordinates(
                destination_inventory_coordinates,
                source,
                instruction.kind ==
                    katana::sh4::InstructionKind::SubRegister);
        } else if (
            instruction.kind ==
                katana::sh4::InstructionKind::AddRegister &&
            destination_inventory_coordinates.empty() &&
            !source_inventory_coordinates.empty() &&
            !state.stack_may_alias[instruction.destination_register] &&
            !state.inventory_stack_may_alias
                [instruction.destination_register]) {
            append_adjusted_coordinates(
                source_inventory_coordinates, destination_value, false);
        }
        if (!adjusted_coordinates_valid)
            adjusted_inventory_stack_coordinates.clear();
        state.stack_may_alias[instruction.destination_register] =
            state.stack_may_alias[instruction.destination_register] ||
            state.stack_may_alias[instruction.source_register];
        state.inventory_stack_may_alias[instruction.destination_register] =
            state.inventory_stack_may_alias[instruction.destination_register] ||
            state.inventory_stack_may_alias[instruction.source_register];
        apply_binary(state[instruction.destination_register],
                     state[instruction.source_register],
                     instruction.kind);
        state.stack_offsets[instruction.destination_register] =
            adjusted_stack_offset;
        static_cast<void>(set_inventory_stack_coordinates(
            state,
            instruction.destination_register,
            std::move(adjusted_inventory_stack_coordinates)));
        if (instruction.destination_register == 15u &&
            !state.stack_offsets[15u].has_value() &&
            inventory_stack_coordinates(state, 15u).empty())
            begin_unresolved_inventory_stack_epoch();
        return;
    }
    case katana::sh4::InstructionKind::AndImmediate:
    case katana::sh4::InstructionKind::OrImmediate:
    case katana::sh4::InstructionKind::XorImmediate: {
        const auto immediate = static_cast<std::uint32_t>(instruction.immediate);
        if (immediate == 0u &&
            (instruction.kind ==
                 katana::sh4::InstructionKind::OrImmediate ||
             instruction.kind ==
                 katana::sh4::InstructionKind::XorImmediate)) {
            // These are exact identities. Preserve stack coordinates and all
            // value-scoped inventory provenance, including a suspended epoch.
            state.inventory_vbr_relative[0u] =
                incoming_destination_vbr_relative;
            state.inventory_fixed_storage_reference[0u] =
                incoming_destination_fixed_storage_reference;
            return;
        }
        if (immediate == 0u &&
            instruction.kind ==
                katana::sh4::InstructionKind::AndImmediate) {
            // Unlike the other zero-immediate forms, AND #0 is an exact
            // overwrite even when the incoming value is unknown.
            set_value(state[0u], 0u);
            state.stack_offsets[0u].reset();
            clear_inventory_stack_coordinates(state, 0u);
            state.stack_may_alias[0u] = false;
            state.inventory_stack_may_alias[0u] = false;
            return;
        }
        apply_unary(state[0u], [&](const std::uint32_t value) {
            return instruction.kind == katana::sh4::InstructionKind::AndImmediate
                       ? value & immediate
                   : instruction.kind == katana::sh4::InstructionKind::OrImmediate
                       ? value | immediate
                       : value ^ immediate;
        });
        invalidate_transformed_stack_coordinate(0u);
        return;
    }
    case katana::sh4::InstructionKind::ShiftLogicalLeftOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 1u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftTwo:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 2u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftEight:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 8u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftSixteen:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 16u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 1u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightTwo:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 2u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightEight:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 8u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightSixteen:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 16u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftArithmeticLeftOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 1u; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ShiftArithmeticRightOne:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> 1);
        });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ExtendUnsignedByte:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value & 0xFFu; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ExtendUnsignedWord:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value & 0xFFFFu; });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ExtendSignedByte:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
        });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::ExtendSignedWord:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
        });
        invalidate_transformed_stack_coordinate(
            instruction.destination_register);
        return;
    case katana::sh4::InstructionKind::MoveT:
        set_values(
            state[instruction.destination_register],
            {0u, 1u});
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = false;
        state.inventory_stack_may_alias[instruction.destination_register] = false;
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_value(state[0u],
                  ((line.address + 4u) & ~3u) +
                      static_cast<std::uint32_t>(instruction.displacement));
        state.stack_offsets[0u].reset();
        clear_inventory_stack_coordinates(state, 0u);
        state.stack_may_alias[0u] = false;
        state.inventory_stack_may_alias[0u] = false;
        state.inventory_fixed_storage_reference[0u] = true;
        return;
    case katana::sh4::InstructionKind::MovWordLoadPcRelative:
    case katana::sh4::InstructionKind::MovLongLoadPcRelative: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovWordLoadPcRelative ? 2u : 4u;
        const auto base = width == 4u ? (line.address + 4u) & ~3u : line.address + 4u;
        load_memory_values(state[instruction.destination_register],
                           state,
                           {base + static_cast<std::uint32_t>(instruction.displacement)},
                           width,
                           image);
        auto& loaded = state[instruction.destination_register];
        loaded.inventory_pc_relative_code_literal_values.clear();
        loaded.inventory_pc_relative_code_literal_values_truncated = false;
        if (width == 4u && loaded.known &&
            loaded.values.size() <= maximum_summary_values) {
            for (const auto candidate : loaded.values) {
                if (validate_decode_candidate(image, candidate).valid())
                    loaded.inventory_pc_relative_code_literal_values.push_back(candidate);
            }
        }
        synchronize_inventory_provenance(loaded);
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] =
            !state[instruction.destination_register].known;
        state.inventory_stack_may_alias[instruction.destination_register] =
            !state[instruction.destination_register].known;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            width == 4u && state[instruction.destination_register].known &&
            !state[instruction.destination_register].values.empty() &&
            state[instruction.destination_register].values.size() <=
                maximum_summary_values;
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
    case katana::sh4::InstructionKind::MovByteStore:
    case katana::sh4::InstructionKind::MovWordStore:
    case katana::sh4::InstructionKind::MovLongStore: {
        const auto width = instruction.kind == katana::sh4::InstructionKind::MovByteStore   ? 1u
                           : instruction.kind == katana::sh4::InstructionKind::MovWordStore ? 2u
                                                                                            : 4u;
        const auto offset = stack_slot(state, instruction.destination_register);
        const auto inventory_offsets =
            inventory_stack_slots(state, instruction.destination_register);
        const auto stored_value = value_with_saved_stack_epoch(
            state, instruction.source_register, width);
        note_unresolved_stack_candidate_store(
            state,
            instruction.destination_register,
            width,
            line.address,
            stored_value);
        store_stack_value(state,
                          offset,
                          state.stack_may_alias
                                  [instruction.destination_register] &&
                              inventory_offsets.empty(),
                          preserve_guarded_stack_inventory,
                          width,
                          stored_value);
        store_inventory_stack_value(state,
                                    offset,
                                    inventory_offsets,
                                    width,
                                    stored_value);
        if (!offset.has_value()) {
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                width,
                                stored_value,
                                state[instruction.destination_register]);
        }
        return;
    }
    case katana::sh4::InstructionKind::MovByteStorePreDecrement:
    case katana::sh4::InstructionKind::MovWordStorePreDecrement:
    case katana::sh4::InstructionKind::MovLongStorePreDecrement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteStorePreDecrement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordStorePreDecrement ? 2u
                                                                                         : 4u;
        adjust_stack_offset(
            state, instruction.destination_register, -static_cast<std::int32_t>(width));
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            incoming_destination_fixed_storage_reference;
        const auto offset = stack_slot(state, instruction.destination_register);
        const auto inventory_offsets =
            inventory_stack_slots(state, instruction.destination_register);
        const auto stored_value = value_with_saved_stack_epoch(
            state, instruction.source_register, width);
        note_unresolved_stack_candidate_store(
            state,
            instruction.destination_register,
            width,
            line.address,
            stored_value);
        store_stack_value(state,
                          offset,
                          state.stack_may_alias
                                  [instruction.destination_register] &&
                              inventory_offsets.empty(),
                          preserve_guarded_stack_inventory,
                          width,
                          stored_value);
        store_inventory_stack_value(state,
                                    offset,
                                    inventory_offsets,
                                    width,
                                    stored_value);
        if (!offset.has_value()) {
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                width,
                                stored_value,
                                state[instruction.destination_register]);
        }
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreDisplacement:
    case katana::sh4::InstructionKind::MovWordStoreDisplacement:
    case katana::sh4::InstructionKind::MovLongStoreDisplacement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteStoreDisplacement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordStoreDisplacement ? 2u
                                                                                         : 4u;
        const auto offset =
            stack_slot(state, instruction.destination_register, instruction.displacement);
        const auto inventory_offsets =
            inventory_stack_slots(state,
                                  instruction.destination_register,
                                  instruction.displacement);
        const auto stored_value = value_with_saved_stack_epoch(
            state, instruction.source_register, width);
        note_unresolved_stack_candidate_store(
            state,
            instruction.destination_register,
            width,
            line.address,
            stored_value);
        store_stack_value(state,
                          offset,
                          state.stack_may_alias
                                  [instruction.destination_register] &&
                              inventory_offsets.empty(),
                          preserve_guarded_stack_inventory,
                          width,
                          stored_value);
        store_inventory_stack_value(state,
                                    offset,
                                    inventory_offsets,
                                    width,
                                    stored_value);
        if (!offset.has_value()) {
            store_memory_values(
                state,
                displaced_addresses(state[instruction.destination_register],
                                    static_cast<std::uint32_t>(instruction.displacement)),
                width,
                stored_value,
                state[instruction.destination_register]);
        }
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoad:
    case katana::sh4::InstructionKind::MovWordLoad:
    case katana::sh4::InstructionKind::MovLongLoad: {
        const auto width = instruction.kind == katana::sh4::InstructionKind::MovByteLoad   ? 1u
                           : instruction.kind == katana::sh4::InstructionKind::MovWordLoad ? 2u
                                                                                           : 4u;
        const bool may_read_unresolved_stack_callback =
            memory_address_may_reference_unresolved_stack_callback(
                state,
                std::array<std::uint8_t, 1u>{
                    instruction.source_register},
                width);
        const auto memory_addresses =
            displaced_addresses(
                state[instruction.source_register], 0u);
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(state[instruction.destination_register],
                             state,
                             stack_slot(state, instruction.source_register),
                             width);
        } else if (const auto inventory_offsets =
                       inventory_stack_slots(
                           state, instruction.source_register);
                   !inventory_offsets.empty()) {
            load_inventory_stack_values(
                state[instruction.destination_register],
                state,
                inventory_offsets,
                width);
        } else {
            load_memory_values(state[instruction.destination_register],
                               state,
                               memory_addresses,
                               width,
                               image,
                               &state[instruction.source_register]);
        }
        merge_saved_stack_epoch_memory_forward(
            state[instruction.destination_register],
            state,
            memory_addresses,
            width);
        state[instruction.destination_register]
            .inventory_stack_callback_loss_unresolved =
            state[instruction.destination_register]
                .inventory_stack_callback_loss_unresolved ||
            may_read_unresolved_stack_callback;
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            width == 4u && incoming_source_fixed_storage_reference;
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadPostIncrement:
    case katana::sh4::InstructionKind::MovWordLoadPostIncrement:
    case katana::sh4::InstructionKind::MovLongLoadPostIncrement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadPostIncrement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadPostIncrement ? 2u
                                                                                         : 4u;
        const bool may_read_unresolved_stack_callback =
            memory_address_may_reference_unresolved_stack_callback(
                state,
                std::array<std::uint8_t, 1u>{
                    instruction.source_register},
                width);
        const auto memory_addresses =
            displaced_addresses(
                state[instruction.source_register], 0u);
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(state[instruction.destination_register],
                             state,
                             stack_slot(state, instruction.source_register),
                             width);
        } else if (const auto inventory_offsets =
                       inventory_stack_slots(
                           state, instruction.source_register);
                   !inventory_offsets.empty()) {
            load_inventory_stack_values(
                state[instruction.destination_register],
                state,
                inventory_offsets,
                width);
        } else {
            load_memory_values(state[instruction.destination_register],
                               state,
                               memory_addresses,
                               width,
                               image,
                               &state[instruction.source_register]);
        }
        merge_saved_stack_epoch_memory_forward(
            state[instruction.destination_register],
            state,
            memory_addresses,
            width);
        state[instruction.destination_register]
            .inventory_stack_callback_loss_unresolved =
            state[instruction.destination_register]
                .inventory_stack_callback_loss_unresolved ||
            may_read_unresolved_stack_callback;
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            width == 4u && incoming_source_fixed_storage_reference;
        if (instruction.source_register != instruction.destination_register) {
            adjust_stack_offset(
                state, instruction.source_register, static_cast<std::int32_t>(width));
            state.inventory_vbr_relative[instruction.source_register] =
                incoming_source_vbr_relative;
            state.inventory_fixed_storage_reference[instruction.source_register] =
                incoming_source_fixed_storage_reference;
        }
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadDisplacement:
    case katana::sh4::InstructionKind::MovWordLoadDisplacement:
    case katana::sh4::InstructionKind::MovLongLoadDisplacement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadDisplacement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadDisplacement ? 2u
                                                                                         : 4u;
        const bool may_read_unresolved_stack_callback =
            memory_address_may_reference_unresolved_stack_callback(
                state,
                std::array<std::uint8_t, 1u>{
                    instruction.source_register},
                width);
        const auto memory_addresses =
            displaced_addresses(
                state[instruction.source_register],
                static_cast<std::uint32_t>(
                    instruction.displacement));
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(
                state[instruction.destination_register],
                state,
                stack_slot(state, instruction.source_register, instruction.displacement),
                width);
        } else if (const auto inventory_offsets =
                       inventory_stack_slots(
                           state,
                           instruction.source_register,
                           instruction.displacement);
                   !inventory_offsets.empty()) {
            load_inventory_stack_values(
                state[instruction.destination_register],
                state,
                inventory_offsets,
                width);
        } else {
            load_memory_values(
                state[instruction.destination_register],
                state,
                memory_addresses,
                width,
                image,
                &state[instruction.source_register]);
        }
        merge_saved_stack_epoch_memory_forward(
            state[instruction.destination_register],
            state,
            memory_addresses,
            width);
        state[instruction.destination_register]
            .inventory_stack_callback_loss_unresolved =
            state[instruction.destination_register]
                .inventory_stack_callback_loss_unresolved ||
            may_read_unresolved_stack_callback;
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            width == 4u && incoming_source_fixed_storage_reference;
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreR0Indexed:
    case katana::sh4::InstructionKind::MovWordStoreR0Indexed:
    case katana::sh4::InstructionKind::MovLongStoreR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteStoreR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordStoreR0Indexed ? 2u
                                                                                      : 4u;
        const auto offset = r0_indexed_stack_slot(
            state, instruction.destination_register);
        bool inventory_coordinate_enumeration_failed = false;
        const auto inventory_offsets =
            r0_indexed_inventory_stack_slots(
                state,
                instruction.destination_register,
                &inventory_coordinate_enumeration_failed);
        const auto may_alias_stack =
            state.stack_may_alias[0u] ||
            state.stack_may_alias[instruction.destination_register];
        const auto stored_value = value_with_saved_stack_epoch(
            state, instruction.source_register, width);
        if (inventory_coordinate_enumeration_failed) {
            if (carries_stack_callback_payload(stored_value))
                state.inventory_unresolved_stack_callback_loss =
                    true;
            else
                record_unresolved_stack_candidate(
                    state,
                    width,
                    line.address,
                    stored_value,
                    instruction.destination_register,
                    "candidate-r0-indexed-store");
        }
        note_unresolved_stack_candidate_store(
            state,
            0u,
            width,
            line.address,
            stored_value);
        note_unresolved_stack_candidate_store(
            state,
            instruction.destination_register,
            width,
            line.address,
            stored_value);
        store_stack_value(state,
                          offset,
                          (may_alias_stack ||
                           inventory_coordinate_enumeration_failed) &&
                              inventory_offsets.empty(),
                          preserve_guarded_stack_inventory,
                          width,
                          stored_value);
        store_inventory_stack_value(state,
                                    offset,
                                    inventory_offsets,
                                    width,
                                    stored_value);
        auto evidence = state[0u];
        if (instruction.destination_register == 0u && evidence.known) {
            for (auto& value : evidence.values)
                value += value;
            normalize(evidence.values);
        } else {
            apply_binary(evidence,
                         state[instruction.destination_register],
                         katana::sh4::InstructionKind::AddRegister);
        }
        if (!offset.has_value())
            store_memory_values(
                state,
                indexed_addresses(state[0u],
                                  state[instruction.destination_register],
                                  instruction.destination_register == 0u),
                width,
                stored_value,
                evidence);
        return;
    }
    case katana::sh4::InstructionKind::FmovStore: {
        auto bases =
            inventory_stack_slots(
                state, instruction.destination_register);
        if (const auto semantic_base =
                stack_slot(
                    state, instruction.destination_register);
            semantic_base.has_value())
            bases = {*semantic_base};
        std::vector<std::pair<std::int32_t, std::size_t>>
            alternatives;
        for (const auto base : bases) {
            alternatives.emplace_back(base, 4u);
            alternatives.emplace_back(base, 8u);
        }
        invalidate_stack_write_alternatives(
            state,
            alternatives,
            state.stack_may_alias
                    [instruction.destination_register] ||
                state.inventory_stack_may_alias
                    [instruction.destination_register]);
        invalidate_memory_values_conservatively(state);
        return;
    }
    case katana::sh4::InstructionKind::FmovStorePreDecrement: {
        auto old_bases =
            inventory_stack_slots(
                state, instruction.destination_register);
        if (const auto semantic_base =
                stack_slot(
                    state, instruction.destination_register);
            semantic_base.has_value())
            old_bases = {*semantic_base};
        std::vector<std::pair<std::int32_t, std::size_t>>
            alternatives;
        for (const auto old_base : old_bases) {
            const auto four_byte_base =
                static_cast<std::int64_t>(old_base) - 4;
            const auto eight_byte_base =
                static_cast<std::int64_t>(old_base) - 8;
            if (four_byte_base >= -maximum_stack_distance)
                alternatives.emplace_back(
                    static_cast<std::int32_t>(four_byte_base), 4u);
            if (eight_byte_base >= -maximum_stack_distance)
                alternatives.emplace_back(
                    static_cast<std::int32_t>(eight_byte_base), 8u);
        }
        invalidate_stack_write_alternatives(
            state,
            alternatives,
            state.stack_may_alias
                    [instruction.destination_register] ||
                state.inventory_stack_may_alias
                    [instruction.destination_register]);
        static constexpr std::array<std::int32_t, 2u> decrements{-4, -8};
        branch_inventory_stack_position(
            state, instruction.destination_register, decrements);
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        state.inventory_fixed_storage_reference
            [instruction.destination_register] =
                incoming_destination_fixed_storage_reference;
        invalidate_memory_values_conservatively(state);
        return;
    }
    case katana::sh4::InstructionKind::FmovLoadPostIncrement: {
        static constexpr std::array<std::int32_t, 2u> increments{4, 8};
        branch_inventory_stack_position(
            state, instruction.source_register, increments);
        state.inventory_vbr_relative[instruction.source_register] =
            incoming_source_vbr_relative;
        state.inventory_fixed_storage_reference
            [instruction.source_register] =
                incoming_source_fixed_storage_reference;
        return;
    }
    case katana::sh4::InstructionKind::FmovStoreR0Indexed: {
        bool inventory_coordinate_enumeration_failed = false;
        auto bases =
            r0_indexed_inventory_stack_slots(
                state,
                instruction.destination_register,
                &inventory_coordinate_enumeration_failed);
        if (const auto semantic_base =
                r0_indexed_stack_slot(
                    state, instruction.destination_register);
            semantic_base.has_value())
            bases = {*semantic_base};
        std::vector<std::pair<std::int32_t, std::size_t>>
            alternatives;
        for (const auto base : bases) {
            alternatives.emplace_back(base, 4u);
            alternatives.emplace_back(base, 8u);
        }
        invalidate_stack_write_alternatives(
            state,
            alternatives,
            state.stack_may_alias[0u] ||
                state.stack_may_alias
                    [instruction.destination_register] ||
                state.inventory_stack_may_alias[0u] ||
                state.inventory_stack_may_alias
                    [instruction.destination_register] ||
                inventory_coordinate_enumeration_failed);
        invalidate_memory_values_conservatively(state);
        return;
    }
    case katana::sh4::InstructionKind::MultiplyAccumulateWord:
    case katana::sh4::InstructionKind::MultiplyAccumulateLong: {
        const auto width =
            instruction.kind ==
                    katana::sh4::InstructionKind::MultiplyAccumulateWord
                ? 2
                : 4;
        adjust_stack_offset(
            state, instruction.destination_register, width);
        adjust_stack_offset(
            state, instruction.source_register, width);
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreGbrDisplacement:
    case katana::sh4::InstructionKind::MovWordStoreGbrDisplacement:
    case katana::sh4::InstructionKind::MovLongStoreGbrDisplacement:
    case katana::sh4::InstructionKind::AndByteImmediate:
    case katana::sh4::InstructionKind::XorByteImmediate:
    case katana::sh4::InstructionKind::OrByteImmediate:
    case katana::sh4::InstructionKind::TestAndSetByte:
    case katana::sh4::InstructionKind::Prefetch:
    case katana::sh4::InstructionKind::TrapAlways:
        invalidate_stack_values_conservatively(state);
        invalidate_memory_values_conservatively(state);
        clear_written(state, instruction);
        if ((written_registers & register_bit(15u)) != 0u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    case katana::sh4::InstructionKind::StoreSpecialRegisterPreDecrement: {
        adjust_stack_offset(state, instruction.destination_register, -4);
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        const auto offset = stack_slot(state, instruction.destination_register);
        const auto inventory_offsets =
            inventory_stack_slots(
                state, instruction.destination_register);
        invalidate_stack_range(state,
                               offset,
                               state.stack_may_alias
                                       [instruction.destination_register] &&
                                   inventory_offsets.empty(),
                               preserve_guarded_stack_inventory,
                               4u);
        AbstractValue unknown;
        store_inventory_stack_value(
            state, offset, inventory_offsets, 4u, unknown);
        if (!offset.has_value()) {
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                4u,
                                unknown,
                                state[instruction.destination_register]);
        }
        return;
    }
    case katana::sh4::InstructionKind::LoadSpecialRegisterPostIncrement:
        if (instruction.special_register ==
            katana::sh4::SpecialRegister::Sr) {
            if (instruction.source_register >= 8u) {
                adjust_stack_offset(
                    state, instruction.source_register, 4);
                state.inventory_vbr_relative
                    [instruction.source_register] =
                        incoming_source_vbr_relative;
                state.inventory_fixed_storage_reference
                    [instruction.source_register] =
                        incoming_source_fixed_storage_reference;
            }
            invalidate_banked_general_registers();
        } else {
            adjust_stack_offset(
                state, instruction.source_register, 4);
            state.inventory_vbr_relative
                [instruction.source_register] =
                    incoming_source_vbr_relative;
            state.inventory_fixed_storage_reference
                [instruction.source_register] =
                    incoming_source_fixed_storage_reference;
        }
        return;
    case katana::sh4::InstructionKind::StoreSpecialRegister:
        clear_written(state, instruction);
        if (instruction.special_register == katana::sh4::SpecialRegister::Vbr) {
            // STC VBR,Rn does not reveal a finite address, but it does establish
            // that Rn is a vector-base value rather than a caller stack alias.
            // Keep ordinary memory reasoning conservative while allowing the
            // inventory-only observer to retain a proven callback subsequently
            // stored through a VBR-relative address.
            state.inventory_stack_may_alias[instruction.destination_register] = false;
            state.inventory_vbr_relative[instruction.destination_register] = true;
        }
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    case katana::sh4::InstructionKind::MovByteLoadR0Indexed:
    case katana::sh4::InstructionKind::MovWordLoadR0Indexed:
    case katana::sh4::InstructionKind::MovLongLoadR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadR0Indexed ? 2u
                                                                                      : 4u;
        bool may_read_unresolved_stack_callback =
            memory_address_may_reference_unresolved_stack_callback(
                state,
                std::array<std::uint8_t, 2u>{
                    0u, instruction.source_register},
                width);
        const auto offset =
            r0_indexed_stack_slot(
                state, instruction.source_register);
        bool inventory_coordinate_enumeration_failed = false;
        const auto inventory_offsets =
            r0_indexed_inventory_stack_slots(
                state,
                instruction.source_register,
                &inventory_coordinate_enumeration_failed);
        const auto memory_addresses =
            indexed_addresses(
                state[0u],
                state[instruction.source_register],
                instruction.source_register == 0u);
        if (inventory_coordinate_enumeration_failed) {
            const auto lost_candidate = std::find_if(
                state.stack_values.begin(),
                state.stack_values.end(),
                [](const auto& stored) {
                    return has_finite_inventory_candidate_values(
                               stored.second) ||
                           carries_stack_callback_payload(
                               stored.second);
                });
            if (lost_candidate != state.stack_values.end()) {
                if (carries_stack_callback_payload(
                        lost_candidate->second))
                    state.inventory_unresolved_stack_callback_loss =
                        true;
                else
                    record_unresolved_stack_candidate(
                        state,
                        width,
                        line.address,
                        lost_candidate->second,
                        instruction.source_register,
                        "candidate-r0-indexed-load");
            }
            may_read_unresolved_stack_callback =
                may_read_unresolved_stack_callback ||
                memory_address_may_reference_unresolved_stack_callback(
                    state,
                    std::array<std::uint8_t, 2u>{
                        0u, instruction.source_register},
                    width);
        }
        auto evidence = state[0u];
        evidence.guarded = evidence.guarded || state[instruction.source_register].guarded;
        evidence.complete = evidence.complete && state[instruction.source_register].complete;
        evidence.contextual_candidate_dependency =
            state[0u].contextual_candidate_dependency ||
            state[instruction.source_register].contextual_candidate_dependency;
        evidence.call_sites.insert(state[instruction.source_register].call_sites.begin(),
                                   state[instruction.source_register].call_sites.end());
        evidence.callees.insert(state[instruction.source_register].callees.begin(),
                                state[instruction.source_register].callees.end());
        if (offset.has_value()) {
            load_stack_value(state[instruction.destination_register],
                             state,
                             offset,
                             width);
        } else if (!inventory_offsets.empty()) {
            load_inventory_stack_values(
                state[instruction.destination_register],
                state,
                inventory_offsets,
                width);
        } else {
            load_memory_values(
                state[instruction.destination_register],
                state,
                memory_addresses,
                width,
                image,
                &evidence);
        }
        merge_saved_stack_epoch_memory_forward(
            state[instruction.destination_register],
            state,
            memory_addresses,
            width);
        state[instruction.destination_register]
            .inventory_stack_callback_loss_unresolved =
            state[instruction.destination_register]
                .inventory_stack_callback_loss_unresolved ||
            may_read_unresolved_stack_callback;
        state.stack_offsets[instruction.destination_register].reset();
        clear_inventory_stack_coordinates(
            state, instruction.destination_register);
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        state.inventory_fixed_storage_reference[instruction.destination_register] =
            width == 4u &&
            (incoming_r0_fixed_storage_reference ||
             incoming_source_fixed_storage_reference);
        if (instruction.destination_register == 15u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
    default:
        if (instruction.kind == katana::sh4::InstructionKind::Unknown) {
            invalidate_stack_values_conservatively(state);
            invalidate_memory_values_conservatively(state);
        }
        clear_written(state, instruction);
        if ((written_registers & register_bit(15u)) != 0u)
            begin_fresh_inventory_stack_epoch(state);
        return;
    }
}

const FunctionRegisterValueSummary* register_summary(const FunctionValueSummary& summary,
                                                      const std::uint8_t register_index) {
    const auto found = std::find_if(summary.registers.begin(),
                                    summary.registers.end(),
                                    [register_index](const auto& candidate) {
                                        return candidate.register_index == register_index;
                                    });
    return found == summary.registers.end() ? nullptr : &*found;
}

void emit_abi_stack_projection_diagnostic(
    const std::uint32_t owner,
    const std::uint32_t call_site,
    const std::uint32_t callee,
    const std::size_t projected_slots,
    const std::optional<std::int32_t> first_rejected_relative_slot,
    const AbiStackArgumentReadSet* const required_stack_reads,
    const std::optional<std::int32_t> caller_sp,
    const std::vector<std::int32_t>& caller_inventory_sp_coordinates,
    const std::map<std::int32_t, AbstractValue>& caller_stack_values) {
    emit_bounded_analyzer_diagnostic(
        0u,
        owner,
        owner,
        0u,
        0u,
        [&] {
            std::fprintf(
                stderr,
                "KATANA_ANALYZER_PROJECTION_TRUNCATION owner=0x%08X "
                "callsite=0x%08X callee=0x%08X projected=%zu "
                "rejected_known=%u rejected_relative=%d "
                "readset_present=%u readset_complete=%u readset_count=%zu "
                "readset=[",
                static_cast<unsigned int>(owner),
                static_cast<unsigned int>(call_site),
                static_cast<unsigned int>(callee),
                projected_slots,
                static_cast<unsigned int>(
                    first_rejected_relative_slot.has_value()),
                static_cast<int>(
                    first_rejected_relative_slot.value_or(0)),
                static_cast<unsigned int>(required_stack_reads != nullptr),
                static_cast<unsigned int>(
                    required_stack_reads != nullptr &&
                    required_stack_reads->complete),
                required_stack_reads == nullptr
                    ? 0u
                    : required_stack_reads->slots.size());
            if (required_stack_reads != nullptr) {
                for (std::size_t index = 0u;
                     index < required_stack_reads->slots.size();
                     ++index)
                    std::fprintf(stderr,
                                 "%s%d",
                                 index == 0u ? "" : ",",
                                 static_cast<int>(
                                     required_stack_reads->slots[index]));
            }
            std::fprintf(
                stderr,
                "] sp_known=%u sp=%d sp_coord_count=%zu sp_coords=[",
                static_cast<unsigned int>(caller_sp.has_value()),
                static_cast<int>(caller_sp.value_or(0)),
                caller_inventory_sp_coordinates.size());
            for (std::size_t index = 0u;
                 index < caller_inventory_sp_coordinates.size();
                 ++index)
                std::fprintf(
                    stderr,
                    "%s%d",
                    index == 0u ? "" : ",",
                    static_cast<int>(
                        caller_inventory_sp_coordinates[index]));
            std::fprintf(stderr,
                         "] stack_slot_count=%zu stack_slots=[",
                         caller_stack_values.size());
            auto index = std::size_t{0u};
            for (const auto& [slot, value] : caller_stack_values) {
                static_cast<void>(value);
                std::fprintf(stderr,
                             "%s%d",
                             index++ == 0u ? "" : ",",
                             static_cast<int>(slot));
            }
            std::fprintf(
                stderr,
                "] top_chain_count=%zu top_chain_truncated=%u top_chain=[",
                required_stack_reads == nullptr
                    ? 0u
                    : required_stack_reads->top_chain.size(),
                static_cast<unsigned int>(
                    required_stack_reads != nullptr &&
                    required_stack_reads->top_chain_truncated));
            if (required_stack_reads != nullptr) {
                for (std::size_t frame_index = 0u;
                     frame_index < required_stack_reads->top_chain.size();
                     ++frame_index) {
                    const auto& frame =
                        required_stack_reads->top_chain[frame_index];
                    std::fprintf(
                        stderr,
                        "%s{owner=0x%08X,site=0x%08X,reason=%s,"
                        "target=0x%08X,contract_present=%u,"
                        "contract_complete=%u,ingress_present=%u,"
                        "ingress_guarded=%u,ingress_complete=%u,"
                        "residual_indirect=%u,external_successor=%u}",
                        frame_index == 0u ? "" : ",",
                        static_cast<unsigned int>(frame.owner),
                        static_cast<unsigned int>(frame.site),
                        abi_stack_read_top_reason_name(frame.reason),
                        static_cast<unsigned int>(frame.target),
                        static_cast<unsigned int>(frame.contract_present),
                        static_cast<unsigned int>(frame.contract_complete),
                        static_cast<unsigned int>(frame.ingress_present),
                        static_cast<unsigned int>(frame.ingress_guarded),
                        static_cast<unsigned int>(frame.ingress_complete),
                        static_cast<unsigned int>(frame.residual_indirect),
                        static_cast<unsigned int>(frame.external_successor));
                }
            }
            std::fprintf(stderr, "]\n");
        });
}

void emit_abi_stack_projection_root_diagnostic(
    const std::uint32_t resolution_owner) {
    emit_bounded_analyzer_diagnostic(
        3u,
        resolution_owner,
        resolution_owner,
        0u,
        0u,
        [&] {
            std::fprintf(
                stderr,
                "KATANA_ANALYZER_PROJECTION_ROOT owner=0x%08X\n",
                static_cast<unsigned int>(resolution_owner));
        });
}

AbstractState make_callee_abi_input(
    const AbstractState& caller,
    const std::uint32_t owner,
    const std::uint32_t call_site,
    const std::uint32_t callee,
    GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics,
    const AbiStackArgumentReadSet* const required_stack_reads = nullptr) {
    // Construct the projected ABI state directly. Copying the complete caller
    // and immediately clearing stack_values used to deep-copy every stack
    // payload on every call edge. Stack coordinates and selected stack values
    // are rebuilt below, so carrying either collection through that copy has
    // no semantic purpose.
    AbstractState input;
    input.registers = caller.registers;
    input.stack_may_alias = caller.stack_may_alias;
    input.inventory_stack_may_alias =
        caller.inventory_stack_may_alias;
    input.inventory_vbr_relative = caller.inventory_vbr_relative;
    input.inventory_fixed_storage_reference =
        caller.inventory_fixed_storage_reference;
    // Register and memory facts remain deliberately conservative here:
    // callees and the return/persistent-store fixpoints can observe them.
    input.memory_values = caller.memory_values;
    input.inventory_unresolved_saved_stack_alias_sources =
        caller.inventory_unresolved_saved_stack_alias_sources;
    input.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        caller
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
    input.inventory_current_stack_epoch_alias_watcher =
        caller.inventory_current_stack_epoch_alias_watcher;
    input.inventory_detached_stack_epoch_alias_watcher =
        caller.inventory_detached_stack_epoch_alias_watcher;
    input.inventory_unresolved_stack_callback_loss =
        caller.inventory_unresolved_stack_callback_loss;
    input.inventory_stack_callback_loss_identity_truncated =
        caller.inventory_stack_callback_loss_identity_truncated;
    const auto caller_sp = caller.stack_offsets[15u];
    auto caller_inventory_sp_coordinates =
        inventory_stack_slots(caller, 15u);
    if (caller_inventory_sp_coordinates.empty() &&
        caller_sp.has_value())
        caller_inventory_sp_coordinates = {*caller_sp};
    const bool caller_has_stack_inventory_candidates =
        std::any_of(
            caller.stack_values.begin(),
            caller.stack_values.end(),
            [](const auto& stored) {
                return has_finite_inventory_candidate_values(
                           stored.second) ||
                       carries_stack_callback_payload(
                           stored.second);
            });
    const bool complete_stack_readset =
        required_stack_reads != nullptr &&
        required_stack_reads->complete;
    const auto retain_current_saved_alias_watcher = [&] {
        for (const auto& [slot, value] : caller.stack_values) {
            static_cast<void>(slot);
            if (has_saved_stack_epoch(value) &&
                value.inventory_saved_stack_epoch
                    .tracks_current_epoch)
                input.inventory_current_stack_epoch_alias_watcher =
                    true;
        }
    };
    if (complete_stack_readset)
        retain_current_saved_alias_watcher();
    for (std::uint8_t index = 0u; index < 15u; ++index) {
        const auto register_offset = caller.stack_offsets[index];
        if (!register_offset.has_value() || !caller_sp.has_value()) {
            input.stack_offsets[index].reset();
        } else {
            const auto relative =
                static_cast<std::int64_t>(*register_offset) -
                static_cast<std::int64_t>(*caller_sp);
            if (relative < -maximum_stack_distance ||
                relative > maximum_stack_distance)
                input.stack_offsets[index].reset();
            else
                input.stack_offsets[index] =
                    static_cast<std::int32_t>(relative);
        }
        const auto register_inventory_coordinates =
            inventory_stack_slots(caller, index);
        std::vector<std::int32_t> relative_inventory_coordinates;
        if (!register_inventory_coordinates.empty() &&
            !caller_inventory_sp_coordinates.empty()) {
            bool relative_coordinates_valid = true;
            for (const auto register_coordinate :
                 register_inventory_coordinates) {
                for (const auto sp_coordinate :
                     caller_inventory_sp_coordinates) {
                    const auto relative =
                        static_cast<std::int64_t>(
                            register_coordinate) -
                        static_cast<std::int64_t>(
                            sp_coordinate);
                    if (!insert_inventory_stack_coordinate(
                            relative_inventory_coordinates, relative)) {
                        relative_inventory_coordinates.clear();
                        relative_coordinates_valid = false;
                        if (caller_has_stack_inventory_candidates)
                            input.inventory_unresolved_stack_callback_loss =
                                true;
                        break;
                    }
                }
                if (!relative_coordinates_valid)
                    break;
            }
        }
        static_cast<void>(set_inventory_stack_coordinates(
            input,
            index,
            std::move(relative_inventory_coordinates)));
    }
    input.stack_offsets[15u] = 0;
    static_cast<void>(set_inventory_stack_coordinates(
        input, 15u, std::vector<std::int32_t>{0}));
    if (caller_inventory_sp_coordinates.empty()) {
        const auto may_read_stack =
            required_stack_reads == nullptr ||
            !required_stack_reads->complete ||
            !required_stack_reads->slots.empty();
        if (may_read_stack && caller_has_stack_inventory_candidates)
            input.inventory_unresolved_stack_callback_loss = true;
        if (may_read_stack) {
            for (const auto& [slot, value] : caller.stack_values) {
                static_cast<void>(slot);
                if (!has_latent_saved_stack_alias(value)) continue;
                add_unresolved_saved_stack_alias(
                    input,
                    unresolved_saved_stack_alias_source_stack,
                    value.inventory_saved_stack_epoch
                        .tracks_current_epoch);
            }
        }
        return input;
    }

    std::size_t projected_slots = 0u;
    bool projection_truncated = false;
    std::optional<std::int32_t> first_rejected_relative_slot;
    const auto project_value =
        [&](const std::int32_t relative_slot,
            const AbstractValue& source) {
            const auto existing =
                input.stack_values.find(relative_slot);
            if (existing == input.stack_values.end() &&
                projected_slots >= maximum_abi_stack_argument_slots) {
                projection_truncated = true;
                if (!first_rejected_relative_slot.has_value())
                    first_rejected_relative_slot = relative_slot;
                return;
            }
            auto value = source;
            value.call_sites.insert(call_site);
            if (!caller_sp.has_value() ||
                caller_inventory_sp_coordinates.size() > 1u) {
                make_unknown_preserving_provenance(value);
                value.guarded = true;
                value.complete = false;
            }
            const auto [stored, inserted] =
                input.stack_values.try_emplace(
                    relative_slot, value);
            if (!inserted) {
                static_cast<void>(
                    merge_value(stored->second, value));
                stored->second.guarded = true;
                stored->second.complete = false;
            } else {
                ++projected_slots;
            }
        };
    const auto report_projection_truncation = [&] {
        if (!projection_truncated) return;
        if (walk_diagnostics != nullptr)
            walk_diagnostics
                ->abi_stack_argument_projection_truncated_functions = 1u;
        emit_abi_stack_projection_diagnostic(
            owner,
            call_site,
            callee,
            projected_slots,
            first_rejected_relative_slot,
            required_stack_reads,
            caller_sp,
            caller_inventory_sp_coordinates,
            caller.stack_values);
    };
    if (required_stack_reads != nullptr &&
        required_stack_reads->complete) {
        for (const auto relative_slot : required_stack_reads->slots) {
            for (const auto projection_base :
                 caller_inventory_sp_coordinates) {
                const auto source_slot =
                    static_cast<std::int64_t>(projection_base) +
                    static_cast<std::int64_t>(relative_slot);
                if (source_slot < -maximum_stack_distance ||
                    source_slot > maximum_stack_distance)
                    continue;
                const auto source = caller.stack_values.find(
                    static_cast<std::int32_t>(source_slot));
                if (source == caller.stack_values.end()) continue;
                project_value(relative_slot, source->second);
                if (projection_truncated) break;
            }
            if (projection_truncated) break;
        }
        report_projection_truncation();
        return input;
    }

    for (const auto& [slot, source] : caller.stack_values) {
        auto projected_source = source;
        if (has_latent_saved_stack_alias(projected_source)) {
            add_unresolved_saved_stack_alias(
                input,
                unresolved_saved_stack_alias_source_stack,
                projected_source.inventory_saved_stack_epoch
                    .tracks_current_epoch);
            projected_source.inventory_saved_stack_epoch = {};
        }
        if (carries_unresolved_stack_callback(projected_source)) {
            // With an incomplete ABI readset the callee may consume any
            // outgoing slot. Exact identities of pure loss markers therefore
            // form an unbounded recursive ladder but add no precision over
            // the existing state-wide fail-closed top.
            input.inventory_unresolved_stack_callback_loss = true;
            projected_source
                .inventory_stack_callback_loss_unresolved = false;
            if (projected_source.inventory_saved_stack_epoch.slots.empty())
                projected_source.inventory_saved_stack_epoch = {};
        }
        if (!has_non_epoch_abstract_fact(projected_source) &&
            !has_saved_stack_epoch(projected_source))
            continue;
        if (!caller_sp.has_value() &&
            !has_finite_inventory_candidate_values(projected_source) &&
            !carries_stack_callback_payload(projected_source))
            continue;
        for (const auto projection_base :
             caller_inventory_sp_coordinates) {
            const auto relative =
                static_cast<std::int64_t>(slot) -
                static_cast<std::int64_t>(projection_base);
            // Only outgoing, word-aligned caller slots are part of the callee
            // ABI. Negative offsets are caller locals/spills and must never
            // become a callee argument by accident.
            if (relative < 0 || relative > maximum_stack_distance ||
                (relative & 3) != 0)
                continue;
            const auto relative_slot =
                static_cast<std::int32_t>(relative);
            project_value(relative_slot, projected_source);
            if (projection_truncated) break;
        }
        if (projection_truncated) break;
    }
    report_projection_truncation();
    return input;
}

void mark_observed_code_pointer_arguments(
    const katana::io::ExecutableImage& image,
    AbstractState& observation) {
    // This provenance belongs to the value passed in an ABI argument register,
    // not to an address used to load that value.  Loads deliberately do not
    // inherit it from their address operand.
    const auto mark_argument = [&](AbstractValue& value) {
        std::vector<std::uint32_t> candidates;
        if (value.known && value.values.size() <= maximum_summary_values)
            candidates = value.values;
        else
            candidates = value.inventory_pc_relative_code_literal_values;
        std::erase_if(candidates, [&](const auto candidate) {
            return !validate_decode_candidate(image, candidate).valid();
        });
        if (candidates.empty()) return;
        // An ABI boundary is evidence per concrete value. Invalid alternatives
        // such as nullptr remain live scalar alternatives but no longer erase
        // a valid callback from the inventory-only candidate domain.
        mark_inventory_code_pointer_values(value, candidates);
    };
    for (std::uint8_t index = 4u; index <= 7u; ++index)
        mark_argument(observation[index]);
    for (auto& [slot, value] : observation.stack_values) {
        static_cast<void>(slot);
        mark_argument(value);
    }
}

void promote_tail_code_literal_arguments(
    const katana::io::ExecutableImage& image,
    const std::uint32_t transfer_site,
    AbstractState& observation) {
    // A directly loaded PC-relative image literal is substantially narrower
    // evidence than an arbitrary decode-looking object field.  Promote only
    // that provenance when the control-flow inventory has independently
    // established a real tail-call ABI boundary.
    const auto promote_argument = [&](AbstractValue& value) {
        std::vector<std::uint32_t> promoted;
        for (const auto candidate : value.inventory_pc_relative_code_literal_values) {
            if (validate_decode_candidate(image, candidate).valid())
                promoted.push_back(candidate);
        }
        if (promoted.empty()) return;
        mark_inventory_code_pointer_values(value, promoted);
        value.call_sites.insert(transfer_site);
    };
    for (std::uint8_t index = 4u; index <= 7u; ++index)
        promote_argument(observation[index]);
    for (auto& [slot, value] : observation.stack_values) {
        static_cast<void>(slot);
        promote_argument(value);
    }
}

void observe_callee_arguments(
    const katana::io::ExecutableImage& image,
    const AbstractState& state,
    const std::uint32_t owner,
    const std::uint32_t call_site,
    const std::span<const std::uint32_t> candidate_callees,
    const bool candidate_callees_guarded,
    std::vector<FunctionEvaluation::CallArguments>* const call_arguments,
    GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics,
    const AbiStackArgumentReadMap* const abi_stack_argument_reads) {
    if (call_arguments == nullptr || candidate_callees.empty()) return;
    for (const auto candidate : candidate_callees) {
        const AbiStackArgumentReadSet* required_stack_reads = nullptr;
        if (abi_stack_argument_reads != nullptr) {
            if (const auto found =
                    abi_stack_argument_reads->find(candidate);
                found != abi_stack_argument_reads->end())
                required_stack_reads = &found->second;
        }
        auto observation = make_callee_abi_input(
            state,
            owner,
            call_site,
            candidate,
            walk_diagnostics,
            required_stack_reads);
        mark_observed_code_pointer_arguments(image, observation);
        if (candidate_callees_guarded) {
            for (auto& value : observation)
                value.guarded = true;
            for (auto& [slot, value] : observation.stack_values) {
                static_cast<void>(slot);
                value.guarded = true;
            }
            for (auto& [address, value] : observation.memory_values) {
                static_cast<void>(address);
                value.guarded = true;
            }
        }
        call_arguments->push_back(
            {call_site, candidate, std::move(observation)});
    }
}
// Contextual candidate summaries are only needed along ABI values that really
// originated at the guarded candidate call. This is deliberately independent
// of code-pointer provenance and cannot create an inventory entry by itself.
void mark_contextual_candidate_abi_arguments(
    AbstractState& state,
    const std::uint16_t entry_register_reads,
    const AbiStackArgumentReadSet* const entry_stack_reads) {
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        if ((entry_register_reads & register_bit(index)) != 0u)
            state[index].contextual_candidate_dependency = true;
    }
    for (auto& [slot, value] : state.stack_values) {
        if (entry_stack_reads == nullptr || !entry_stack_reads->complete ||
            std::binary_search(entry_stack_reads->slots.begin(),
                               entry_stack_reads->slots.end(),
                               slot))
            value.contextual_candidate_dependency = true;
    }
}

[[nodiscard]] bool has_contextual_candidate_abi_argument(
    const AbstractState& state,
    const std::uint16_t entry_register_reads,
    const AbiStackArgumentReadSet* const entry_stack_reads) {
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        if ((entry_register_reads & register_bit(index)) != 0u &&
            (state[index].contextual_candidate_dependency ||
             carries_stack_callback_payload(state[index]) ||
             has_latent_saved_stack_alias(state[index])))
            return true;
    }
    const bool may_read_stack =
        entry_stack_reads == nullptr ||
        !entry_stack_reads->complete ||
        !entry_stack_reads->slots.empty();
    if (state.inventory_current_stack_epoch_alias_watcher ||
        state.inventory_detached_stack_epoch_alias_watcher)
        return true;
    if (may_read_stack &&
        (state.inventory_unresolved_stack_callback_loss ||
         (state.inventory_unresolved_saved_stack_alias_sources &
          unresolved_saved_stack_alias_source_stack) != 0u))
        return true;
    for (const auto& [slot, value] : state.stack_values) {
        if ((entry_stack_reads == nullptr || !entry_stack_reads->complete ||
             std::binary_search(entry_stack_reads->slots.begin(),
                                entry_stack_reads->slots.end(),
                                slot)) &&
            (value.contextual_candidate_dependency ||
             carries_stack_callback_payload(value) ||
             has_latent_saved_stack_alias(value)))
            return true;
    }
    return false;
}

void observe_inventory_transfers(
    const katana::io::ExecutableImage& image,
    const AbstractState& state,
    const std::uint32_t owner,
    const std::uint32_t transfer_site,
    const std::span<const std::uint32_t> candidate_callees,
    const bool guarded,
    const bool complete,
    const bool requires_code_pointer,
    const bool observes_abi_arguments,
    std::vector<FunctionEvaluation::InventoryTransfer>* const transfers,
    GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics,
    const AbiStackArgumentReadMap* const abi_stack_argument_reads) {
    if (transfers == nullptr || candidate_callees.empty()) return;
    for (const auto candidate_callee : candidate_callees) {
        const AbiStackArgumentReadSet* required_stack_reads = nullptr;
        if (abi_stack_argument_reads != nullptr) {
            if (const auto found =
                    abi_stack_argument_reads->find(candidate_callee);
                found != abi_stack_argument_reads->end())
                required_stack_reads = &found->second;
        }
        auto observation = make_callee_abi_input(
            state,
            owner,
            transfer_site,
            candidate_callee,
            walk_diagnostics,
            required_stack_reads);
        if (observes_abi_arguments)
            promote_tail_code_literal_arguments(
                image, transfer_site, observation);
        bool found_code_pointer = false;
        bool found_unresolved_stack_callback =
            observation.inventory_unresolved_stack_callback_loss;
        const auto observe_code_pointer = [&](const AbstractValue& value) {
            found_code_pointer = std::any_of(
                value.inventory_code_pointer_values.begin(),
                value.inventory_code_pointer_values.end(),
                [&](const auto candidate) {
                    return validate_decode_candidate(image, candidate).valid();
                }) || found_code_pointer;
            found_unresolved_stack_callback =
                found_unresolved_stack_callback ||
                carries_stack_callback_payload(value);
        };
        for (const auto& value : observation)
            observe_code_pointer(value);
        for (const auto& [offset, value] : observation.stack_values) {
            static_cast<void>(offset);
            observe_code_pointer(value);
        }
        for (auto& [address, value] : observation.memory_values) {
            static_cast<void>(address);
            observe_code_pointer(value);
        }
        if (requires_code_pointer && !found_code_pointer &&
            !found_unresolved_stack_callback)
            continue;
        if (guarded || !complete) {
            for (auto& value : observation) {
                if (!value.known) continue;
                value.guarded = true;
                value.complete = value.complete && complete;
            }
            for (auto& [offset, value] : observation.stack_values) {
                static_cast<void>(offset);
                value.guarded = true;
                value.complete = value.complete && complete;
            }
            for (auto& [address, value] : observation.memory_values) {
                static_cast<void>(address);
                value.guarded = true;
                value.complete = value.complete && complete;
            }
        }
        transfers->push_back({transfer_site,
                              candidate_callee,
                              std::move(observation),
                              guarded,
                              complete});
    }
}

void apply_call(AbstractState& state,
                const katana::io::ExecutableImage& image,
                const std::uint32_t owner,
                const std::uint32_t call_site,
                const std::optional<std::uint32_t> callee,
                const std::span<const std::uint32_t> candidate_callees,
                const bool candidate_callees_guarded,
                const bool candidate_callees_complete,
                const std::map<std::uint32_t, FunctionValueSummary>& summaries,
                std::vector<FunctionEvaluation::CallArguments>* call_arguments,
                const bool preserve_guarded_stack_inventory = false,
                const std::map<std::uint32_t, FunctionValueSummary>*
                    contextual_summaries = nullptr,
                GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics = nullptr,
                const AbiStackArgumentReadMap* const
                    abi_stack_argument_reads = nullptr) {
    observe_callee_arguments(image,
                             state,
                             owner,
                             call_site,
                             candidate_callees,
                             candidate_callees_guarded,
                             call_arguments,
                             walk_diagnostics,
                             abi_stack_argument_reads);
    std::map<std::uint32_t, AbstractValue>
        preserved_saved_stack_memory;
    for (const auto& [address, value] : state.memory_values) {
        if (!has_saved_stack_epoch(value) &&
            !carries_unresolved_stack_callback(value))
            continue;
        auto preserved = value;
        make_unknown_preserving_provenance(preserved);
        preserved.guarded = true;
        preserved.complete = false;
        preserved_saved_stack_memory.emplace(
            address, std::move(preserved));
    }
    const auto discard_stack_values = [&] {
        for (const auto& [slot, value] : state.stack_values) {
            static_cast<void>(slot);
            if (carries_stack_callback_payload(value)) {
                state.inventory_unresolved_stack_callback_loss =
                    true;
            } else if (has_latent_saved_stack_alias(value)) {
                add_unresolved_saved_stack_alias(
                    state,
                    unresolved_saved_stack_alias_source_stack,
                    value.inventory_saved_stack_epoch
                        .tracks_current_epoch);
            }
        }
        state.stack_values.clear();
    };
    if (image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC) {
        for (std::size_t index = 0u; index < state.size(); ++index) {
            make_unknown(state[index]);
            state.stack_offsets[index].reset();
            clear_inventory_stack_coordinates(
                state, static_cast<std::uint8_t>(index));
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
            state.inventory_vbr_relative[index] = false;
            state.inventory_fixed_storage_reference[index] = false;
        }
        state[15u].inventory_stack_derived = true;
        discard_stack_values();
        state.memory_values =
            std::move(preserved_saved_stack_memory);
        return;
    }
    const bool escaped_stack_alias =
        std::any_of(state.stack_may_alias.begin(),
                    state.stack_may_alias.begin() + 8,
                    [](const bool may_alias) { return may_alias; });
    if (escaped_stack_alias) {
        if (preserve_guarded_stack_inventory) {
            for (auto& [offset, value] : state.stack_values) {
                static_cast<void>(offset);
                value.guarded = true;
                value.complete = false;
            }
        } else {
            discard_stack_values();
        }
    }
    state.memory_values = preserved_saved_stack_memory;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        make_unknown(state[index]);
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.stack_offsets[index].reset();
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        clear_inventory_stack_coordinates(state, index);
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.stack_may_alias[index] = true;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.inventory_stack_may_alias[index] = true;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.inventory_vbr_relative[index] = false;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.inventory_fixed_storage_reference[index] = false;
    make_unknown(state[15u]);
    state[15u].inventory_stack_derived = true;
    state.inventory_vbr_relative[15u] = false;
    state.inventory_fixed_storage_reference[15u] = false;
    std::vector<std::uint32_t> callees;
    if (callee.has_value())
        callees.push_back(*callee);
    else
        callees.assign(candidate_callees.begin(), candidate_callees.end());
    if (callees.empty()) return;
    normalize(callees);
    std::vector<std::uint32_t> returned_values;
    std::set<std::uint32_t> evidence_callees;
    bool returned_guarded = candidate_callees_guarded;
    bool returned_complete = candidate_callees_complete;
    // Ordinary stack reasoning must include the unknown members of an
    // incomplete callee family.  The separate inventory-only bit below may
    // still retain finite non-stack values from the known summaries; it is
    // consumed exclusively by the two guarded inventory observers.
    bool returned_may_alias_stack = !candidate_callees_complete;
    bool returned_inventory_may_alias_stack = false;
    std::vector<std::uint32_t> returned_inventory_code_pointer_values;
    std::vector<std::uint32_t> returned_inventory_pc_relative_code_literal_values;
    bool returned_inventory_code_pointer_values_truncated = false;
    bool returned_inventory_pc_relative_code_literal_values_truncated = false;
    bool returned_contextual_candidate_dependency = false;
    bool returned_stack_callback_loss_unresolved = false;
    bool returned_saved_stack_alias_latent = false;
    bool returned_saved_stack_alias_tracks_current_epoch = false;
    std::uint8_t returned_unresolved_saved_stack_alias_sources = 0u;
    bool returned_unresolved_saved_stack_alias_tracks_current_epoch =
        false;
    bool returned_unresolved_stack_callback_loss = false;
    bool returned_stack_callback_loss_identity_truncated = false;
    bool returned_memory_complete = candidate_callees_complete;
    bool returned_memory_initialized = false;
    std::map<std::uint32_t, AbstractValue> returned_memory;
    std::set<std::uint32_t>
        returned_memory_stack_callback_loss_unresolved;
    std::map<std::uint32_t, bool>
        returned_memory_saved_stack_alias_latent;
    for (const auto candidate : callees) {
        const FunctionValueSummary* summary = nullptr;
        if (contextual_summaries != nullptr) {
            const auto contextual = contextual_summaries->find(candidate);
            if (contextual != contextual_summaries->end())
                summary = &contextual->second;
        }
        if (summary == nullptr) {
            const auto global = summaries.find(candidate);
            if (global != summaries.end()) summary = &global->second;
        }
        if (summary == nullptr) {
            returned_complete = false;
            returned_may_alias_stack = true;
            returned_memory_complete = false;
            continue;
        }
        returned_unresolved_saved_stack_alias_sources =
            static_cast<std::uint8_t>(
                returned_unresolved_saved_stack_alias_sources |
                summary
                    ->inventory_unresolved_saved_stack_alias_sources);
        returned_unresolved_saved_stack_alias_tracks_current_epoch =
            returned_unresolved_saved_stack_alias_tracks_current_epoch ||
            summary
                ->inventory_unresolved_saved_stack_alias_tracks_current_epoch;
        returned_unresolved_stack_callback_loss =
            returned_unresolved_stack_callback_loss ||
            summary->inventory_unresolved_stack_callback_loss;
        returned_stack_callback_loss_identity_truncated =
            returned_stack_callback_loss_identity_truncated ||
            summary
                ->inventory_stack_callback_loss_identity_truncated;
        for (const auto& memory : summary->memory_values) {
            if (memory.inventory_saved_stack_alias_latent) {
                const auto existing =
                    returned_memory_saved_stack_alias_latent.find(
                        memory.address);
                if (existing ==
                        returned_memory_saved_stack_alias_latent.end() &&
                    returned_memory_saved_stack_alias_latent.size() >=
                        maximum_memory_values) {
                    returned_unresolved_saved_stack_alias_sources =
                        static_cast<std::uint8_t>(
                            returned_unresolved_saved_stack_alias_sources |
                            unresolved_saved_stack_alias_source_memory);
                    returned_unresolved_saved_stack_alias_tracks_current_epoch =
                        returned_unresolved_saved_stack_alias_tracks_current_epoch ||
                        memory
                            .inventory_saved_stack_alias_tracks_current_epoch;
                } else {
                    auto& tracks_current =
                        returned_memory_saved_stack_alias_latent[
                            memory.address];
                    tracks_current =
                        tracks_current ||
                        memory
                            .inventory_saved_stack_alias_tracks_current_epoch;
                }
            }
            if (memory
                    .inventory_stack_callback_loss_unresolved) {
                if (!returned_memory_stack_callback_loss_unresolved
                         .contains(memory.address) &&
                    returned_memory_stack_callback_loss_unresolved
                            .size() >= maximum_memory_values) {
                    if (walk_diagnostics != nullptr)
                        walk_diagnostics
                            ->inventory_candidate_values_truncated =
                            true;
                } else {
                    returned_memory_stack_callback_loss_unresolved
                        .insert(memory.address);
                }
            }
        }
        if (!summary->memory_complete) {
            returned_memory_complete = false;
        } else {
            std::map<std::uint32_t, AbstractValue> candidate_memory;
            for (const auto& memory : summary->memory_values) {
                AbstractValue value;
                value.known = !memory.values.empty();
                value.guarded = memory.guarded || candidate_callees_guarded;
                value.complete = memory.complete;
                value.values = memory.values;
                value.inventory_stack_callback_loss_unresolved =
                    memory
                        .inventory_stack_callback_loss_unresolved;
                if (memory.inventory_saved_stack_alias_latent) {
                    value.inventory_saved_stack_epoch.present = true;
                    value.inventory_saved_stack_epoch.unresolved = true;
                    value.inventory_saved_stack_epoch
                        .tracks_current_epoch =
                        memory
                            .inventory_saved_stack_alias_tracks_current_epoch;
                }
                value.call_sites = {call_site};
                value.callees = {candidate};
                if (value.known ||
                    value
                        .inventory_stack_callback_loss_unresolved ||
                    has_saved_stack_epoch(value))
                    candidate_memory.emplace(
                        memory.address, std::move(value));
            }
            if (!returned_memory_initialized) {
                returned_memory = std::move(candidate_memory);
                returned_memory_initialized = true;
            } else {
                for (auto value = returned_memory.begin(); value != returned_memory.end();) {
                    const auto candidate_value = candidate_memory.find(value->first);
                    if (candidate_value == candidate_memory.end()) {
                        value = returned_memory.erase(value);
                        continue;
                    }
                    merge_value(value->second, candidate_value->second);
                    if (!value->second.known &&
                        !has_saved_stack_epoch(value->second) &&
                        !carries_unresolved_stack_callback(
                            value->second))
                        value = returned_memory.erase(value);
                    else
                        ++value;
                }
            }
        }
        const auto* returned = register_summary(*summary, 0u);
        if (returned == nullptr) {
            returned_complete = false;
            returned_may_alias_stack = true;
            continue;
        }
        returned_stack_callback_loss_unresolved =
            returned_stack_callback_loss_unresolved ||
            returned
                ->inventory_stack_callback_loss_unresolved;
        returned_saved_stack_alias_latent =
            returned_saved_stack_alias_latent ||
            returned->inventory_saved_stack_alias_latent;
        returned_saved_stack_alias_tracks_current_epoch =
            returned_saved_stack_alias_tracks_current_epoch ||
            returned
                ->inventory_saved_stack_alias_tracks_current_epoch;
        returned_inventory_code_pointer_values.insert(
            returned_inventory_code_pointer_values.end(),
            returned->inventory_code_pointer_values.begin(),
            returned->inventory_code_pointer_values.end());
        returned_inventory_pc_relative_code_literal_values.insert(
            returned_inventory_pc_relative_code_literal_values.end(),
            returned->inventory_pc_relative_code_literal_values.begin(),
            returned->inventory_pc_relative_code_literal_values.end());
        returned_inventory_code_pointer_values_truncated =
            returned_inventory_code_pointer_values_truncated ||
            returned->inventory_code_pointer_values_truncated;
        returned_inventory_pc_relative_code_literal_values_truncated =
            returned_inventory_pc_relative_code_literal_values_truncated ||
            returned->inventory_pc_relative_code_literal_values_truncated;
        if (returned->values.empty()) {
            returned_complete = false;
            returned_may_alias_stack = true;
            continue;
        }
        if (!returned->complete) {
            returned_complete = false;
            returned_may_alias_stack = true;
        }
        returned_values.insert(
            returned_values.end(), returned->values.begin(), returned->values.end());
        returned_guarded = returned_guarded || returned->guarded || !returned->complete;
        returned_may_alias_stack =
            returned_may_alias_stack || returned->may_alias_stack;
        returned_inventory_may_alias_stack =
            returned_inventory_may_alias_stack || returned->may_alias_stack;
        returned_contextual_candidate_dependency =
            returned_contextual_candidate_dependency || returned->contextual_candidate_dependency;
        evidence_callees.insert(candidate);
        evidence_callees.insert(returned->evidence_callees.begin(),
                                returned->evidence_callees.end());
    }
    if (returned_memory_complete && returned_memory_initialized) {
        for (const auto& [address, value] :
             preserved_saved_stack_memory) {
            if (!returned_memory.contains(address))
                returned_memory.emplace(address, value);
        }
        state.memory_values = std::move(returned_memory);
    }
    add_unresolved_saved_stack_alias(
        state,
        returned_unresolved_saved_stack_alias_sources,
        returned_unresolved_saved_stack_alias_tracks_current_epoch);
    state.inventory_unresolved_stack_callback_loss =
        state.inventory_unresolved_stack_callback_loss ||
        returned_unresolved_stack_callback_loss;
    state.inventory_stack_callback_loss_identity_truncated =
        state.inventory_stack_callback_loss_identity_truncated ||
        returned_stack_callback_loss_identity_truncated;
    if (returned_stack_callback_loss_identity_truncated &&
        walk_diagnostics != nullptr)
        walk_diagnostics->inventory_candidate_values_truncated =
            true;
    for (const auto address :
         returned_memory_stack_callback_loss_unresolved) {
        if (!state.memory_values.contains(address) &&
            state.memory_values.size() >= maximum_memory_values) {
            if (walk_diagnostics != nullptr)
                walk_diagnostics
                    ->inventory_candidate_values_truncated = true;
            state
                .inventory_stack_callback_loss_identity_truncated =
                true;
            continue;
        }
        AbstractValue unresolved;
        unresolved.guarded = true;
        unresolved.complete = false;
        unresolved.inventory_stack_callback_loss_unresolved =
            true;
        const auto [stored, inserted] =
            state.memory_values.try_emplace(
                address, unresolved);
        if (!inserted)
            static_cast<void>(
                merge_value(stored->second, unresolved));
    }
    for (const auto& [address, tracks_current_epoch] :
         returned_memory_saved_stack_alias_latent) {
        if (!state.memory_values.contains(address) &&
            state.memory_values.size() >= maximum_memory_values) {
            add_unresolved_saved_stack_alias(
                state,
                unresolved_saved_stack_alias_source_memory,
                tracks_current_epoch);
            continue;
        }
        AbstractValue latent;
        latent.guarded = true;
        latent.complete = false;
        latent.inventory_saved_stack_epoch.present = true;
        latent.inventory_saved_stack_epoch.unresolved = true;
        latent.inventory_saved_stack_epoch.tracks_current_epoch =
            tracks_current_epoch;
        const auto [stored, inserted] =
            state.memory_values.try_emplace(
                address, latent);
        if (!inserted)
            static_cast<void>(
                merge_value(stored->second, latent));
    }
    normalize(returned_values);
    const bool finite_return =
        !returned_values.empty() &&
        returned_values.size() <= maximum_summary_values;
    if (!finite_return &&
        returned_inventory_code_pointer_values.empty() &&
        returned_inventory_pc_relative_code_literal_values.empty() &&
        !returned_inventory_code_pointer_values_truncated &&
        !returned_inventory_pc_relative_code_literal_values_truncated &&
        !returned_stack_callback_loss_unresolved &&
        !returned_saved_stack_alias_latent)
        return;
    if (finite_return) {
        state[0u].known = true;
        state[0u].values = std::move(returned_values);
    }
    state[0u].guarded = returned_guarded || !returned_complete ||
                        !finite_return;
    state[0u].complete = returned_complete && finite_return;
    state[0u].call_sites = {call_site};
    state[0u].callees = std::move(evidence_callees);
    state[0u].inventory_code_pointer_values =
        std::move(returned_inventory_code_pointer_values);
    state[0u].inventory_pc_relative_code_literal_values =
        std::move(returned_inventory_pc_relative_code_literal_values);
    state[0u].inventory_code_pointer_values_truncated =
        returned_inventory_code_pointer_values_truncated;
    state[0u].inventory_pc_relative_code_literal_values_truncated =
        returned_inventory_pc_relative_code_literal_values_truncated;
    synchronize_inventory_provenance(state[0u]);
    state[0u].contextual_candidate_dependency = returned_contextual_candidate_dependency;
    state[0u].inventory_stack_callback_loss_unresolved =
        returned_stack_callback_loss_unresolved;
    if (returned_saved_stack_alias_latent) {
        state[0u].inventory_saved_stack_epoch.present = true;
        state[0u].inventory_saved_stack_epoch.unresolved = true;
        state[0u].inventory_saved_stack_epoch
            .tracks_current_epoch =
            returned_saved_stack_alias_tracks_current_epoch;
    }
    state.stack_may_alias[0u] = returned_may_alias_stack;
    state.inventory_stack_may_alias[0u] =
        returned_inventory_may_alias_stack;
    state.inventory_vbr_relative[0u] = false;
    state.inventory_fixed_storage_reference[0u] = false;
}

const katana::sh4::DisassemblyLine& controlling_line(const BasicBlock& block) {
    const auto last = block.lines.size() - 1u;
    return block.lines[last].is_delay_slot && last > 0u &&
                   block.lines[last - 1u].instruction.has_delay_slot &&
                   block.lines[last].address == block.lines[last - 1u].address + 2u
               ? block.lines[last - 1u]
               : block.lines[last];
}

std::vector<std::uint32_t> checked_targets(const katana::io::ExecutableImage& image,
                                           const katana::sh4::DisassemblyLine& line,
                                           const AbstractValue& value) {
    std::vector<std::uint32_t> targets;
    for (const auto candidate : value.values) {
        std::uint32_t target = candidate;
        if (line.instruction.kind == katana::sh4::InstructionKind::Braf ||
            line.instruction.kind == katana::sh4::InstructionKind::Bsrf) {
            target += line.address + 4u;
        }
        const auto validation = validate_decode_candidate(image, target);
        if (!validation.valid()) return {};
        targets.push_back(validation.resolved_address);
    }
    normalize(targets);
    return targets;
}

std::vector<std::uint32_t>
checked_inventory_targets(const katana::io::ExecutableImage& image,
                          const katana::sh4::DisassemblyLine& line,
                          const AbstractValue& value) {
    // Inventory code-pointer values are absolute, decode-validated addresses.
    // BRAF/BSRF consume a relative displacement, which needs a distinct
    // provenance domain before it can participate in this fallback.
    if (line.instruction.kind == katana::sh4::InstructionKind::Braf ||
        line.instruction.kind == katana::sh4::InstructionKind::Bsrf)
        return {};
    std::vector<std::uint32_t> targets;
    for (const auto candidate : value.inventory_code_pointer_values) {
        const auto validation = validate_decode_candidate(image, candidate);
        if (validation.valid())
            targets.push_back(validation.resolved_address);
    }
    normalize(targets);
    return targets;
}

void observe_stored_code_addresses(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& line,
    const AbstractState& state,
    const bool allow_forwarded_unknown_object_store,
    GuardedCodeInventoryCollector& guarded_inventory_collector,
    std::vector<StoredCodeAddressCandidate>& candidates,
    GuardedCodeInventoryWalkDiagnostics* const
        walk_diagnostics) {
    using K = katana::sh4::InstructionKind;
    const auto& instruction = line.instruction;
    bool supported = false;
    bool stack_based = false;
    bool stack_derived = false;
    bool vbr_relative_destination = false;
    bool destination_proven_non_stack = false;
    bool fixed_storage_destination = false;
    std::vector<std::uint32_t> effective_destinations;
    std::set<std::uint32_t> evidence_call_sites;
    std::set<std::uint32_t> evidence_callees;
    const auto include_provenance = [&](const AbstractValue& evidence) {
        evidence_call_sites.insert(evidence.call_sites.begin(), evidence.call_sites.end());
        evidence_callees.insert(evidence.callees.begin(), evidence.callees.end());
    };
    const auto finite_effective_address = [&](std::vector<std::uint32_t> addresses) {
        return !addresses.empty() &&
               addresses.size() <= maximum_summary_values;
    };
    const auto finite_value = [](const AbstractValue& value) {
        return value.known && !value.values.empty() &&
               value.values.size() <= maximum_summary_values;
    };
    switch (instruction.kind) {
    case K::MovLongStore:
        supported = true;
        vbr_relative_destination =
            state.inventory_vbr_relative[instruction.destination_register];
        destination_proven_non_stack =
            !state.inventory_stack_may_alias[instruction.destination_register];
        fixed_storage_destination =
            state.inventory_fixed_storage_reference[instruction.destination_register];
        effective_destinations = displaced_addresses(
            state[instruction.destination_register], 0u);
        stack_based =
            !vbr_relative_destination &&
            state.inventory_stack_may_alias[instruction.destination_register] &&
            !finite_effective_address(effective_destinations);
        stack_derived =
            !vbr_relative_destination &&
            state[instruction.destination_register].inventory_stack_derived &&
            !finite_effective_address(effective_destinations);
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStorePreDecrement:
        supported = true;
        vbr_relative_destination =
            state.inventory_vbr_relative[instruction.destination_register];
        destination_proven_non_stack =
            !state.inventory_stack_may_alias[instruction.destination_register];
        fixed_storage_destination =
            state.inventory_fixed_storage_reference[instruction.destination_register];
        effective_destinations = displaced_addresses(
            state[instruction.destination_register],
            static_cast<std::uint32_t>(-4));
        stack_based =
            !vbr_relative_destination &&
            state.inventory_stack_may_alias[instruction.destination_register] &&
            !finite_effective_address(effective_destinations);
        stack_derived =
            !vbr_relative_destination &&
            state[instruction.destination_register].inventory_stack_derived &&
            !finite_effective_address(effective_destinations);
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStoreDisplacement:
        supported = true;
        vbr_relative_destination =
            state.inventory_vbr_relative[instruction.destination_register];
        destination_proven_non_stack =
            !state.inventory_stack_may_alias[instruction.destination_register];
        fixed_storage_destination =
            state.inventory_fixed_storage_reference[instruction.destination_register];
        effective_destinations = displaced_addresses(
            state[instruction.destination_register],
            static_cast<std::uint32_t>(instruction.displacement));
        stack_based =
            !vbr_relative_destination &&
            state.inventory_stack_may_alias[instruction.destination_register] &&
            !finite_effective_address(effective_destinations);
        stack_derived =
            !vbr_relative_destination &&
            state[instruction.destination_register].inventory_stack_derived &&
            !finite_effective_address(effective_destinations);
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStoreR0Indexed:
        supported = true;
        if (instruction.destination_register != 0u) {
            const auto r0_vbr_relative =
                state.inventory_vbr_relative[0u];
            const auto base_vbr_relative =
                state.inventory_vbr_relative[instruction.destination_register];
            vbr_relative_destination =
                (r0_vbr_relative && !base_vbr_relative &&
                 finite_value(state[instruction.destination_register])) ||
                (base_vbr_relative && !r0_vbr_relative &&
                 finite_value(state[0u]));
        }
        destination_proven_non_stack =
            !state.inventory_stack_may_alias[0u] &&
            !state.inventory_stack_may_alias
                [instruction.destination_register];
        effective_destinations =
            indexed_addresses(state[0u],
                              state[instruction.destination_register],
                              instruction.destination_register == 0u);
        stack_based =
            !vbr_relative_destination &&
            (state.inventory_stack_may_alias[0u] ||
             state.inventory_stack_may_alias
                 [instruction.destination_register]) &&
            !finite_effective_address(effective_destinations);
        stack_derived =
            !vbr_relative_destination &&
            (state[0u].inventory_stack_derived ||
             state[instruction.destination_register].inventory_stack_derived) &&
            !finite_effective_address(effective_destinations);
        include_provenance(state[0u]);
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStoreGbrDisplacement:
        supported = true;
        break;
    default:
        break;
    }
    if (!supported) return;
    const auto& value = state[instruction.source_register];
    const bool forwarded_code_pointer_store =
        allow_forwarded_unknown_object_store && !stack_derived &&
        !value.inventory_code_pointer_values.empty();
    // Fixed PC-relative storage does not prove the loaded pointer is non-stack.
    // It only allows a direct PC-relative callback literal to be inventoried as
    // GuardedPartial; no table scan or fixed CFG edge is created from this path.
    const bool fixed_storage_literal_store =
        fixed_storage_destination && !stack_derived &&
        !value.inventory_pc_relative_code_literal_values.empty();
    if (stack_based && !forwarded_code_pointer_store && !fixed_storage_literal_store) return;

    include_provenance(value);
    const bool finite_resolved_non_stack_destination =
        !vbr_relative_destination && destination_proven_non_stack &&
        finite_effective_address(effective_destinations) &&
        std::ranges::all_of(
            effective_destinations,
            [&](const auto address) {
                return image.resolve_segment_address(address, 4u).has_value();
            });
    // A finite physical address is ideal, but a value-widened destination can
    // remain provably outside the caller stack (for example an object argument
    // plus a bounded field offset). It is a guarded inventory sink, never a
    // fixed edge, so retain that narrow non-stack proof as well.
    const bool persistent_destination = finite_resolved_non_stack_destination ||
                                        vbr_relative_destination ||
                                        (destination_proven_non_stack && !stack_derived);
    if (!persistent_destination && !forwarded_code_pointer_store &&
        !fixed_storage_literal_store)
        return;
    if (walk_diagnostics != nullptr &&
        carries_unresolved_stack_callback(value))
        walk_diagnostics->abi_stack_base_unresolved = true;

    auto candidate_values = value.values;
    candidate_values.insert(candidate_values.end(),
                            value.inventory_code_pointer_values.begin(),
                            value.inventory_code_pointer_values.end());
    if (persistent_destination || fixed_storage_literal_store) {
        candidate_values.insert(
            candidate_values.end(),
            value.inventory_pc_relative_code_literal_values.begin(),
            value.inventory_pc_relative_code_literal_values.end());
    }
    normalize(candidate_values);
    if (candidate_values.empty()) return;

    std::vector<std::uint32_t> validated_candidates;
    validated_candidates.reserve(candidate_values.size());
    bool all_candidates_valid = true;
    for (const auto candidate : candidate_values) {
        const auto validation = validate_decode_candidate(image, candidate);
        const bool direct_candidate =
            has_inventory_code_pointer_value(value, candidate) ||
            // A direct PC-relative literal is still a bounded guarded AOT
            // candidate when its image slot is writable. The live store/load
            // remains authoritative; completeness must not erase the entry.
             ((persistent_destination || fixed_storage_literal_store) &&
              has_inventory_pc_relative_code_literal_value(value, candidate));
        const bool scan_stored_table =
            finite_resolved_non_stack_destination && finite_value(value) &&
            std::binary_search(value.values.begin(), value.values.end(), candidate);
        bool table_candidate = false;
        if (scan_stored_table) {
            const auto& table =
                guarded_inventory_collector.stored_snapshot_table(
                    image, line.address, candidate);
            if (table.has_value()) {
                table_candidate = true;
                for (const auto& entry : table->entries) {
                    StoredCodeAddressCandidate observation;
                    observation.target_address = entry.target;
                    observation.complete = false;
                    observation.guarded = true;
                    observation.store_instruction_addresses = {line.address};
                    observation.evidence_call_sites.assign(
                        evidence_call_sites.begin(), evidence_call_sites.end());
                    observation.evidence_callees.assign(
                        evidence_callees.begin(), evidence_callees.end());
                    candidates.push_back(std::move(observation));
                }
            }
        }
        if (!direct_candidate) {
            if (!table_candidate && !validation.valid())
                all_candidates_valid = false;
            continue;
        }
        if (!validation.valid()) {
            all_candidates_valid = false;
            continue;
        }
        validated_candidates.push_back(validation.resolved_address);
    }
    normalize(validated_candidates);
    const bool complete =
        value.known && value.complete && all_candidates_valid &&
        !value.inventory_code_pointer_values_truncated &&
        !value.inventory_pc_relative_code_literal_values_truncated;
    for (const auto candidate : validated_candidates) {
        StoredCodeAddressCandidate observation;
        observation.target_address = candidate;
        observation.complete = complete;
        // A stored value proves only that native code may be needed.  The later
        // live memory load remains authoritative even for an otherwise complete
        // source value.
        observation.guarded = true;
        observation.store_instruction_addresses = {line.address};
        observation.evidence_call_sites.assign(evidence_call_sites.begin(),
                                               evidence_call_sites.end());
        observation.evidence_callees.assign(evidence_callees.begin(), evidence_callees.end());
        candidates.push_back(std::move(observation));
    }
}

void observe_returned_code_address_tables(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& line,
    const AbstractState& state,
    std::vector<ReturnedCodeAddressTableCandidate>& candidates) {
    using K = katana::sh4::InstructionKind;
    const auto static_image_pointer = [&](const AbstractValue& pointer) {
        return pointer.known && !pointer.values.empty() &&
               pointer.values.size() <= maximum_summary_values &&
               std::ranges::all_of(pointer.values, [&](const auto address) {
                   return image.resolve_segment_address(address, 4u).has_value();
               });
    };
    AbstractValue effective_address;
    switch (line.instruction.kind) {
    case K::MovLongLoad:
    case K::MovLongLoadPostIncrement: {
        const auto base_register = line.instruction.source_register;
        if (state.inventory_stack_may_alias[base_register] &&
            !static_image_pointer(state[base_register]))
            return;
        effective_address = state[base_register];
        break;
    }
    case K::MovLongLoadDisplacement: {
        const auto base_register = line.instruction.source_register;
        if (state.inventory_stack_may_alias[base_register] &&
            !static_image_pointer(state[base_register]))
            return;
        effective_address = state[base_register];
        if (effective_address.known) {
            effective_address.values =
                displaced_addresses(effective_address,
                                    static_cast<std::uint32_t>(
                                        line.instruction.displacement));
        }
        break;
    }
    case K::MovLongLoadR0Indexed: {
        const auto base_register = line.instruction.source_register;
        const auto& index = state[0u];
        const auto& base = state[base_register];
        effective_address.known = index.known && base.known;
        effective_address.guarded = index.guarded || base.guarded;
        effective_address.complete = index.complete && base.complete;
        effective_address.call_sites = index.call_sites;
        effective_address.call_sites.insert(base.call_sites.begin(),
                                            base.call_sites.end());
        effective_address.callees = index.callees;
        effective_address.callees.insert(base.callees.begin(),
                                         base.callees.end());
        effective_address.values = indexed_addresses(index, base, base_register == 0u);
        if ((state.inventory_stack_may_alias[0u] ||
             state.inventory_stack_may_alias[base_register]) &&
            !static_image_pointer(effective_address))
            return;
        break;
    }
    default:
        return;
    }

    if (!effective_address.known || effective_address.values.empty() ||
        effective_address.values.size() > maximum_summary_values ||
        effective_address.call_sites.empty() ||
        effective_address.callees.empty())
        return;

    constexpr detail::SnapshotPointerCandidateScanPolicy scan_policy{
        .minimum_entries = 1u,
        .maximum_scanned_slots = 64u,
        .maximum_skipped_slots = 8u,
        .maximum_consecutive_skipped_slots = 2u,
        .treat_null_as_reserved = true,
        .reject_truncated_scan = false,
    };
    for (const auto table_address : effective_address.values) {
        const auto table = detail::analyze_snapshot_pointer_candidates(
            image,
            line.address,
            table_address,
            JumpTableDispatchKind::Call,
            scan_policy);
        if (!table.has_value()) continue;
        ReturnedCodeAddressTableCandidate candidate;
        candidate.table_address = table->table_address;
        candidate.target_addresses.reserve(table->entries.size());
        for (const auto& entry : table->entries)
            candidate.target_addresses.push_back(entry.target);
        candidate.scan_truncated = table->candidate_scan_truncated;
        candidate.load_instruction_addresses = {line.address};
        candidate.evidence_call_sites.assign(effective_address.call_sites.begin(),
                                             effective_address.call_sites.end());
        candidate.evidence_callees.assign(effective_address.callees.begin(),
                                          effective_address.callees.end());
        candidates.push_back(std::move(candidate));
    }
}

enum class ResolutionCollectionMode : std::uint8_t {
    None,
    Semantic,
    GuardedInventory,
};

FunctionEvaluation evaluate_function(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>& indirect_callees,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>& tail_ingresses,
    const std::map<std::uint32_t, FunctionValueSummary>& summaries,
    const AbstractState& initial_state,
    const ResolutionCollectionMode resolution_mode,
    const bool may_merge_stack_inventory = false,
    GuardedCodeInventoryCollector* const guarded_inventory_collector = nullptr,
    const std::set<std::uint32_t>* const isolated_inventory_call_sites = nullptr,
    const std::map<std::uint32_t, FunctionValueSummary>*
        contextual_summaries = nullptr,
    const TailIngressMap* const local_tail_ingresses = nullptr,
    GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics = nullptr,
    const AbiStackArgumentReadMap* const
        abi_stack_argument_reads = nullptr,
    const std::uint8_t inventory_sink_sources = 0u) {
    FunctionEvaluation evaluation;
    evaluation.summary.function_address = function.entry_address;
    // Sink relevance is now checked against the exact carrying register or
    // stack slot at each transfer. Retain the parameter for call-site API
    // stability, but never reintroduce the former function-wide OR.
    static_cast<void>(inventory_sink_sources);
    if (resolution_mode == ResolutionCollectionMode::None)
        evaluation.call_arguments.reserve(function.direct_callees.size() +
                                          function.tail_jump_targets.size());
    // Final resolution still needs inventory-only ABI observations when a
    // collector is active.  Without them, a locally computed code pointer
    // passed through a candidate-only call cannot enter the bounded forwarded
    // store walk, even though no semantic call edge is being asserted.
    auto* const call_arguments =
        resolution_mode == ResolutionCollectionMode::Semantic &&
                guarded_inventory_collector == nullptr
            ? nullptr
            : &evaluation.call_arguments;
    auto* const inventory_transfers =
        guarded_inventory_collector == nullptr ? nullptr : &evaluation.inventory_transfers;
    constexpr std::size_t observation_compaction_floor = 1'024u;
    std::size_t next_call_argument_compaction =
        observation_compaction_floor;
    std::size_t next_inventory_transfer_compaction =
        observation_compaction_floor;
    std::size_t next_resolution_compaction =
        observation_compaction_floor;
    std::unordered_set<std::uint32_t> members;
    members.reserve(function.block_addresses.size());
    members.insert(function.block_addresses.begin(), function.block_addresses.end());
    std::unordered_map<std::uint32_t, AbstractState> inputs;
    inputs.reserve(function.block_addresses.size());
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> queued;
    queued.reserve(function.block_addresses.size());
    inputs.emplace(function.entry_address, initial_state);
    pending.push_back(function.entry_address);
    queued.insert(function.entry_address);
    // Keep only the monotone joined state per physical RTS. A loop can revisit
    // one return block tens of thousands of times; retaining every transient
    // AbstractState makes both memory and the final summary pass scale with
    // visit history instead of program size.
    std::map<std::uint32_t, AbstractState> returns;
    std::size_t local_fixpoint_iterations = 0u;
    while (!pending.empty()) {
        if (local_fixpoint_iterations >=
            maximum_local_fixpoint_iterations) {
            evaluation.local_fixpoint_budget_exhausted = true;
            break;
        }
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        ++local_fixpoint_iterations;
        const bool sampled_local_iteration =
            local_fixpoint_iterations >= 64u &&
            ((local_fixpoint_iterations &
              (local_fixpoint_iterations - 1u)) == 0u ||
             local_fixpoint_iterations % 65'536u == 0u);
        if (sampled_local_iteration)
            emit_analyzer_fixpoint_trace("local",
                                         local_fixpoint_iterations,
                                         function.entry_address,
                                         address,
                                         pending.size());
        const auto block = blocks.find(address);
        if (block == blocks.end()) continue;
        auto state = inputs.at(address);
        if (walk_diagnostics != nullptr &&
            inventory_candidate_values_truncated(state))
            walk_diagnostics->inventory_candidate_values_truncated = true;
        struct DelayedCall {
            std::uint32_t call_site = 0u;
            std::optional<std::uint32_t> direct_callee;
            std::vector<std::uint32_t> candidate_callees;
            bool candidate_callees_guarded = false;
            bool candidate_callees_complete = false;
        };
        std::optional<DelayedCall> delayed_call;
        struct DelayedTailIngress {
            std::uint32_t transfer_site = 0u;
            std::vector<std::uint32_t> candidate_callees;
            bool candidate_callees_guarded = true;
            bool candidate_callees_complete = false;
            bool requires_code_pointer = false;
            bool observes_abi_arguments = false;
        };
        std::optional<DelayedTailIngress> delayed_tail_ingress;
        for (const auto& line : block->second->lines) {
            const bool indirect = line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
            if (resolution_mode != ResolutionCollectionMode::None && indirect) {
                const auto& value = state[line.instruction.branch_register];
                if (walk_diagnostics != nullptr &&
                    carries_unresolved_stack_callback(value))
                    walk_diagnostics->abi_stack_base_unresolved =
                        true;
                InterproceduralTargetResolution resolution;
                resolution.instruction_address = line.address;
                resolution.register_index = line.instruction.branch_register;
                resolution.call = line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
                if (resolution_mode == ResolutionCollectionMode::Semantic &&
                    value.known && !value.values.empty()) {
                    auto targets = checked_targets(image, line, value);
                    if (!targets.empty()) {
                        resolution.targets = std::move(targets);
                        resolution.call_sites.assign(value.call_sites.begin(),
                                                     value.call_sites.end());
                        resolution.callees.assign(value.callees.begin(), value.callees.end());
                        resolution.guarded = value.guarded || !value.complete;
                        resolution.complete = value.complete;
                        resolution.evidence =
                            value.complete ? (value.guarded ? ControlFlowEvidence::GuardedComplete
                                                            : ControlFlowEvidence::ProvenComplete)
                                           : ControlFlowEvidence::GuardedPartial;
                        resolution.reason =
                            value.guarded ? "guarded-function-memory"
                            : !value.callees.empty()
                                ? (resolution.targets.size() == 1u
                                       ? "interprocedural-return-constant"
                                       : "interprocedural-return-set")
                                : (resolution.targets.size() == 1u ? "function-cfg-constant"
                                                                   : "function-cfg-set");
                    }
                }
                if (resolution.targets.empty() &&
                    !value.inventory_code_pointer_values.empty()) {
                    auto targets = checked_inventory_targets(image, line, value);
                    if (!targets.empty()) {
                        resolution.targets = std::move(targets);
                        resolution.call_sites.assign(value.call_sites.begin(),
                                                     value.call_sites.end());
                        resolution.callees.assign(value.callees.begin(),
                                                  value.callees.end());
                        resolution.guarded = true;
                        resolution.complete = false;
                        resolution.evidence = ControlFlowEvidence::GuardedPartial;
                        resolution.reason = "inventory-code-pointer-set";
                    }
                }
                if (resolution.targets.empty() &&
                    resolution_mode == ResolutionCollectionMode::Semantic) {
                    resolution.guarded = true;
                    resolution.complete = false;
                    resolution.evidence = ControlFlowEvidence::Unresolved;
                    resolution.reason = "context-target-unknown";
                }
                if (!resolution.targets.empty() ||
                    resolution_mode == ResolutionCollectionMode::Semantic)
                    evaluation.resolutions.push_back(std::move(resolution));
            }

            const bool call =
                line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall;
            if (!call && guarded_inventory_collector != nullptr) {
                std::vector<StoredCodeAddressCandidate> stored_candidates;
                observe_stored_code_addresses(
                    image,
                    line,
                    state,
                    may_merge_stack_inventory,
                    *guarded_inventory_collector,
                    stored_candidates,
                    walk_diagnostics);
                if (isolated_inventory_call_sites != nullptr) {
                    for (auto& candidate : stored_candidates) {
                        candidate.complete = false;
                        candidate.guarded = true;
                        candidate.evidence_call_sites.insert(
                            candidate.evidence_call_sites.end(),
                            isolated_inventory_call_sites->begin(),
                            isolated_inventory_call_sites->end());
                    }

                }
                guarded_inventory_collector->collect(
                    std::move(stored_candidates));
                if (isolated_inventory_call_sites == nullptr) {
                    std::vector<ReturnedCodeAddressTableCandidate>
                        returned_tables;
                    observe_returned_code_address_tables(
                        image, line, state, returned_tables);
                    guarded_inventory_collector->collect(
                        std::move(returned_tables));
                }
            }
            if (!call) {
                const auto tail_ingress = find_tail_ingress(
                    tail_ingresses, local_tail_ingresses, line.address);
                if (tail_ingress.has_value() &&
                    tail_ingress->observes_abi_arguments)
                    promote_tail_code_literal_arguments(
                        image, line.address, state);
                apply_transfer(
                    state, line, image, may_merge_stack_inventory);
            }
            if (delayed_call.has_value()) {
                apply_call(state,
                           image,
                           function.entry_address,
                           delayed_call->call_site,
                           delayed_call->direct_callee,
                           delayed_call->candidate_callees,
                           delayed_call->candidate_callees_guarded,
                           delayed_call->candidate_callees_complete,
                           summaries,
                           call_arguments,
                           may_merge_stack_inventory,
                            contextual_summaries,
                            walk_diagnostics,
                            abi_stack_argument_reads);
                delayed_call.reset();
            }
            if (delayed_tail_ingress.has_value()) {
                observe_inventory_transfers(
                    image,
                    state,
                    function.entry_address,
                    delayed_tail_ingress->transfer_site,
                    delayed_tail_ingress->candidate_callees,
                    delayed_tail_ingress->candidate_callees_guarded,
                    delayed_tail_ingress->candidate_callees_complete,
                    delayed_tail_ingress->requires_code_pointer,
                    delayed_tail_ingress->observes_abi_arguments,
                    inventory_transfers,
                    walk_diagnostics,
                    abi_stack_argument_reads);
                delayed_tail_ingress.reset();
            }
            if (call) {
                const auto callee =
                    line.instruction.control_flow == katana::sh4::ControlFlowKind::Call
                        ? line.target_address
                        : std::nullopt;
                std::vector<std::uint32_t> candidate_callees;
                bool candidate_callees_guarded = false;
                bool candidate_callees_complete = false;
                if (callee.has_value()) {
                    candidate_callees.push_back(*callee);
                    candidate_callees_complete = true;
                } else if (const auto found = indirect_callees.find(line.address);
                           found != indirect_callees.end()) {
                    candidate_callees = found->second.targets;
                    candidate_callees_guarded = found->second.guarded;
                    candidate_callees_complete = found->second.complete;
                }
                if (line.instruction.has_delay_slot)
                    delayed_call = DelayedCall{line.address,
                                               callee,
                                               std::move(candidate_callees),
                                               candidate_callees_guarded,
                                               candidate_callees_complete};
                else
                    apply_call(state,
                               image,
                               function.entry_address,
                               line.address,
                               callee,
                               candidate_callees,
                               candidate_callees_guarded,
                               candidate_callees_complete,
                               summaries,
                               call_arguments,
                               may_merge_stack_inventory,
                                contextual_summaries,
                                walk_diagnostics,
                                abi_stack_argument_reads);
            }
            if (!call &&
                (line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::UnconditionalBranch ||
                 line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::IndirectBranch ||
                 line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::ConditionalBranch)) {
                const auto tail_ingress = find_tail_ingress(
                    tail_ingresses, local_tail_ingresses, line.address);
                if (!tail_ingress.has_value()) continue;
                if (line.instruction.has_delay_slot) {
                    delayed_tail_ingress = DelayedTailIngress{
                        line.address,
                        tail_ingress->targets,
                        tail_ingress->guarded,
                        tail_ingress->complete,
                        tail_ingress->requires_code_pointer,
                        tail_ingress->observes_abi_arguments};
                } else {
                    observe_inventory_transfers(image,
                                                state,
                                                function.entry_address,
                                                line.address,
                                                tail_ingress->targets,
                                                tail_ingress->guarded,
                                                tail_ingress->complete,
                                                tail_ingress->requires_code_pointer,
                                                tail_ingress->observes_abi_arguments,
                                                inventory_transfers,
                                                walk_diagnostics,
                                                abi_stack_argument_reads);
                }
            }
            if (walk_diagnostics != nullptr &&
                inventory_candidate_values_truncated(state))
                walk_diagnostics->inventory_candidate_values_truncated = true;
        }
        if (evaluation.call_arguments.size() >=
            next_call_argument_compaction) {
            coalesce_call_arguments(
                evaluation.call_arguments);
            next_call_argument_compaction =
                std::max(
                    observation_compaction_floor,
                    evaluation.call_arguments.size() * 2u);
        }
        if (evaluation.inventory_transfers.size() >=
            next_inventory_transfer_compaction) {
            coalesce_inventory_transfers(
                evaluation.inventory_transfers);
            next_inventory_transfer_compaction =
                std::max(
                    observation_compaction_floor,
                    evaluation.inventory_transfers.size() * 2u);
        }
        if (evaluation.resolutions.size() >=
            next_resolution_compaction) {
            coalesce_resolutions(evaluation.resolutions);
            next_resolution_compaction =
                std::max(
                    observation_compaction_floor,
                    evaluation.resolutions.size() * 2u);
        }
        if (controlling_line(*block->second).instruction.kind ==
            katana::sh4::InstructionKind::Rts) {
            const auto return_site =
                controlling_line(*block->second).address;
            const auto [returned, inserted] =
                returns.try_emplace(return_site, state);
            if (!inserted)
                static_cast<void>(
                    merge_state(returned->second,
                                state,
                                may_merge_stack_inventory));
        }
        for (const auto successor : block->second->successors) {
            if (!members.contains(successor)) continue;
            const auto [input, inserted] = inputs.emplace(successor, state);
            const bool merged =
                !inserted &&
                merge_state(input->second, state, may_merge_stack_inventory);
            if ((inserted || merged) &&
                queued.insert(successor).second)
                pending.push_back(successor);
        }
    }

    if (walk_diagnostics != nullptr) {
        walk_diagnostics->maximum_local_fixpoint_iterations =
            std::max(
                walk_diagnostics->maximum_local_fixpoint_iterations,
                local_fixpoint_iterations);
        if (evaluation.local_fixpoint_budget_exhausted)
            ++walk_diagnostics->local_fixpoint_limited_evaluations;
    }

    // A block can be revisited while its local input converges.  Publish one
    // conservative observation per physical callsite/callee pair; exposing
    // transient visits to the interprocedural worklist lets the same callsite
    // alternately replace its callee input and can keep a recursive graph
    // alive long after the local state has stabilized.
    coalesce_call_arguments(evaluation.call_arguments);
    coalesce_inventory_transfers(
        evaluation.inventory_transfers);
    coalesce_resolutions(evaluation.resolutions);

    for (const auto& [return_site, return_state] : returns) {
        static_cast<void>(return_site);
        evaluation.summary
            .inventory_unresolved_saved_stack_alias_sources =
            static_cast<std::uint8_t>(
                evaluation.summary
                    .inventory_unresolved_saved_stack_alias_sources |
                return_state
                    .inventory_unresolved_saved_stack_alias_sources);
        evaluation.summary
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch =
            evaluation.summary
                    .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
            return_state
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
        evaluation.summary.inventory_unresolved_stack_callback_loss =
            evaluation.summary.inventory_unresolved_stack_callback_loss ||
            return_state.inventory_unresolved_stack_callback_loss;
        evaluation.summary
            .inventory_stack_callback_loss_identity_truncated =
            evaluation.summary
                    .inventory_stack_callback_loss_identity_truncated ||
            return_state
                .inventory_stack_callback_loss_identity_truncated;
    }

    const std::array<std::uint8_t, 8u> summary_registers{0u, 8u, 9u, 10u, 11u, 12u, 13u, 14u};
    for (const auto register_index : summary_registers) {
        FunctionRegisterValueSummary summary;
        summary.register_index = register_index;
        summary.abi_preserved =
            register_index >= 8u && image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC;
        summary.may_alias_stack = returns.empty();
        for (const auto& [return_site, state] : returns) {
            summary.return_sites.push_back(return_site);
            summary.may_alias_stack =
                summary.may_alias_stack || state.stack_may_alias[register_index];
        }
        if (summary.abi_preserved) {
            summary.reason = returns.empty() ? "no-return" : "abi-preserved-input";
            evaluation.summary.registers.push_back(std::move(summary));
            continue;
        }
        bool complete = !returns.empty();
        bool finite = !returns.empty();
        std::set<std::uint32_t> inventory_code_pointer_values;
        std::set<std::uint32_t> inventory_pc_relative_code_literal_values;
        bool inventory_code_pointer_values_truncated = false;
        bool inventory_pc_relative_code_literal_values_truncated = false;
        bool contextual_candidate_dependency = false;
        bool inventory_stack_callback_loss_unresolved = false;
        bool inventory_saved_stack_alias_latent = false;
        bool inventory_saved_stack_alias_tracks_current_epoch = false;
        std::set<std::uint32_t> values;
        std::set<std::uint32_t> evidence;
        for (const auto& [return_site, state] : returns) {
            static_cast<void>(return_site);
            const auto& value = state[register_index];
            inventory_stack_callback_loss_unresolved =
                inventory_stack_callback_loss_unresolved ||
                carries_stack_callback_payload(value);
            inventory_saved_stack_alias_latent =
                inventory_saved_stack_alias_latent ||
                has_latent_saved_stack_alias(value);
            inventory_saved_stack_alias_tracks_current_epoch =
                inventory_saved_stack_alias_tracks_current_epoch ||
                (has_latent_saved_stack_alias(value) &&
                 value.inventory_saved_stack_epoch
                     .tracks_current_epoch);
            if (carries_stack_callback_payload(value)) {
                complete = false;
                summary.guarded = true;
            }
            contextual_candidate_dependency =
                contextual_candidate_dependency || value.contextual_candidate_dependency;
            inventory_code_pointer_values.insert(
                value.inventory_code_pointer_values.begin(),
                value.inventory_code_pointer_values.end());
            inventory_pc_relative_code_literal_values.insert(
                value.inventory_pc_relative_code_literal_values.begin(),
                value.inventory_pc_relative_code_literal_values.end());
            inventory_code_pointer_values_truncated =
                inventory_code_pointer_values_truncated ||
                value.inventory_code_pointer_values_truncated;
            inventory_pc_relative_code_literal_values_truncated =
                inventory_pc_relative_code_literal_values_truncated ||
                value.inventory_pc_relative_code_literal_values_truncated;
            if (!value.known || value.values.empty()) {
                complete = false;
                finite = false;
                continue;
            }
            if (!value.complete) complete = false;
            values.insert(value.values.begin(), value.values.end());
            evidence.insert(value.callees.begin(), value.callees.end());
            summary.guarded = summary.guarded || value.guarded || !value.complete;
        }
        if (values.size() > maximum_summary_values) {
            complete = false;
            finite = false;
        }
        summary.complete = complete;
        summary.contextual_candidate_dependency = contextual_candidate_dependency;
        summary.inventory_stack_callback_loss_unresolved =
            inventory_stack_callback_loss_unresolved;
        summary.inventory_saved_stack_alias_latent =
            inventory_saved_stack_alias_latent;
        summary.inventory_saved_stack_alias_tracks_current_epoch =
            inventory_saved_stack_alias_tracks_current_epoch;
        if (finite) summary.values.assign(values.begin(), values.end());
        summary.inventory_code_pointer_values.assign(
            inventory_code_pointer_values.begin(), inventory_code_pointer_values.end());
        summary.inventory_pc_relative_code_literal_values.assign(
            inventory_pc_relative_code_literal_values.begin(),
            inventory_pc_relative_code_literal_values.end());
        if (summary.inventory_code_pointer_values.size() >
            maximum_guarded_code_inventory) {
            summary.inventory_code_pointer_values.resize(
                maximum_guarded_code_inventory);
            inventory_code_pointer_values_truncated = true;
        }
        if (summary.inventory_pc_relative_code_literal_values.size() >
            maximum_guarded_code_inventory) {
            summary.inventory_pc_relative_code_literal_values.resize(
                maximum_guarded_code_inventory);
            inventory_pc_relative_code_literal_values_truncated = true;
        }
        summary.inventory_code_pointer_values_truncated =
            inventory_code_pointer_values_truncated;
        summary.inventory_pc_relative_code_literal_values_truncated =
            inventory_pc_relative_code_literal_values_truncated;
        summary.inventory_code_pointer = !summary.inventory_code_pointer_values.empty();
        summary.inventory_pc_relative_code_literal =
            !summary.inventory_pc_relative_code_literal_values.empty();
        summary.evidence_callees.assign(evidence.begin(), evidence.end());
        summary.reason = complete
                             ? (summary.values.size() == 1u ? "constant-return"
                                                           : "finite-return-set")
                         : finite ? (summary.values.size() == 1u
                                         ? "constant-return-candidate"
                                         : "finite-return-set-candidate")
                                  : (returns.empty() ? "no-return"
                                                     : "return-path-unknown");
        evaluation.summary.registers.push_back(std::move(summary));
    }
    evaluation.summary.memory_complete = !returns.empty();
    if (!returns.empty()) {
        std::set<std::uint32_t>
            returned_memory_stack_callback_loss_unresolved;
        std::map<std::uint32_t, bool>
            returned_memory_saved_stack_alias_latent;
        for (const auto& [return_site, state] : returns) {
            static_cast<void>(return_site);
            for (const auto& [address, value] :
                 state.memory_values) {
                const bool callback_loss =
                    carries_stack_callback_payload(value);
                const bool latent_alias =
                    has_latent_saved_stack_alias(value);
                if (!callback_loss && !latent_alias)
                    continue;
                const auto initial =
                    initial_state.memory_values.find(address);
                // apply_call() keeps the caller's exact saved-stack memory
                // unless a callee summary replaces that cell. A no-op helper
                // must therefore not downgrade an unchanged finite epoch to
                // an unresolved marker. A newly created, changed or already
                // unresolved payload still cannot cross the public summary
                // structurally and must fail closed at this exact address.
                if (!carries_unresolved_stack_callback(value) &&
                    initial != initial_state.memory_values.end() &&
                    same_stack_callback_provenance(
                        initial->second, value))
                    continue;
                if (latent_alias && !callback_loss) {
                    auto& tracks_current =
                        returned_memory_saved_stack_alias_latent[
                            address];
                    tracks_current =
                        tracks_current ||
                        value.inventory_saved_stack_epoch
                            .tracks_current_epoch;
                    continue;
                }
                if (!returned_memory_stack_callback_loss_unresolved
                         .contains(address) &&
                    returned_memory_stack_callback_loss_unresolved
                            .size() >= maximum_memory_values) {
                    if (walk_diagnostics != nullptr)
                        walk_diagnostics
                            ->inventory_candidate_values_truncated =
                            true;
                    continue;
                }
                returned_memory_stack_callback_loss_unresolved
                    .insert(address);
            }
        }
        auto first_return = returns.begin();
        auto returned_memory =
            first_return->second.memory_values;
        for (auto return_state = std::next(first_return);
             return_state != returns.end();
             ++return_state) {
            for (auto value = returned_memory.begin(); value != returned_memory.end();) {
                const auto candidate = return_state->second.memory_values.find(value->first);
                if (candidate == return_state->second.memory_values.end()) {
                    value = returned_memory.erase(value);
                    continue;
                }
                merge_value(value->second, candidate->second);
                if (!value->second.known)
                    value = returned_memory.erase(value);
                else
                    ++value;
            }
        }
        for (auto& [address, value] : returned_memory) {
            FunctionMemoryValueSummary memory;
            memory.address = address;
            memory.inventory_stack_callback_loss_unresolved =
                returned_memory_stack_callback_loss_unresolved
                    .erase(address) != 0u;
            const auto latent =
                returned_memory_saved_stack_alias_latent.find(
                    address);
            memory.inventory_saved_stack_alias_latent =
                latent !=
                returned_memory_saved_stack_alias_latent.end();
            if (latent !=
                returned_memory_saved_stack_alias_latent.end()) {
                memory
                    .inventory_saved_stack_alias_tracks_current_epoch =
                    latent->second;
                returned_memory_saved_stack_alias_latent.erase(
                    latent);
            }
            memory.complete =
                value.complete &&
                !memory
                     .inventory_stack_callback_loss_unresolved &&
                !memory.inventory_saved_stack_alias_latent;
            memory.guarded =
                value.guarded ||
                memory
                    .inventory_stack_callback_loss_unresolved ||
                memory.inventory_saved_stack_alias_latent;
            memory.values = std::move(value.values);
            evaluation.summary.memory_values.push_back(std::move(memory));
        }
        for (const auto address :
             returned_memory_stack_callback_loss_unresolved) {
            if (evaluation.summary.memory_values.size() >=
                maximum_memory_values) {
                if (walk_diagnostics != nullptr)
                    walk_diagnostics
                        ->inventory_candidate_values_truncated =
                        true;
                break;
            }
            FunctionMemoryValueSummary memory;
            memory.address = address;
            memory.guarded = true;
            memory
                .inventory_stack_callback_loss_unresolved = true;
            evaluation.summary.memory_values.push_back(
                std::move(memory));
        }
        for (const auto& [address, tracks_current_epoch] :
             returned_memory_saved_stack_alias_latent) {
            if (evaluation.summary.memory_values.size() >=
                maximum_memory_values) {
                if (walk_diagnostics != nullptr)
                    walk_diagnostics
                        ->inventory_candidate_values_truncated =
                        true;
                break;
            }
            FunctionMemoryValueSummary memory;
            memory.address = address;
            memory.guarded = true;
            memory.inventory_saved_stack_alias_latent = true;
            memory
                .inventory_saved_stack_alias_tracks_current_epoch =
                tracks_current_epoch;
            evaluation.summary.memory_values.push_back(
                std::move(memory));
        }
        std::sort(
            evaluation.summary.memory_values.begin(),
            evaluation.summary.memory_values.end(),
            [](const auto& left, const auto& right) {
                return left.address < right.address;
            });
    }
    return evaluation;
}

std::optional<std::int32_t>
callee_relative_stack_offset(const AbstractState& call_observation,
                             const std::uint8_t register_index) {
    const auto register_offset = call_observation.stack_offsets[register_index];
    const auto caller_sp_offset = call_observation.stack_offsets[15u];
    if (!register_offset.has_value() || !caller_sp_offset.has_value())
        return std::nullopt;
    const auto rebased = static_cast<std::int64_t>(*register_offset) -
                         static_cast<std::int64_t>(*caller_sp_offset);
    if (rebased < -maximum_stack_distance || rebased > maximum_stack_distance)
        return std::nullopt;
    return static_cast<std::int32_t>(rebased);
}

bool merge_candidate_input(
    CandidateInput& destination,
    const FunctionEvaluation::CallArguments& observation,
    GuardedCodeInventoryWalkDiagnostics* const walk_diagnostics) {
    const auto [stored, inserted] =
        destination.observations.try_emplace(observation.call_site, observation.state);
    if (!inserted) {
        if (stored->second == observation.state) return false;
        stored->second = observation.state;
    }
    AbstractState merged;
    merged.stack_offsets[15u] = 0;
    for (const auto& [call_site, state] : destination.observations) {
        static_cast<void>(call_site);
        merged.inventory_unresolved_saved_stack_alias_sources =
            static_cast<std::uint8_t>(
                merged.inventory_unresolved_saved_stack_alias_sources |
                state.inventory_unresolved_saved_stack_alias_sources);
        merged
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch =
            merged
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
            state
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
        merged.inventory_current_stack_epoch_alias_watcher =
            merged.inventory_current_stack_epoch_alias_watcher ||
            state.inventory_current_stack_epoch_alias_watcher;
        merged.inventory_detached_stack_epoch_alias_watcher =
            merged.inventory_detached_stack_epoch_alias_watcher ||
            state.inventory_detached_stack_epoch_alias_watcher;
        merged.inventory_unresolved_stack_callback_loss =
            merged.inventory_unresolved_stack_callback_loss ||
            state.inventory_unresolved_stack_callback_loss;
        merged.inventory_stack_callback_loss_identity_truncated =
            merged.inventory_stack_callback_loss_identity_truncated ||
            state
                .inventory_stack_callback_loss_identity_truncated;
    }
    if (merged.inventory_stack_callback_loss_identity_truncated &&
        walk_diagnostics != nullptr)
        walk_diagnostics->inventory_candidate_values_truncated =
            true;
    if (destination.unknown_ingress || destination.expected_call_sites.empty() ||
        !std::all_of(
            destination.expected_call_sites.begin(),
            destination.expected_call_sites.end(),
            [&](const auto call_site) { return destination.observations.contains(call_site); })) {
        // An incomplete callsite family cannot publish scalar constants, but
        // losing inventory-only stack-snapshot provenance here would make the
        // later complete family unsound. Keep the exact bounded epoch when it
        // is still representable; only a real loss remains a loss marker.
        std::array<bool, 15u> retained_register_provenance{};
        for (const auto& [call_site, state] : destination.observations) {
            static_cast<void>(call_site);
            for (std::uint8_t index = 0u; index < 15u; ++index) {
                const auto& source = state[index];
                if (!has_saved_stack_epoch(source) &&
                    !carries_unresolved_stack_callback(source))
                    continue;
                AbstractValue provenance;
                provenance.guarded = true;
                provenance.complete = false;
                provenance.inventory_stack_callback_loss_unresolved =
                    source.inventory_stack_callback_loss_unresolved;
                provenance.inventory_saved_stack_epoch =
                    source.inventory_saved_stack_epoch;
                if (!retained_register_provenance[index]) {
                    merged[index] = std::move(provenance);
                    retained_register_provenance[index] = true;
                } else {
                    static_cast<void>(
                        merge_value(merged[index], provenance));
                }
            }
            const auto retain_provenance =
                [&](auto& values,
                    const auto key,
                    const std::size_t limit,
                    const std::uint8_t alias_source,
                    const AbstractValue& source) {
                if (!values.contains(key) &&
                    values.size() >= limit) {
                    if (has_latent_saved_stack_alias(source)) {
                        add_unresolved_saved_stack_alias(
                            merged,
                            alias_source,
                            source.inventory_saved_stack_epoch
                                .tracks_current_epoch);
                    } else {
                        if (walk_diagnostics != nullptr)
                            walk_diagnostics
                                ->inventory_candidate_values_truncated =
                                true;
                        merged.inventory_unresolved_stack_callback_loss =
                            true;
                        merged
                            .inventory_stack_callback_loss_identity_truncated =
                            true;
                    }
                    return;
                }
                AbstractValue provenance;
                provenance.guarded = true;
                provenance.complete = false;
                provenance.inventory_stack_callback_loss_unresolved =
                    source.inventory_stack_callback_loss_unresolved;
                provenance.inventory_saved_stack_epoch =
                    source.inventory_saved_stack_epoch;
                const auto [stored_value, inserted_value] =
                    values.try_emplace(key, provenance);
                if (!inserted_value)
                    static_cast<void>(
                        merge_value(stored_value->second, provenance));
            };
            for (const auto& [slot, value] : state.stack_values) {
                if (has_saved_stack_epoch(value) ||
                    carries_unresolved_stack_callback(value))
                    retain_provenance(
                        merged.stack_values,
                        slot,
                        maximum_abi_stack_argument_slots,
                        unresolved_saved_stack_alias_source_stack,
                        value);
            }
            for (const auto& [address, value] : state.memory_values) {
                if (has_saved_stack_epoch(value) ||
                    carries_unresolved_stack_callback(value))
                    retain_provenance(
                        merged.memory_values,
                        address,
                        maximum_memory_values,
                        unresolved_saved_stack_alias_source_memory,
                        value);
            }
        }
        const bool changed = destination.state != merged;
        destination.state = std::move(merged);
        return changed;
    }
    for (std::uint8_t index = 0u; index < 15u; ++index) {
        auto& target = merged[index];
        bool first = true;
        bool all_known = true;
        bool first_stack_provenance = true;
        bool exact_stack_provenance = true;
        bool definite_inventory_stack_provenance = true;
        bool may_alias_stack = false;
        bool inventory_may_alias_stack = false;
        bool inventory_stack_derived = true;
        bool inventory_vbr_relative = true;
        bool inventory_fixed_storage_reference = true;
        std::optional<std::int32_t> stack_offset;
        std::vector<std::int32_t> inventory_stack_coordinates_union;
        std::set<std::uint32_t> inventory_code_pointer_values;
        std::set<std::uint32_t> inventory_pc_relative_code_literal_values;
        bool inventory_code_pointer_values_truncated = false;
        bool inventory_pc_relative_code_literal_values_truncated = false;
        bool inventory_stack_callback_loss_unresolved = false;
        InventorySavedStackEpoch inventory_saved_stack_epoch;
        bool inventory_saved_stack_epoch_initialized = false;
        std::set<std::uint32_t> call_sites;
        std::set<std::uint32_t> callees;
        for (const auto call_site : destination.expected_call_sites) {
            const auto& call_observation = destination.observations.at(call_site);
            const auto& source = call_observation[index];
            call_sites.insert(source.call_sites.begin(), source.call_sites.end());
            callees.insert(source.callees.begin(), source.callees.end());
            may_alias_stack =
                may_alias_stack || call_observation.stack_may_alias[index];
            inventory_may_alias_stack =
                inventory_may_alias_stack ||
                call_observation.inventory_stack_may_alias[index];
            inventory_stack_derived =
                inventory_stack_derived && source.inventory_stack_derived;
            inventory_code_pointer_values.insert(
                source.inventory_code_pointer_values.begin(),
                source.inventory_code_pointer_values.end());
            inventory_pc_relative_code_literal_values.insert(
                source.inventory_pc_relative_code_literal_values.begin(),
                source.inventory_pc_relative_code_literal_values.end());
            inventory_code_pointer_values_truncated =
                inventory_code_pointer_values_truncated ||
                source.inventory_code_pointer_values_truncated;
            inventory_pc_relative_code_literal_values_truncated =
                inventory_pc_relative_code_literal_values_truncated ||
                source.inventory_pc_relative_code_literal_values_truncated;
            inventory_stack_callback_loss_unresolved =
                inventory_stack_callback_loss_unresolved ||
                carries_unresolved_stack_callback(source);
            if (has_saved_stack_epoch(source)) {
                if (!inventory_saved_stack_epoch_initialized) {
                    inventory_saved_stack_epoch =
                        source.inventory_saved_stack_epoch;
                    inventory_saved_stack_epoch_initialized = true;
                } else {
                    static_cast<void>(
                        merge_inventory_saved_stack_epoch(
                            inventory_saved_stack_epoch,
                            source.inventory_saved_stack_epoch,
                            true));
                }
            }
            inventory_vbr_relative =
                inventory_vbr_relative &&
                call_observation.inventory_vbr_relative[index];
            inventory_fixed_storage_reference =
                inventory_fixed_storage_reference &&
                call_observation.inventory_fixed_storage_reference[index];
            const auto rebased_stack_offset =
                callee_relative_stack_offset(call_observation, index);
            if (first_stack_provenance) {
                stack_offset = rebased_stack_offset;
                first_stack_provenance = false;
            } else if (stack_offset != rebased_stack_offset) {
                exact_stack_provenance = false;
            }
            const auto observation_inventory_coordinates =
                inventory_stack_slots(call_observation, index);
            if (observation_inventory_coordinates.empty()) {
                definite_inventory_stack_provenance = false;
            } else if (definite_inventory_stack_provenance) {
                inventory_stack_coordinates_union.insert(
                    inventory_stack_coordinates_union.end(),
                    observation_inventory_coordinates.begin(),
                    observation_inventory_coordinates.end());
                normalize_stack_coordinates(
                    inventory_stack_coordinates_union);
                if (inventory_stack_coordinates_union.size() >
                    maximum_inventory_stack_coordinates) {
                    inventory_stack_coordinates_union.clear();
                    definite_inventory_stack_provenance = false;
                }
            }
            if (source.known || (index >= 4u && index <= 7u)) call_sites.insert(call_site);
            if (!source.known || source.values.empty()) {
                all_known = false;
                continue;
            }
            if (first) {
                target = source;
                first = false;
            } else {
                target.values.insert(
                    target.values.end(), source.values.begin(), source.values.end());
                normalize(target.values);
                target.complete = target.complete && source.complete;
            }
            target.guarded = true;
            if (target.values.size() > maximum_summary_values) {
                all_known = false;
            }
        }
        if (!all_known || first) make_unknown(target);
        target.inventory_stack_derived = inventory_stack_derived;
        target.inventory_code_pointer_values.assign(
            inventory_code_pointer_values.begin(), inventory_code_pointer_values.end());
        target.inventory_pc_relative_code_literal_values.assign(
            inventory_pc_relative_code_literal_values.begin(),
            inventory_pc_relative_code_literal_values.end());
        target.inventory_code_pointer_values_truncated =
            inventory_code_pointer_values_truncated;
        target.inventory_pc_relative_code_literal_values_truncated =
            inventory_pc_relative_code_literal_values_truncated;
        target.inventory_stack_callback_loss_unresolved =
            inventory_stack_callback_loss_unresolved;
        target.inventory_saved_stack_epoch =
            std::move(inventory_saved_stack_epoch);
        synchronize_inventory_provenance(target);
        target.call_sites = std::move(call_sites);
        target.callees = std::move(callees);
        merged.stack_may_alias[index] = may_alias_stack;
        merged.inventory_stack_may_alias[index] =
            inventory_may_alias_stack;
        merged.inventory_vbr_relative[index] =
            inventory_vbr_relative;
        merged.inventory_fixed_storage_reference[index] =
            inventory_fixed_storage_reference;
        if (may_alias_stack && exact_stack_provenance)
            merged.stack_offsets[index] = stack_offset;
        else
            merged.stack_offsets[index].reset();
        if (!definite_inventory_stack_provenance)
            inventory_stack_coordinates_union.clear();
        static_cast<void>(set_inventory_stack_coordinates(
            merged,
            index,
            std::move(inventory_stack_coordinates_union)));
    }
    const auto first_call_site = *destination.expected_call_sites.begin();
    std::set<std::uint32_t>
        memory_stack_callback_loss_unresolved;
    std::set<std::int32_t>
        stack_callback_loss_unresolved;
    std::map<std::uint32_t, InventorySavedStackEpoch>
        memory_saved_stack_epochs;
    std::map<std::int32_t, InventorySavedStackEpoch>
        stack_saved_stack_epochs;
    for (const auto call_site : destination.expected_call_sites) {
        const auto& call_state =
            destination.observations.at(call_site);
        for (const auto& [address, value] :
             call_state.memory_values) {
            if (carries_unresolved_stack_callback(value)) {
                if (!memory_stack_callback_loss_unresolved
                         .contains(address) &&
                    memory_stack_callback_loss_unresolved.size() >=
                        maximum_memory_values) {
                    if (walk_diagnostics != nullptr)
                        walk_diagnostics
                            ->inventory_candidate_values_truncated =
                            true;
                    merged.inventory_unresolved_stack_callback_loss =
                        true;
                    merged
                        .inventory_stack_callback_loss_identity_truncated =
                        true;
                } else {
                    memory_stack_callback_loss_unresolved.insert(
                        address);
                }
            }
            if (has_saved_stack_epoch(value)) {
                if (!memory_saved_stack_epochs.contains(address) &&
                    memory_saved_stack_epochs.size() >=
                        maximum_memory_values) {
                    if (has_latent_saved_stack_alias(value)) {
                        add_unresolved_saved_stack_alias(
                            merged,
                            unresolved_saved_stack_alias_source_memory,
                            value.inventory_saved_stack_epoch
                                .tracks_current_epoch);
                    } else {
                        if (walk_diagnostics != nullptr)
                            walk_diagnostics
                                ->inventory_candidate_values_truncated =
                                true;
                        merged.inventory_unresolved_stack_callback_loss =
                            true;
                        merged
                            .inventory_stack_callback_loss_identity_truncated =
                            true;
                    }
                    continue;
                }
                const auto [stored_epoch, inserted_epoch] =
                    memory_saved_stack_epochs.try_emplace(
                        address,
                        value.inventory_saved_stack_epoch);
                if (!inserted_epoch)
                    static_cast<void>(
                        merge_inventory_saved_stack_epoch(
                            stored_epoch->second,
                            value.inventory_saved_stack_epoch,
                            true));
            }
        }
        for (const auto& [slot, value] :
             call_state.stack_values) {
            if (carries_unresolved_stack_callback(value)) {
                if (!stack_callback_loss_unresolved.contains(slot) &&
                    stack_callback_loss_unresolved.size() >=
                        maximum_abi_stack_argument_slots) {
                    if (walk_diagnostics != nullptr)
                        walk_diagnostics
                            ->inventory_candidate_values_truncated =
                            true;
                    merged.inventory_unresolved_stack_callback_loss =
                        true;
                    merged
                        .inventory_stack_callback_loss_identity_truncated =
                        true;
                } else {
                    stack_callback_loss_unresolved.insert(slot);
                }
            }
            if (has_saved_stack_epoch(value)) {
                if (!stack_saved_stack_epochs.contains(slot) &&
                    stack_saved_stack_epochs.size() >=
                        maximum_abi_stack_argument_slots) {
                    if (has_latent_saved_stack_alias(value)) {
                        add_unresolved_saved_stack_alias(
                            merged,
                            unresolved_saved_stack_alias_source_stack,
                            value.inventory_saved_stack_epoch
                                .tracks_current_epoch);
                    } else {
                        if (walk_diagnostics != nullptr)
                            walk_diagnostics
                                ->inventory_candidate_values_truncated =
                                true;
                        merged.inventory_unresolved_stack_callback_loss =
                            true;
                        merged
                            .inventory_stack_callback_loss_identity_truncated =
                            true;
                    }
                    continue;
                }
                const auto [stored_epoch, inserted_epoch] =
                    stack_saved_stack_epochs.try_emplace(
                        slot,
                        value.inventory_saved_stack_epoch);
                if (!inserted_epoch)
                    static_cast<void>(
                        merge_inventory_saved_stack_epoch(
                            stored_epoch->second,
                            value.inventory_saved_stack_epoch,
                            true));
            }
        }
    }
    merged.memory_values = destination.observations.at(first_call_site).memory_values;
    for (auto value = merged.memory_values.begin(); value != merged.memory_values.end();) {
        bool retained = true;
        for (const auto call_site : destination.expected_call_sites) {
            if (call_site == first_call_site) continue;
            const auto& source_values = destination.observations.at(call_site).memory_values;
            const auto source = source_values.find(value->first);
            if (source == source_values.end()) {
                retained = false;
                break;
            }
            merge_value(value->second, source->second);
            if (!value->second.known &&
                !has_inventory_candidate_values(value->second) &&
                !has_saved_stack_epoch(value->second) &&
                !carries_unresolved_stack_callback(
                    value->second)) {
                retained = false;
                break;
            }
        }
        if (!retained)
            value = merged.memory_values.erase(value);
        else {
            value->second.guarded = true;
            ++value;
        }
    }
    for (const auto address :
         memory_stack_callback_loss_unresolved) {
        if (!merged.memory_values.contains(address) &&
            merged.memory_values.size() >= maximum_memory_values) {
            if (walk_diagnostics != nullptr)
                walk_diagnostics
                    ->inventory_candidate_values_truncated = true;
            merged.inventory_unresolved_stack_callback_loss = true;
            merged
                .inventory_stack_callback_loss_identity_truncated =
                true;
            continue;
        }
        AbstractValue unresolved;
        unresolved.guarded = true;
        unresolved.complete = false;
        unresolved.inventory_stack_callback_loss_unresolved = true;
        const auto [stored_value, inserted_value] =
            merged.memory_values.try_emplace(address, unresolved);
        if (!inserted_value)
            static_cast<void>(
                merge_value(stored_value->second, unresolved));
    }
    for (auto& [address, epoch] :
         memory_saved_stack_epochs) {
        if (!merged.memory_values.contains(address) &&
            merged.memory_values.size() >= maximum_memory_values) {
            if (!epoch.candidate_payload_lost &&
                epoch.slots.empty()) {
                add_unresolved_saved_stack_alias(
                    merged,
                    unresolved_saved_stack_alias_source_memory,
                    epoch.tracks_current_epoch);
            } else {
                if (walk_diagnostics != nullptr)
                    walk_diagnostics
                        ->inventory_candidate_values_truncated = true;
                merged.inventory_unresolved_stack_callback_loss =
                    true;
                merged
                    .inventory_stack_callback_loss_identity_truncated =
                    true;
            }
            continue;
        }
        AbstractValue provenance;
        provenance.guarded = true;
        provenance.complete = false;
        provenance.inventory_saved_stack_epoch =
            std::move(epoch);
        const auto [stored_value, inserted_value] =
            merged.memory_values.try_emplace(
                address, provenance);
        if (!inserted_value)
            static_cast<void>(
                merge_value(stored_value->second, provenance));
    }
    merged.stack_values = destination.observations.at(first_call_site).stack_values;
    for (auto value = merged.stack_values.begin(); value != merged.stack_values.end();) {
        bool retained = true;
        for (const auto call_site : destination.expected_call_sites) {
            if (call_site == first_call_site) continue;
            const auto& source_values = destination.observations.at(call_site).stack_values;
            const auto source = source_values.find(value->first);
            if (source == source_values.end()) {
                retained = false;
                break;
            }
            merge_value(value->second, source->second);
            if (!value->second.known &&
                !has_inventory_candidate_values(value->second) &&
                !has_saved_stack_epoch(value->second) &&
                !carries_unresolved_stack_callback(
                    value->second)) {
                retained = false;
                break;
            }
        }
        if (!retained)
            value = merged.stack_values.erase(value);
        else {
            value->second.guarded = true;
            ++value;
        }
    }
    for (const auto slot :
         stack_callback_loss_unresolved) {
        if (!merged.stack_values.contains(slot) &&
            merged.stack_values.size() >=
                maximum_abi_stack_argument_slots) {
            if (walk_diagnostics != nullptr)
                walk_diagnostics
                    ->inventory_candidate_values_truncated = true;
            merged.inventory_unresolved_stack_callback_loss = true;
            merged
                .inventory_stack_callback_loss_identity_truncated =
                true;
            continue;
        }
        AbstractValue unresolved;
        unresolved.guarded = true;
        unresolved.complete = false;
        unresolved.inventory_stack_callback_loss_unresolved = true;
        const auto [stored_value, inserted_value] =
            merged.stack_values.try_emplace(slot, unresolved);
        if (!inserted_value)
            static_cast<void>(
                merge_value(stored_value->second, unresolved));
    }
    for (auto& [slot, epoch] :
         stack_saved_stack_epochs) {
        if (!merged.stack_values.contains(slot) &&
            merged.stack_values.size() >=
                maximum_abi_stack_argument_slots) {
            if (!epoch.candidate_payload_lost &&
                epoch.slots.empty()) {
                add_unresolved_saved_stack_alias(
                    merged,
                    unresolved_saved_stack_alias_source_stack,
                    epoch.tracks_current_epoch);
            } else {
                if (walk_diagnostics != nullptr)
                    walk_diagnostics
                        ->inventory_candidate_values_truncated = true;
                merged.inventory_unresolved_stack_callback_loss =
                    true;
                merged
                    .inventory_stack_callback_loss_identity_truncated =
                    true;
            }
            continue;
        }
        AbstractValue provenance;
        provenance.guarded = true;
        provenance.complete = false;
        provenance.inventory_saved_stack_epoch =
            std::move(epoch);
        const auto [stored_value, inserted_value] =
            merged.stack_values.try_emplace(
                slot, provenance);
        if (!inserted_value)
            static_cast<void>(
                merge_value(stored_value->second, provenance));
    }
    const bool changed = destination.state != merged;
    destination.state = std::move(merged);
    return changed;
}

bool requires_isolated_store_harvest(
    const CandidateInput& input,
    const std::uint32_t call_site,
    const AbstractState& observation) {
    if (input.unknown_ingress || input.expected_call_sites.empty() ||
        !std::all_of(
            input.expected_call_sites.begin(),
            input.expected_call_sites.end(),
            [&](const auto call_site) { return input.observations.contains(call_site); }))
        return true;
    if (!input.expected_call_sites.contains(call_site)) return true;
    if (observation.inventory_unresolved_saved_stack_alias_sources !=
            input.state
                .inventory_unresolved_saved_stack_alias_sources ||
        observation
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch !=
            input.state
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        observation.inventory_current_stack_epoch_alias_watcher !=
            input.state.inventory_current_stack_epoch_alias_watcher ||
        observation.inventory_detached_stack_epoch_alias_watcher !=
            input.state.inventory_detached_stack_epoch_alias_watcher ||
        observation.inventory_unresolved_stack_callback_loss !=
            input.state.inventory_unresolved_stack_callback_loss ||
        observation.inventory_stack_callback_loss_identity_truncated !=
            input.state
                .inventory_stack_callback_loss_identity_truncated)
        return true;
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        const auto& observed = observation[index];
        const auto& merged = input.state[index];
        if (!same_stack_callback_provenance(observed, merged))
            return true;
        if (!observed.known || observed.values.empty()) continue;
        if (!merged.known ||
            std::any_of(observed.values.begin(),
                        observed.values.end(),
                        [&](const auto value) {
                            return std::find(
                                       merged.values.begin(), merged.values.end(), value) ==
                                   merged.values.end();
                        }))
            return true;
    }
    for (const auto& [slot, observed] : observation.stack_values) {
        if (!observed.known || observed.values.empty()) continue;
        const auto merged = input.state.stack_values.find(slot);
        if (merged == input.state.stack_values.end() || !merged->second.known ||
            std::any_of(observed.values.begin(),
                        observed.values.end(),
                        [&](const auto value) {
                            return std::find(merged->second.values.begin(),
                                             merged->second.values.end(),
                                             value) == merged->second.values.end();
                        }))
            return true;
    }
    const auto unresolved_map_differs =
        [](const auto& observed_values,
           const auto& merged_values) {
            const auto provenance_at = [](const auto& values,
                                          const auto key) {
                const auto found = values.find(key);
                return found == values.end()
                           ? AbstractValue{}
                           : found->second;
            };
            for (const auto& [key, value] : observed_values) {
                if (!same_stack_callback_provenance(
                        value,
                        provenance_at(merged_values, key)))
                    return true;
            }
            for (const auto& [key, value] : merged_values) {
                if (!same_stack_callback_provenance(
                        value,
                        provenance_at(observed_values, key)))
                    return true;
            }
            return false;
        };
    if (unresolved_map_differs(observation.stack_values,
                               input.state.stack_values) ||
        unresolved_map_differs(observation.memory_values,
                               input.state.memory_values))
        return true;
    return false;
}

bool guarded_inventory_store_instruction(
    const katana::sh4::InstructionKind kind) noexcept {
    using K = katana::sh4::InstructionKind;
    switch (kind) {
    case K::MovLongStore:
    case K::MovLongStorePreDecrement:
    case K::MovLongStoreDisplacement:
    case K::MovLongStoreR0Indexed:
    case K::MovLongStoreGbrDisplacement:
        return true;
    default:
        return false;
    }
}

// The guarded inventory's forwarding walk is only useful for a function whose
// incoming ABI value can actually reach the source operand of a persistent
// long store. A syntactic "contains mov.l store" test is much too broad: it
// turns ordinary object/data stores throughout the call graph into callback
// registrar candidates and exhausts the bounded isolated-context walk.
//
// This deliberately small, value-shape-only pass is not a second value
// analysis. It tracks ABI arguments through register moves and stack
// spill/reload patterns, plus direct helper calls whose result may depend on a
// forwarded ABI argument. Unknown memory loads do not inherit the taint, the
// same contract used by the real inventory collector. If stack tracking is
// lost, the pass becomes conservative for subsequent stack loads rather than
// hiding a possible callback path.
constexpr std::uint8_t abi_stack_argument_taint = 1u << 4u;
constexpr std::uint8_t abi_argument_taint_mask =
    (abi_stack_argument_taint << 1u) - 1u;

struct AbiPersistentStoreFlowState {
    std::array<std::uint8_t, 16u> register_taints{};
    std::array<std::optional<std::int32_t>, 16u> stack_offsets{};
    std::array<std::vector<std::int32_t>, 16u> stack_offset_candidates;
    // `stack_derived` means every represented path is stack-derived.
    // `stack_may_alias` additionally retains mixed stack/non-stack joins.
    std::array<bool, 16u> stack_derived{};
    std::array<bool, 16u> stack_may_alias{};
    std::array<std::optional<std::uint32_t>, 16u> constants{};
    std::map<std::int32_t, std::uint8_t> stack_taints;
    std::set<std::int32_t> stack_definitely_defined;
    bool stack_tracking_lost = false;

    bool operator==(const AbiPersistentStoreFlowState&) const = default;
};

[[nodiscard]] std::vector<std::int32_t>
abi_stack_coordinates(const AbiPersistentStoreFlowState& state,
                      const std::uint8_t register_index) {
    if (!state.stack_derived[register_index]) return {};
    if (state.stack_offsets[register_index].has_value())
        return {*state.stack_offsets[register_index]};
    return state.stack_offset_candidates[register_index];
}

bool set_abi_stack_coordinates(
    AbiPersistentStoreFlowState& state,
    const std::uint8_t register_index,
    std::vector<std::int32_t> coordinates) {
    normalize_stack_coordinates(coordinates);
    if (coordinates.empty() ||
        coordinates.size() > maximum_inventory_stack_coordinates) {
        state.stack_tracking_lost = true;
        state.stack_offsets[register_index].reset();
        state.stack_offset_candidates[register_index].clear();
        return false;
    }
    if (coordinates.size() == 1u) {
        state.stack_offsets[register_index] = coordinates.front();
        state.stack_offset_candidates[register_index].clear();
    } else {
        state.stack_offsets[register_index].reset();
        state.stack_offset_candidates[register_index] =
            std::move(coordinates);
    }
    return true;
}

[[nodiscard]] std::vector<std::int32_t>
abi_stack_slots(const AbiPersistentStoreFlowState& state,
                const std::uint8_t base_register,
                const std::int32_t displacement = 0) {
    auto slots = abi_stack_coordinates(state, base_register);
    for (auto& slot : slots) {
        const auto displaced =
            static_cast<std::int64_t>(slot) +
            static_cast<std::int64_t>(displacement);
        if (displaced < -maximum_stack_distance ||
            displaced > maximum_stack_distance)
            return {};
        slot = static_cast<std::int32_t>(displaced);
    }
    normalize_stack_coordinates(slots);
    return slots;
}

bool adjust_abi_stack_offsets(
    AbiPersistentStoreFlowState& state,
    const std::uint8_t register_index,
    const std::span<const std::int32_t> deltas) {
    const auto coordinates =
        abi_stack_coordinates(state, register_index);
    if (coordinates.empty() || deltas.empty()) {
        state.stack_tracking_lost = true;
        state.stack_offsets[register_index].reset();
        state.stack_offset_candidates[register_index].clear();
        return false;
    }
    std::vector<std::int32_t> adjusted;
    adjusted.reserve(coordinates.size() * deltas.size());
    for (const auto coordinate : coordinates) {
        for (const auto delta : deltas) {
            const auto candidate =
                static_cast<std::int64_t>(coordinate) +
                static_cast<std::int64_t>(delta);
            if (candidate < -maximum_stack_distance ||
                candidate > maximum_stack_distance) {
                state.stack_tracking_lost = true;
                state.stack_offsets[register_index].reset();
                state.stack_offset_candidates[register_index].clear();
                return false;
            }
            adjusted.push_back(
                static_cast<std::int32_t>(candidate));
        }
    }
    return set_abi_stack_coordinates(
        state, register_index, std::move(adjusted));
}

bool adjust_abi_stack_offset(AbiPersistentStoreFlowState& state,
                             const std::uint8_t register_index,
                             const std::int32_t delta) {
    const std::array deltas{delta};
    return adjust_abi_stack_offsets(
        state, register_index, deltas);
}

void clear_abi_flow_register(AbiPersistentStoreFlowState& state,
                             const std::uint8_t register_index) {
    // Overwriting a register discards only that pointer. It does not make the
    // tracked stack memory or SP coordinate unknown.
    state.register_taints[register_index] = 0u;
    state.stack_offsets[register_index].reset();
    state.stack_offset_candidates[register_index].clear();
    state.stack_derived[register_index] = false;
    state.stack_may_alias[register_index] = false;
    state.constants[register_index].reset();
}

[[nodiscard]] std::uint8_t
abi_stack_load_taint(const AbiPersistentStoreFlowState& state,
                     const std::span<const std::int32_t> slots,
                     AbiStackArgumentReadSet* const entry_stack_reads,
                     AbiStackReadTopReason* const top_reason = nullptr) {
    if (slots.empty()) {
        if (entry_stack_reads != nullptr) {
            set_abi_stack_read_top_reason(
                top_reason, AbiStackReadTopReason::LocalStackCoordinate);
            make_abi_stack_argument_reads_unknown(*entry_stack_reads);
        }
        return state.stack_tracking_lost ? abi_argument_taint_mask : 0u;
    }
    auto taint = std::uint8_t{0u};
    for (const auto slot : slots) {
        if (slot >= 0 &&
            !state.stack_definitely_defined.contains(slot) &&
            entry_stack_reads != nullptr)
            static_cast<void>(
                insert_abi_stack_argument_read(
                    *entry_stack_reads, slot, top_reason));
        if (const auto stored = state.stack_taints.find(slot);
            stored != state.stack_taints.end()) {
            taint = static_cast<std::uint8_t>(
                taint | stored->second);
        } else if (slot >= 0) {
            // At function entry, non-negative slots are incoming ABI stack
            // arguments; negative slots are local until a store defines them.
            taint = static_cast<std::uint8_t>(
                taint | abi_stack_argument_taint);
        }
    }
    if (state.stack_tracking_lost)
        taint = static_cast<std::uint8_t>(
            taint | abi_argument_taint_mask);
    return taint;
}

void store_abi_stack_taints(
    AbiPersistentStoreFlowState& state,
    const std::span<const std::int32_t> slots,
    const std::uint8_t taint,
    const bool definitely_defines_long = true) {
    if (slots.empty()) {
        state.stack_tracking_lost = true;
        return;
    }
    for (const auto slot : slots) {
        if (!state.stack_taints.contains(slot) &&
            state.stack_taints.size() >=
                maximum_abi_persistent_flow_stack_slots) {
            state.stack_tracking_lost = true;
            return;
        }
        auto stored_taint = taint;
        if (slots.size() > 1u) {
            if (const auto previous =
                    state.stack_taints.find(slot);
                previous != state.stack_taints.end())
                stored_taint = static_cast<std::uint8_t>(
                    stored_taint | previous->second);
            else if (slot >= 0)
                stored_taint = static_cast<std::uint8_t>(
                    stored_taint |
                    abi_stack_argument_taint);
        }
        state.stack_taints[slot] = stored_taint;
        if (definitely_defines_long && slots.size() == 1u)
            state.stack_definitely_defined.insert(slot);
    }
}

[[nodiscard]] std::uint8_t
abi_outgoing_stack_argument_taint(const AbiPersistentStoreFlowState& state) {
    if (state.stack_tracking_lost) return abi_argument_taint_mask;
    auto taint = std::uint8_t{0u};
    const auto stack_bases = abi_stack_coordinates(state, 15u);
    if (stack_bases.empty()) return abi_stack_argument_taint;
    for (const auto& [slot, value] : state.stack_taints) {
        // At a call boundary, outgoing stack arguments begin at the current
        // stack pointer.  This also preserves a caller's unmaterialized
        // incoming fifth-or-later argument when the frame is reused.
        if (std::any_of(
                stack_bases.begin(),
                stack_bases.end(),
                [&](const auto stack_base) {
                    return slot >= stack_base;
                }))
            taint = static_cast<std::uint8_t>(taint | value);
    }
    // A non-materialized incoming stack argument has no stack_taints entry.
    // It can still be passed through unchanged at this call boundary.
    return static_cast<std::uint8_t>(
        (taint | abi_stack_argument_taint) & abi_argument_taint_mask);
}

[[nodiscard]] bool merge_abi_persistent_store_flow_state(
    AbiPersistentStoreFlowState& destination,
    const AbiPersistentStoreFlowState& source) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.register_taints.size(); ++index) {
        const auto destination_was_stack_derived =
            destination.stack_derived[index];
        const auto destination_coordinates =
            abi_stack_coordinates(
                destination,
                static_cast<std::uint8_t>(index));
        const auto source_coordinates =
            abi_stack_coordinates(
                source,
                static_cast<std::uint8_t>(index));
        const auto merged_taint = static_cast<std::uint8_t>(
            destination.register_taints[index] | source.register_taints[index]);
        if (merged_taint != destination.register_taints[index]) {
            destination.register_taints[index] = merged_taint;
            changed = true;
        }
        // Definite stack provenance survives only when every predecessor has
        // it. The separate may-alias bit retains mixed stack/non-stack joins:
        // loads stay conservative while stores still expose the non-stack
        // branch as a possible persistent sink.
        const auto merged_stack_derived =
            destination.stack_derived[index] && source.stack_derived[index];
        const auto merged_stack_may_alias =
            destination.stack_may_alias[index] ||
            source.stack_may_alias[index];
        if (merged_stack_derived != destination.stack_derived[index]) {
            destination.stack_derived[index] = merged_stack_derived;
            changed = true;
        }
        if (merged_stack_may_alias !=
            destination.stack_may_alias[index]) {
            destination.stack_may_alias[index] =
                merged_stack_may_alias;
            changed = true;
        }
        if (merged_stack_derived) {
            if (destination_coordinates.empty() ||
                source_coordinates.empty()) {
                if (!destination.stack_tracking_lost) {
                    destination.stack_tracking_lost = true;
                    changed = true;
                }
                if (destination.stack_offsets[index].has_value() ||
                    !destination
                         .stack_offset_candidates[index]
                         .empty()) {
                    destination.stack_offsets[index].reset();
                    destination
                        .stack_offset_candidates[index]
                        .clear();
                    changed = true;
                }
            } else {
                auto merged_coordinates =
                    destination_coordinates;
                merged_coordinates.insert(
                    merged_coordinates.end(),
                    source_coordinates.begin(),
                    source_coordinates.end());
                normalize_stack_coordinates(
                    merged_coordinates);
                if (
                merged_coordinates.size() >
                    maximum_inventory_stack_coordinates) {
                    if (!destination.stack_tracking_lost) {
                        destination.stack_tracking_lost = true;
                        changed = true;
                    }
                    if (destination
                            .stack_offsets[index]
                            .has_value() ||
                        !destination
                             .stack_offset_candidates[index]
                             .empty()) {
                        destination.stack_offsets[index].reset();
                        destination
                            .stack_offset_candidates[index]
                            .clear();
                        changed = true;
                    }
                } else {
                    const auto previous_offset =
                        destination.stack_offsets[index];
                    const auto previous_candidates =
                        destination
                            .stack_offset_candidates[index];
                    static_cast<void>(
                        set_abi_stack_coordinates(
                            destination,
                            static_cast<std::uint8_t>(index),
                            std::move(merged_coordinates)));
                    changed =
                        previous_offset !=
                            destination.stack_offsets[index] ||
                        previous_candidates !=
                            destination
                                .stack_offset_candidates[index] ||
                        changed;
                }
            }
        } else {
            if ((destination_was_stack_derived ||
                 source.stack_derived[index]) &&
                !destination.stack_tracking_lost) {
                destination.stack_tracking_lost = true;
                changed = true;
            }
            if (destination.stack_offsets[index].has_value() ||
                !destination
                     .stack_offset_candidates[index]
                     .empty()) {
                destination.stack_offsets[index].reset();
                destination
                    .stack_offset_candidates[index]
                    .clear();
                changed = true;
            }
        }
        if (destination.constants[index] != source.constants[index] &&
            destination.constants[index].has_value()) {
            destination.constants[index].reset();
            changed = true;
        }
    }
    std::set<std::int32_t> slots;
    for (const auto& [slot, value] : destination.stack_taints) {
        static_cast<void>(value);
        slots.insert(slot);
    }
    for (const auto& [slot, value] : source.stack_taints) {
        static_cast<void>(value);
        slots.insert(slot);
    }
    if (slots.size() > maximum_abi_persistent_flow_stack_slots) {
        if (!destination.stack_tracking_lost) {
            destination.stack_tracking_lost = true;
            changed = true;
        }
        if (!destination.stack_taints.empty() ||
            !destination.stack_definitely_defined.empty()) {
            destination.stack_taints.clear();
            destination.stack_definitely_defined.clear();
            changed = true;
        }
    } else {
        for (const auto slot : slots) {
            const auto destination_value =
                destination.stack_taints.find(slot);
            const auto source_value =
                source.stack_taints.find(slot);
            auto merged = std::uint8_t{0u};
            if (destination_value !=
                destination.stack_taints.end())
                merged = static_cast<std::uint8_t>(
                    merged | destination_value->second);
            else if (slot >= 0)
                merged = static_cast<std::uint8_t>(
                    merged | abi_stack_argument_taint);
            if (source_value != source.stack_taints.end())
                merged = static_cast<std::uint8_t>(
                    merged | source_value->second);
            else if (slot >= 0)
                merged = static_cast<std::uint8_t>(
                    merged | abi_stack_argument_taint);
            if (destination_value ==
                    destination.stack_taints.end() ||
                destination_value->second != merged) {
                destination.stack_taints[slot] = merged;
                changed = true;
            }
        }
        std::set<std::int32_t> definitely_defined;
        std::set_intersection(
            destination.stack_definitely_defined.begin(),
            destination.stack_definitely_defined.end(),
            source.stack_definitely_defined.begin(),
            source.stack_definitely_defined.end(),
            std::inserter(
                definitely_defined,
                definitely_defined.end()));
        if (definitely_defined !=
            destination.stack_definitely_defined) {
            destination.stack_definitely_defined =
                std::move(definitely_defined);
            changed = true;
        }
    }
    if (source.stack_tracking_lost && !destination.stack_tracking_lost) {
        destination.stack_tracking_lost = true;
        changed = true;
    }
    return changed;
}

// A local, syntactically non-stack store reached by the bounded ABI taint flow.
// It is deliberately a potential GuardedPartial sink, never a CFG edge.  The
// value mask is relative to this function's SuperH C ABI inputs; stack-tainted
// sites remain recorded for reachability but are not eligible for the direct
// register-only shortcut below.
struct AbiPersistentStoreSite {
    std::uint32_t store_instruction_address = 0u;
    std::uint8_t value_sources = 0u;
    std::uint16_t destination_registers = 0u;

    bool operator==(const AbiPersistentStoreSite&) const = default;
};

void normalize_abi_persistent_store_sites(
    std::vector<AbiPersistentStoreSite>& sites) {
    std::sort(sites.begin(), sites.end(), [](const auto& left, const auto& right) {
        return left.store_instruction_address < right.store_instruction_address;
    });
    std::vector<AbiPersistentStoreSite> normalized;
    normalized.reserve(sites.size());
    for (const auto& site : sites) {
        if (site.value_sources == 0u) continue;
        if (!normalized.empty() &&
            normalized.back().store_instruction_address ==
                site.store_instruction_address) {
            normalized.back().value_sources = static_cast<std::uint8_t>(
                normalized.back().value_sources | site.value_sources);
            normalized.back().destination_registers = static_cast<std::uint16_t>(
                normalized.back().destination_registers |
                site.destination_registers);
            continue;
        }
        normalized.push_back(site);
    }
    sites = std::move(normalized);
}

[[nodiscard]] std::uint16_t abi_persistent_store_destination_registers(
    const katana::sh4::DecodedInstruction& instruction) {
    using K = katana::sh4::InstructionKind;
    switch (instruction.kind) {
    case K::MovLongStore:
    case K::MovLongStorePreDecrement:
    case K::MovLongStoreDisplacement:
        return register_bit(instruction.destination_register);
    case K::MovLongStoreR0Indexed:
        return static_cast<std::uint16_t>(
            register_bit(0u) |
            register_bit(instruction.destination_register));
    default:
        return std::uint16_t{0u};
    }
}

struct AbiPersistentStoreSignature {
    // A return mask is complete only when every reached exit is an RTS path.
    // Incomplete/external exits deliberately keep the conservative top mask.
    bool return_sources_complete = false;
    std::uint8_t returned_r0_sources = abi_argument_taint_mask;
    std::uint8_t persistent_store_sources = 0u;
    std::uint8_t indirect_dispatch_sources = 0u;
    AbiStackArgumentReadSet stack_slots_read_before_definition;
    std::vector<AbiPersistentStoreSite> local_persistent_store_sites;
};

using AbiReturnSourceMap = std::unordered_map<std::uint32_t, std::uint8_t>;
using AbiPersistentStoreSourceMap =
    std::unordered_map<std::uint32_t, std::uint8_t>;
using AbiIndirectDispatchSourceMap =
    std::unordered_map<std::uint32_t, std::uint8_t>;
using AbiPersistentStoreSiteMap =
    std::unordered_map<std::uint32_t, std::vector<AbiPersistentStoreSite>>;

[[nodiscard]] std::uint8_t compose_abi_return_taint(
    const AbiPersistentStoreFlowState& state,
    const std::uint8_t return_sources) {
    auto result = std::uint8_t{0u};
    for (std::uint8_t index = 0u; index < 4u; ++index) {
        if ((return_sources & static_cast<std::uint8_t>(1u << index)) != 0u)
            result = static_cast<std::uint8_t>(
                result | state.register_taints[4u + index]);
    }
    if ((return_sources & abi_stack_argument_taint) != 0u)
        result = static_cast<std::uint8_t>(
            result | abi_outgoing_stack_argument_taint(state));
    return static_cast<std::uint8_t>(result & abi_argument_taint_mask);
}

void compose_abi_stack_argument_reads(
    const AbiPersistentStoreFlowState& state,
    const AbiStackArgumentReadSet& callee_reads,
    AbiStackArgumentReadSet& caller_reads,
    AbiStackReadTopReason* const top_reason = nullptr) {
    if (!caller_reads.complete) return;
    if (!callee_reads.complete) {
        set_abi_stack_read_top_reason(
            top_reason, AbiStackReadTopReason::CalleeTop);
        make_abi_stack_argument_reads_unknown(caller_reads);
        return;
    }
    if (callee_reads.slots.empty()) return;
    const auto stack_bases = abi_stack_coordinates(state, 15u);
    if (stack_bases.empty()) {
        set_abi_stack_read_top_reason(
            top_reason, AbiStackReadTopReason::CallerStackCoordinate);
        make_abi_stack_argument_reads_unknown(caller_reads);
        return;
    }
    for (const auto callee_slot : callee_reads.slots) {
        for (const auto stack_base : stack_bases) {
            const auto caller_slot =
                static_cast<std::int64_t>(stack_base) +
                static_cast<std::int64_t>(callee_slot);
            if (caller_slot < -maximum_stack_distance ||
                caller_slot > maximum_stack_distance) {
                set_abi_stack_read_top_reason(
                    top_reason, AbiStackReadTopReason::ComposeRange);
                make_abi_stack_argument_reads_unknown(caller_reads);
                return;
            }
            if (caller_slot >= 0 &&
                !state.stack_definitely_defined.contains(
                    static_cast<std::int32_t>(caller_slot))) {
                static_cast<void>(
                    insert_abi_stack_argument_read(
                        caller_reads, caller_slot, top_reason));
                if (!caller_reads.complete) return;
            }
        }
        // A negative outgoing slot is caller-local. If it contains a value
        // originating in an incoming stack argument, that earlier load has
        // already recorded the exact entry slot in this same signature pass.
    }
}

// Returns the ABI-source mask written by the current instruction to a
// syntactically persistent long-store destination. The dataflow itself remains
// deliberately smaller than the value analysis: it follows only forms that
// preserve a callback value without dereferencing arbitrary object memory.
[[nodiscard]] std::uint8_t apply_abi_persistent_store_flow(
    AbiPersistentStoreFlowState& state,
    const katana::sh4::DisassemblyLine& line,
    const katana::io::ExecutableImage& image,
    AbiStackArgumentReadSet* const entry_stack_reads,
    AbiStackReadTopReason* const top_reason = nullptr) {
    using K = katana::sh4::InstructionKind;
    const auto& instruction = line.instruction;
    const auto source_taint = state.register_taints[instruction.source_register];
    const auto persistent_store = [&](const bool destination_is_stack) {
        return destination_is_stack ? std::uint8_t{0u} : source_taint;
    };
    const auto unknown_stack_load = [&] {
        if (entry_stack_reads != nullptr) {
            set_abi_stack_read_top_reason(
                top_reason, AbiStackReadTopReason::LocalStackCoordinate);
            make_abi_stack_argument_reads_unknown(*entry_stack_reads);
        }
        return abi_argument_taint_mask;
    };
    const auto stack_store = [&](const std::uint8_t base,
                                 const std::int32_t displacement = 0) {
        const auto slots =
            abi_stack_slots(state, base, displacement);
        if (state.stack_derived[base])
            store_abi_stack_taints(
                state, slots, source_taint);
        else if (state.stack_may_alias[base])
            state.stack_tracking_lost = true;
        return persistent_store(state.stack_derived[base]);
    };
    const auto bounded_displacement =
        [](const std::optional<std::uint32_t> value)
            -> std::optional<std::int32_t> {
            if (!value.has_value()) return std::nullopt;
            auto signed_value = static_cast<std::int64_t>(*value);
            if (signed_value > std::numeric_limits<std::int32_t>::max())
                signed_value -= (std::int64_t{1} << 32u);
            if (signed_value < -maximum_stack_distance ||
                signed_value > maximum_stack_distance)
                return std::nullopt;
            return static_cast<std::int32_t>(signed_value);
        };
    const auto r0_indexed_stack_slots =
        [&](const std::uint8_t other_register)
            -> std::vector<std::int32_t> {
            if (other_register == 0u) return {};
            const auto combine =
                [&](const std::uint8_t stack_register,
                    const std::uint8_t displacement_register)
                    -> std::vector<std::int32_t> {
                    if (!state.stack_derived[stack_register] ||
                        state.stack_may_alias[displacement_register])
                        return {};
                    const auto displacement = bounded_displacement(
                        state.constants[displacement_register]);
                    if (!displacement.has_value()) return {};
                    return abi_stack_slots(
                        state, stack_register, *displacement);
                };
            if (auto slots = combine(other_register, 0u);
                !slots.empty())
                return slots;
            return combine(0u, other_register);
        };
    const auto degrade_address_register_to_may_stack =
        [&](const std::uint8_t register_index) {
            if (!state.stack_may_alias[register_index]) {
                clear_abi_flow_register(state, register_index);
                return;
            }
            state.stack_tracking_lost = true;
            state.register_taints[register_index] = 0u;
            state.stack_offsets[register_index].reset();
            state.stack_offset_candidates[register_index].clear();
            state.stack_derived[register_index] = false;
            state.stack_may_alias[register_index] = true;
            state.constants[register_index].reset();
        };
    const auto adjust_address_register =
        [&](const std::uint8_t register_index, const std::int32_t delta) {
            if (state.stack_derived[register_index])
                adjust_abi_stack_offset(state, register_index, delta);
            else if (state.stack_may_alias[register_index])
                degrade_address_register_to_may_stack(register_index);
            else
                clear_abi_flow_register(state, register_index);
        };
    const auto apply_fixed_shift =
        [&](const std::uint8_t register_index,
            const auto operation) {
            const auto constant = state.constants[register_index];
            if (state.stack_may_alias[register_index])
                degrade_address_register_to_may_stack(register_index);
            else
                clear_abi_flow_register(state, register_index);
            if (constant.has_value())
                state.constants[register_index] = operation(*constant);
        };
    const auto overwrite_stack_slot =
        [&](const std::uint8_t base,
            const std::int32_t displacement,
            const std::uint8_t taint,
            const bool definitely_defines_long = false) {
            if (state.stack_derived[base]) {
                store_abi_stack_taints(
                    state,
                    abi_stack_slots(state, base, displacement),
                    taint,
                    definitely_defines_long);
            } else if (state.stack_may_alias[base]) {
                state.stack_tracking_lost = true;
            }
        };
    switch (instruction.kind) {
    case K::MovRegister:
        if (state.stack_may_alias[instruction.destination_register] &&
            !state.stack_may_alias[instruction.source_register])
            state.stack_tracking_lost = true;
        state.register_taints[instruction.destination_register] = source_taint;
        state.stack_offsets[instruction.destination_register] =
            state.stack_offsets[instruction.source_register];
        state.stack_offset_candidates[
            instruction.destination_register] =
            state.stack_offset_candidates[
                instruction.source_register];
        state.stack_derived[instruction.destination_register] =
            state.stack_derived[instruction.source_register];
        state.stack_may_alias[instruction.destination_register] =
            state.stack_may_alias[instruction.source_register];
        state.constants[instruction.destination_register] =
            state.constants[instruction.source_register];
        return std::uint8_t{0u};
    case K::AddImmediate: {
        const auto constant = state.constants[instruction.destination_register];
        if (state.stack_derived[instruction.destination_register])
            adjust_abi_stack_offset(
                state, instruction.destination_register, instruction.immediate);
        else if (state.stack_may_alias[instruction.destination_register])
            degrade_address_register_to_may_stack(
                instruction.destination_register);
        else if (instruction.immediate != 0)
            clear_abi_flow_register(state, instruction.destination_register);
        if (constant.has_value())
            state.constants[instruction.destination_register] =
                *constant + static_cast<std::uint32_t>(instruction.immediate);
        return std::uint8_t{0u};
    }
    case K::AddRegister:
    case K::SubRegister: {
        const auto destination_register = instruction.destination_register;
        const auto source_constant = state.constants[instruction.source_register];
        const auto destination_constant = state.constants[destination_register];
        if (state.stack_derived[destination_register]) {
            if (!source_constant.has_value()) {
                degrade_address_register_to_may_stack(
                    destination_register);
                return std::uint8_t{0u};
            }
            auto delta = static_cast<std::int64_t>(*source_constant);
            if (delta > std::numeric_limits<std::int32_t>::max())
                delta -= (std::int64_t{1} << 32u);
            if (instruction.kind == K::SubRegister) delta = -delta;
            if (delta < std::numeric_limits<std::int32_t>::min() ||
                delta > std::numeric_limits<std::int32_t>::max() ||
                !adjust_abi_stack_offset(
                    state,
                    destination_register,
                    static_cast<std::int32_t>(delta))) {
                degrade_address_register_to_may_stack(
                    destination_register);
                return std::uint8_t{0u};
            }
            state.constants[destination_register].reset();
            return std::uint8_t{0u};
        }
        if (state.stack_may_alias[destination_register]) {
            degrade_address_register_to_may_stack(
                destination_register);
            return std::uint8_t{0u};
        }
        clear_abi_flow_register(state, destination_register);
        if (destination_constant.has_value() && source_constant.has_value()) {
            state.constants[destination_register] =
                instruction.kind == K::AddRegister
                    ? *destination_constant + *source_constant
                    : *destination_constant - *source_constant;
        }
        return std::uint8_t{0u};
    }
    case K::ShiftLogicalLeftOne:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value << 1u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalLeftTwo:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value << 2u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalLeftEight:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value << 8u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalLeftSixteen:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value << 16u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalRightOne:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value >> 1u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalRightTwo:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value >> 2u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalRightEight:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value >> 8u; });
        return std::uint8_t{0u};
    case K::ShiftLogicalRightSixteen:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value >> 16u; });
        return std::uint8_t{0u};
    case K::ShiftArithmeticLeftOne:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) { return value << 1u; });
        return std::uint8_t{0u};
    case K::ShiftArithmeticRightOne:
        apply_fixed_shift(
            instruction.destination_register,
            [](const std::uint32_t value) {
                return static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(value) >> 1);
            });
        return std::uint8_t{0u};
    case K::MovLongStore:
        return stack_store(instruction.destination_register);
    case K::MovByteStore:
    case K::MovWordStore:
        overwrite_stack_slot(instruction.destination_register,
                             0,
                             abi_argument_taint_mask);
        return std::uint8_t{0u};
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement: {
        const auto width =
            instruction.kind == K::MovByteStorePreDecrement ? 1 : 2;
        adjust_address_register(instruction.destination_register, -width);
        overwrite_stack_slot(instruction.destination_register,
                             0,
                             abi_argument_taint_mask);
        return std::uint8_t{0u};
    }
    case K::MovLongStorePreDecrement:
        adjust_address_register(instruction.destination_register, -4);
        return stack_store(instruction.destination_register);
    case K::MovLongStoreDisplacement:
        return stack_store(instruction.destination_register, instruction.displacement);
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
        overwrite_stack_slot(instruction.destination_register,
                             instruction.displacement,
                             abi_argument_taint_mask);
        return std::uint8_t{0u};
    case K::MovLongStoreR0Indexed: {
        if (const auto slots = r0_indexed_stack_slots(
                instruction.destination_register);
            !slots.empty()) {
            store_abi_stack_taints(
                state, slots, source_taint);
            return std::uint8_t{0u};
        }
        if (state.stack_may_alias[0u] ||
            state.stack_may_alias[instruction.destination_register])
            state.stack_tracking_lost = true;
        return source_taint;
    }
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
        if (state.stack_may_alias[0u] ||
            state.stack_may_alias[instruction.destination_register])
            state.stack_tracking_lost = true;
        return std::uint8_t{0u};
    case K::MovLongStoreGbrDisplacement:
        return source_taint;
    case K::MovByteStoreGbrDisplacement:
    case K::MovWordStoreGbrDisplacement:
        return std::uint8_t{0u};
    case K::StoreSpecialRegisterPreDecrement:
        adjust_address_register(instruction.destination_register, -4);
        overwrite_stack_slot(
            instruction.destination_register, 0, 0u, true);
        return std::uint8_t{0u};
    case K::LoadSpecialRegisterPostIncrement:
        if (instruction.special_register ==
            katana::sh4::SpecialRegister::Sr) {
            if (instruction.source_register >= 8u)
                adjust_address_register(
                    instruction.source_register, 4);
            for (std::uint8_t index = 0u; index <= 7u;
                 ++index)
                clear_abi_flow_register(state, index);
        } else {
            adjust_address_register(
                instruction.source_register, 4);
        }
        return std::uint8_t{0u};
    case K::FmovStorePreDecrement:
        // FPSCR.SZ selects a four- or eight-byte transfer. Both bounded stack
        // coordinates remain live until a later instruction reconverges them.
        if (state.stack_derived[
                instruction.destination_register]) {
            constexpr std::array<std::int32_t, 2u>
                decrements{-4, -8};
            if (adjust_abi_stack_offsets(
                    state,
                    instruction.destination_register,
                    decrements))
                overwrite_stack_slot(
                    instruction.destination_register,
                    0,
                    0u);
        } else if (state.stack_may_alias[
                       instruction.destination_register]) {
            degrade_address_register_to_may_stack(
                instruction.destination_register);
        }
        return std::uint8_t{0u};
    case K::FmovLoadPostIncrement:
        if (state.stack_derived[instruction.source_register]) {
            constexpr std::array<std::int32_t, 2u>
                increments{4, 8};
            static_cast<void>(
                adjust_abi_stack_offsets(
                    state,
                    instruction.source_register,
                    increments));
        } else if (state.stack_may_alias[
                       instruction.source_register]) {
            degrade_address_register_to_may_stack(
                instruction.source_register);
        }
        return std::uint8_t{0u};
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong: {
        const auto width =
            instruction.kind == K::MultiplyAccumulateWord ? 2 : 4;
        adjust_address_register(instruction.destination_register, width);
        adjust_address_register(instruction.source_register, width);
        return std::uint8_t{0u};
    }
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement: {
        const auto base = instruction.source_register;
        const auto long_load =
            instruction.kind == K::MovLongLoad ||
            instruction.kind == K::MovLongLoadDisplacement ||
            instruction.kind == K::MovLongLoadPostIncrement;
        const auto displacement =
            instruction.kind == K::MovByteLoadDisplacement ||
                    instruction.kind == K::MovWordLoadDisplacement ||
                    instruction.kind == K::MovLongLoadDisplacement
                ? instruction.displacement
                : 0;
        const auto taint = static_cast<std::uint8_t>(
            long_load
                ? state.stack_derived[base]
                      ? abi_stack_load_taint(
                            state,
                            abi_stack_slots(
                                state, base, displacement),
                            entry_stack_reads,
                            top_reason)
                      : state.stack_may_alias[base]
                            ? unknown_stack_load()
                            : 0u
                : 0u);
        clear_abi_flow_register(state, instruction.destination_register);
        state.register_taints[instruction.destination_register] = taint;
        const auto post_increment =
            instruction.kind == K::MovByteLoadPostIncrement ||
            instruction.kind == K::MovWordLoadPostIncrement ||
            instruction.kind == K::MovLongLoadPostIncrement;
        if (post_increment &&
            instruction.source_register != instruction.destination_register) {
            const auto width =
                instruction.kind == K::MovByteLoadPostIncrement   ? 1
                : instruction.kind == K::MovWordLoadPostIncrement ? 2
                                                                   : 4;
            adjust_address_register(instruction.source_register, width);
        }
        return std::uint8_t{0u};
    }
    case K::MovLongLoadR0Indexed: {
        const auto slots =
            r0_indexed_stack_slots(
                instruction.source_register);
        const auto taint =
            !slots.empty()
                ? abi_stack_load_taint(
                      state, slots, entry_stack_reads, top_reason)
                : state.stack_may_alias[0u] ||
                          state.stack_may_alias[instruction.source_register]
                      ? unknown_stack_load()
                      : std::uint8_t{0u};
        clear_abi_flow_register(state, instruction.destination_register);
        state.register_taints[instruction.destination_register] = taint;
        return std::uint8_t{0u};
    }
    case K::MovLongLoadGbrDisplacement:
        clear_abi_flow_register(state, instruction.destination_register);
        return std::uint8_t{0u};
    case K::MovLongLoadPcRelative:
    case K::MovWordLoadPcRelative: {
        const auto width =
            instruction.kind == K::MovWordLoadPcRelative ? 2u : 4u;
        const auto base =
            width == 4u ? (line.address + 4u) & ~3u : line.address + 4u;
        const auto loaded = read_image_value(
            image,
            base + static_cast<std::uint32_t>(instruction.displacement),
            width);
        clear_abi_flow_register(state, instruction.destination_register);
        if (loaded.has_value())
            state.constants[instruction.destination_register] = loaded->value;
        return std::uint8_t{0u};
    }
    case K::MoveAddressPcRelative:
        clear_abi_flow_register(state, instruction.destination_register);
        state.constants[instruction.destination_register] =
            ((line.address + 4u) & ~3u) +
            static_cast<std::uint32_t>(instruction.displacement);
        return std::uint8_t{0u};
    case K::MovImmediate:
        clear_abi_flow_register(state, instruction.destination_register);
        state.constants[instruction.destination_register] =
            static_cast<std::uint32_t>(instruction.immediate);
        return std::uint8_t{0u};
    default: {
        const auto written_registers = general_register_write_mask(instruction);
        for (std::uint8_t index = 0u; index < state.register_taints.size(); ++index) {
            if ((written_registers & register_bit(index)) == 0u)
                continue;
            if (state.stack_may_alias[index])
                degrade_address_register_to_may_stack(index);
            else
                clear_abi_flow_register(state, index);
        }
        return std::uint8_t{0u};
    }
    }
}

[[nodiscard]] AbiPersistentStoreSignature
analyze_abi_persistent_store_signature(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const AbiReturnSourceMap* const return_sources = nullptr,
    const AbiPersistentStoreSourceMap* const persistent_store_sources = nullptr,
    const AbiIndirectDispatchSourceMap* const indirect_dispatch_sources = nullptr,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>* const
        inventory_indirect_callees = nullptr,
    const AbiStackArgumentReadMap* const abi_stack_argument_reads = nullptr,
    const TailIngressMap* const inventory_tail_ingresses = nullptr) {
    std::unordered_set<std::uint32_t> members;
    members.reserve(function.block_addresses.size());
    members.insert(function.block_addresses.begin(), function.block_addresses.end());
    if (!members.contains(function.entry_address)) return {};

    AbiPersistentStoreFlowState entry;
    for (std::uint8_t index = 4u; index <= 7u; ++index)
        entry.register_taints[index] = static_cast<std::uint8_t>(1u << (index - 4u));
    entry.stack_offsets[15u] = 0;
    entry.stack_derived[15u] = true;
    entry.stack_may_alias[15u] = true;

    std::unordered_map<std::uint32_t, AbiPersistentStoreFlowState> inputs;
    inputs.reserve(function.block_addresses.size());
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> queued;
    queued.reserve(function.block_addresses.size());
    inputs.emplace(function.entry_address, entry);
    pending.push_back(function.entry_address);
    queued.insert(function.entry_address);
    AbiPersistentStoreSignature signature;
    signature.returned_r0_sources = 0u;
    bool saw_return = false;
    bool unknown_return_path = false;
    const auto make_stack_read_top_frame =
        [&](const AbiStackReadTopReason reason,
            const std::uint32_t site) {
            AbiStackReadTopFrame frame;
            frame.reason = reason;
            frame.owner = function.entry_address;
            frame.site = site;
            return frame;
        };
    const auto record_stack_read_top_transition =
        [&](const bool was_complete,
            const AbiStackReadTopFrame frame,
            const AbiStackArgumentReadSet* const parent) {
            if (was_complete &&
                !signature.stack_slots_read_before_definition.complete)
                record_abi_stack_read_top(
                    signature.stack_slots_read_before_definition,
                    frame,
                    parent);
        };
    const auto mark_stack_reads_top =
        [&](const AbiStackReadTopFrame frame,
            const AbiStackArgumentReadSet* const parent) {
            const auto was_complete =
                signature.stack_slots_read_before_definition.complete;
            make_abi_stack_argument_reads_unknown(
                signature.stack_slots_read_before_definition);
            record_stack_read_top_transition(
                was_complete, frame, parent);
        };

    struct DelayedCall {
        std::uint32_t call_site = 0u;
        std::optional<std::uint32_t> return_callee;
        std::vector<std::uint32_t> inventory_callees;
        bool stack_reads_complete = true;
        bool stack_reads_guarded = false;
    };
    const auto apply_call_return =
        [&](AbiPersistentStoreFlowState& state,
            const std::optional<std::uint32_t> callee) {
            auto source_mask = abi_argument_taint_mask;
            if (callee.has_value() && return_sources != nullptr) {
                if (const auto found = return_sources->find(*callee);
                    found != return_sources->end())
                    source_mask = found->second;
            }
            const auto returned_taint = compose_abi_return_taint(state, source_mask);
            for (std::uint8_t index = 0u; index <= 7u; ++index)
                clear_abi_flow_register(state, index);
            state.register_taints[0u] = returned_taint;
        };
    const auto observe_inventory_callee_sinks =
        [&](const AbiPersistentStoreFlowState& state,
            const std::vector<std::uint32_t>& callees) {
            for (const auto callee : callees) {
                if (persistent_store_sources != nullptr) {
                    const auto found = persistent_store_sources->find(callee);
                    if (found != persistent_store_sources->end() &&
                        found->second != 0u) {
                        signature.persistent_store_sources =
                            static_cast<std::uint8_t>(
                                signature.persistent_store_sources |
                                compose_abi_return_taint(state, found->second));
                    }
                }
                if (indirect_dispatch_sources != nullptr) {
                    const auto found = indirect_dispatch_sources->find(callee);
                    if (found != indirect_dispatch_sources->end() &&
                        found->second != 0u) {
                        signature.indirect_dispatch_sources =
                            static_cast<std::uint8_t>(
                                signature.indirect_dispatch_sources |
                                compose_abi_return_taint(state, found->second));
                    }
                }
            }
        };
    const auto observe_callee_stack_reads =
        [&](const AbiPersistentStoreFlowState& state,
            const std::uint32_t site,
            const std::vector<std::uint32_t>& callees,
            const bool complete,
            const bool guarded,
            const IndirectCalleeCandidates* const ingress) {
            const auto observation_frame =
                [&](const AbiStackReadTopReason reason,
                    const std::uint32_t target) {
                    auto frame =
                        make_stack_read_top_frame(reason, site);
                    frame.target = target;
                    frame.ingress_present = ingress != nullptr;
                    if (ingress != nullptr) {
                        frame.ingress_guarded = ingress->guarded;
                        frame.ingress_complete = ingress->complete;
                    }
                    return frame;
                };
            // A guarded-partial inventory edge is target-complete within the
            // runtime guard for each concrete candidate.  Its global target
            // enumeration remains partial, but that must not turn a known
            // candidate-conditioned ABI contract into Top. Unguarded partial
            // calls still retain the conservative legacy behavior.
            if (!complete && !guarded) {
                mark_stack_reads_top(
                    observation_frame(
                        AbiStackReadTopReason::CalleeSetIncomplete, 0u),
                    nullptr);
                return;
            }
            if (callees.empty()) {
                mark_stack_reads_top(
                    observation_frame(
                        AbiStackReadTopReason::EmptyCalleeSet, 0u),
                    nullptr);
                return;
            }
            if (abi_stack_argument_reads == nullptr) {
                mark_stack_reads_top(
                    observation_frame(
                        AbiStackReadTopReason::ReadMapUnavailable, 0u),
                    nullptr);
                return;
            }
            for (const auto callee : callees) {
                const auto found =
                    abi_stack_argument_reads->find(callee);
                if (found == abi_stack_argument_reads->end()) {
                    auto frame = observation_frame(
                        AbiStackReadTopReason::CalleeMissing, callee);
                    frame.contract_present = false;
                    mark_stack_reads_top(frame, nullptr);
                    return;
                }
                const auto was_complete =
                    signature.stack_slots_read_before_definition.complete;
                auto top_reason = AbiStackReadTopReason::None;
                compose_abi_stack_argument_reads(
                    state,
                    found->second,
                    signature.stack_slots_read_before_definition,
                    &top_reason);
                if (!signature.stack_slots_read_before_definition.complete) {
                    auto frame = observation_frame(
                        top_reason == AbiStackReadTopReason::None
                            ? AbiStackReadTopReason::CalleeTop
                            : top_reason,
                        callee);
                    frame.contract_present = true;
                    frame.contract_complete = found->second.complete;
                    record_stack_read_top_transition(
                        was_complete,
                        frame,
                        top_reason == AbiStackReadTopReason::CalleeTop
                            ? &found->second
                            : nullptr);
                    return;
                }
            }
        };

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        const auto block = blocks.find(address);
        if (block == blocks.end() || block->second->lines.empty()) {
            mark_stack_reads_top(
                make_stack_read_top_frame(
                    AbiStackReadTopReason::MissingBlock, address),
                nullptr);
            unknown_return_path = true;
            continue;
        }
        auto state = inputs.at(address);
        std::optional<DelayedCall> delayed_call;
        for (const auto& line : block->second->lines) {
            const auto indirect_dispatch =
                line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
            if (indirect_dispatch) {
                signature.indirect_dispatch_sources =
                    static_cast<std::uint8_t>(
                        signature.indirect_dispatch_sources |
                        state.register_taints[
                            line.instruction.branch_register]);
            }
            const auto stack_reads_were_complete =
                signature.stack_slots_read_before_definition.complete;
            auto local_top_reason = AbiStackReadTopReason::None;
            const auto local_store_sources =
                apply_abi_persistent_store_flow(
                    state,
                    line,
                    image,
                    &signature.stack_slots_read_before_definition,
                    &local_top_reason);
            record_stack_read_top_transition(
                stack_reads_were_complete,
                make_stack_read_top_frame(
                    local_top_reason == AbiStackReadTopReason::None
                        ? AbiStackReadTopReason::LocalStackCoordinate
                        : local_top_reason,
                    line.address),
                nullptr);
            signature.persistent_store_sources = static_cast<std::uint8_t>(
                signature.persistent_store_sources | local_store_sources);
            if (local_store_sources != 0u) {
                signature.local_persistent_store_sites.push_back(
                    {line.address,
                     local_store_sources,
                     abi_persistent_store_destination_registers(line.instruction)});
            }
            // The delay slot writes the actual outgoing ABI arguments. Sample
            // the inventory-only store slice after it, before the call return
            // clobbers the caller-visible registers.
            if (delayed_call.has_value()) {
                observe_inventory_callee_sinks(
                    state, delayed_call->inventory_callees);
                observe_callee_stack_reads(
                    state,
                    delayed_call->call_site,
                    delayed_call->inventory_callees,
                    delayed_call->stack_reads_complete,
                    delayed_call->stack_reads_guarded,
                    nullptr);
                apply_call_return(state, delayed_call->return_callee);
                delayed_call.reset();
            }
            const auto call =
                line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall;
            if (call) {
                const auto return_callee =
                    line.instruction.control_flow == katana::sh4::ControlFlowKind::Call
                        ? line.target_address
                        : std::optional<std::uint32_t>{};
                std::vector<std::uint32_t> inventory_callees;
                auto stack_reads_complete = true;
                auto stack_reads_guarded = false;
                if (return_callee.has_value()) {
                    inventory_callees.push_back(*return_callee);
                } else if (inventory_indirect_callees != nullptr) {
                    if (const auto candidates =
                            inventory_indirect_callees->find(line.address);
                        candidates != inventory_indirect_callees->end()) {
                        inventory_callees = candidates->second.targets;
                        stack_reads_complete =
                            candidates->second.complete;
                        stack_reads_guarded =
                            candidates->second.guarded;
                    } else {
                        stack_reads_complete = false;
                    }
                } else {
                    stack_reads_complete = false;
                }
                if (line.instruction.has_delay_slot) {
                    delayed_call = DelayedCall{
                        line.address,
                        return_callee,
                        std::move(inventory_callees),
                        stack_reads_complete,
                        stack_reads_guarded};
                } else {
                    observe_inventory_callee_sinks(state, inventory_callees);
                    observe_callee_stack_reads(
                        state,
                        line.address,
                        inventory_callees,
                        stack_reads_complete,
                        stack_reads_guarded,
                        nullptr);
                    apply_call_return(state, return_callee);
                }
            }
        }
        const auto& control = controlling_line(*block->second);
        const auto control_position = std::find_if(
            block->second->lines.begin(),
            block->second->lines.end(),
            [&control](const auto& line) {
                return line.address == control.address;
            });
        const auto paired_delay_slot =
            !control.instruction.has_delay_slot ||
            (control_position != block->second->lines.end() &&
             std::next(control_position) !=
                 block->second->lines.end() &&
             std::next(control_position)->is_delay_slot &&
             std::next(control_position)->address ==
                 control.address + 2u);
        if (delayed_call.has_value() || !paired_delay_slot)
            mark_stack_reads_top(
                make_stack_read_top_frame(
                    AbiStackReadTopReason::MissingDelay,
                    delayed_call.has_value()
                        ? delayed_call->call_site
                        : control.address),
                nullptr);
        const auto ingress =
            inventory_tail_ingresses == nullptr
                ? TailIngressMap::const_iterator{}
                : inventory_tail_ingresses->find(control.address);
        const auto has_contract_ingress =
            inventory_tail_ingresses != nullptr &&
            ingress != inventory_tail_ingresses->end();
        if (has_contract_ingress) {
            observe_callee_stack_reads(
                state,
                control.address,
                ingress->second.targets,
                ingress->second.complete,
                ingress->second.guarded,
                &ingress->second);
        }
        if (control.instruction.control_flow ==
                katana::sh4::ControlFlowKind::UnconditionalBranch ||
            control.instruction.control_flow ==
                katana::sh4::ControlFlowKind::ConditionalBranch ||
            control.instruction.control_flow ==
                katana::sh4::ControlFlowKind::IndirectBranch) {
            if (!has_contract_ingress) {
                const auto has_external_successor =
                    std::any_of(
                        block->second->successors.begin(),
                        block->second->successors.end(),
                        [&](const auto successor) {
                            return !members.contains(successor);
                        });
                if (control.instruction.control_flow ==
                        katana::sh4::ControlFlowKind::IndirectBranch ||
                    has_external_successor) {
                    auto frame = make_stack_read_top_frame(
                        control.instruction.control_flow ==
                                katana::sh4::ControlFlowKind::IndirectBranch
                            ? AbiStackReadTopReason::IndirectNoIngress
                            : AbiStackReadTopReason::
                                  ExternalSuccessorNoIngress,
                        control.address);
                    frame.residual_indirect =
                        block->second->has_indirect_successor;
                    frame.external_successor =
                        has_external_successor;
                    mark_stack_reads_top(frame, nullptr);
                }
            }
        }
        // SH-4 executes the delay slot before publishing a return value.
        // Sampling after the complete block also keeps `rts; mov r4,r0`
        // visible to the interprocedural ABI-return summary.
        if (control.instruction.kind == katana::sh4::InstructionKind::Rts) {
            saw_return = true;
            signature.returned_r0_sources = static_cast<std::uint8_t>(
                signature.returned_r0_sources | state.register_taints[0u]);
        } else {
            bool has_internal_successor = false;
            for (const auto successor : block->second->successors) {
                if (members.contains(successor)) {
                    has_internal_successor = true;
                    continue;
                }
                // Direct and indirect call targets leave the caller's function
                // only temporarily. Every other external successor is a tail,
                // exception or unknown exit and cannot narrow a return mask.
                if (control.instruction.control_flow !=
                        katana::sh4::ControlFlowKind::Call &&
                    control.instruction.control_flow !=
                        katana::sh4::ControlFlowKind::IndirectCall)
                    unknown_return_path = true;
            }
            if (!has_internal_successor) unknown_return_path = true;
        }
        for (const auto successor : block->second->successors) {
            if (!members.contains(successor)) continue;
            const auto [input, inserted] = inputs.emplace(successor, state);
            const auto merged = !inserted &&
                                merge_abi_persistent_store_flow_state(input->second, state);
            if ((inserted || merged) && queued.insert(successor).second)
                pending.push_back(successor);
        }
    }
    signature.return_sources_complete = saw_return && !unknown_return_path;
    if (!signature.return_sources_complete)
        signature.returned_r0_sources = abi_argument_taint_mask;
    normalize_abi_persistent_store_sites(signature.local_persistent_store_sites);
    return signature;
}

[[nodiscard]] bool function_forwards_abi_argument_to_persistent_inventory_store(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks) {
    return analyze_abi_persistent_store_signature(image, function, blocks)
               .persistent_store_sources != 0u;
}

bool function_contains_non_stack_inventory_store_shape(
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks) {
    for (const auto block_address : function.block_addresses) {
        const auto block = blocks.find(block_address);
        if (block == blocks.end()) continue;
        if (std::any_of(
                block->second->lines.begin(),
                block->second->lines.end(),
                [](const auto& line) {
                    return guarded_inventory_store_instruction(
                               line.instruction.kind) &&
                           line.instruction.destination_register != 15u;
                }))
            return true;
    }
    return false;
}

constexpr std::uint8_t abi_entry_register_mask = 0x0Fu;
constexpr std::uint16_t abi_entry_general_register_mask =
    register_bit(4u) | register_bit(5u) |
    register_bit(6u) | register_bit(7u);

[[nodiscard]] constexpr std::uint8_t
abi_entry_register_bit(const std::uint8_t register_index) {
    return register_index >= 4u && register_index <= 7u
               ? static_cast<std::uint8_t>(1u << (register_index - 4u))
               : 0u;
}

[[nodiscard]] constexpr std::uint8_t
abi_entry_register_bits(const std::uint16_t general_register_mask) {
    auto result = std::uint8_t{0u};
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        if ((general_register_mask & register_bit(index)) != 0u)
            result = static_cast<std::uint8_t>(result | abi_entry_register_bit(index));
    }
    return result;
}

// This intentionally only returns an exact mask for instructions whose general
// register operands are fully known here. Any omitted or uncertain form returns
// nullopt and therefore retains every entry ABI register.
[[nodiscard]] std::optional<std::uint16_t>
known_general_register_read_mask(const katana::sh4::DecodedInstruction& instruction) {
    using K = katana::sh4::InstructionKind;
    switch (instruction.kind) {
    case K::Nop:
    case K::Rts:
    case K::MovImmediate:
    case K::MoveT:
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
    case K::MoveAddressPcRelative:
    case K::StoreSpecialRegister:
    case K::ClearMac:
    case K::DivideInitializeUnsigned:
    case K::ClearS:
    case K::SetS:
    case K::ClearT:
    case K::SetT:
    case K::Bra:
    case K::Bsr:
    case K::Bt:
    case K::Bf:
    case K::BtS:
    case K::BfS:
    case K::Fldi0:
    case K::Fldi1:
    case K::Flds:
    case K::Fsts:
    case K::Fabs:
    case K::Fadd:
    case K::FcmpEqual:
    case K::FcmpGreater:
    case K::Fdiv:
    case K::FloatFromFpul:
    case K::Fmac:
    case K::Fmul:
    case K::Fneg:
    case K::Fsqrt:
    case K::Fsrra:
    case K::Fsca:
    case K::Fipr:
    case K::Ftrv:
    case K::Fsub:
    case K::Ftrc:
    case K::FcnvDoubleToSingle:
    case K::FcnvSingleToDouble:
    case K::Frchg:
    case K::Fschg:
        return std::uint16_t{0u};

    case K::MovRegister:
    case K::NegateRegister:
    case K::NotRegister:
    case K::NegateWithCarry:
    case K::ExtendUnsignedByte:
    case K::ExtendUnsignedWord:
    case K::ExtendSignedByte:
    case K::ExtendSignedWord:
    case K::SwapBytes:
    case K::SwapWords:
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::LoadSpecialRegister:
    case K::LoadSpecialRegisterPostIncrement:
    case K::FmovLoad:
    case K::FmovLoadPostIncrement:
        return register_bit(instruction.source_register);

    case K::AddImmediate:
    case K::DecrementAndTest:
    case K::ShiftLogicalLeftOne:
    case K::ShiftLogicalRightOne:
    case K::ShiftArithmeticLeftOne:
    case K::ShiftArithmeticRightOne:
    case K::ShiftLogicalLeftTwo:
    case K::ShiftLogicalLeftEight:
    case K::ShiftLogicalLeftSixteen:
    case K::ShiftLogicalRightTwo:
    case K::ShiftLogicalRightEight:
    case K::ShiftLogicalRightSixteen:
    case K::RotateLeft:
    case K::RotateRight:
    case K::RotateLeftThroughT:
    case K::RotateRightThroughT:
    case K::ComparePositiveOrZero:
    case K::ComparePositive:
    case K::TestAndSetByte:
    case K::StoreSpecialRegisterPreDecrement:
        return register_bit(instruction.destination_register);

    case K::AddRegister:
    case K::SubRegister:
    case K::AddWithCarry:
    case K::AddWithOverflow:
    case K::SubWithCarry:
    case K::SubWithOverflow:
    case K::ExtractMiddle:
    case K::ShiftArithmeticDynamic:
    case K::ShiftLogicalDynamic:
    case K::MultiplyLong:
    case K::MultiplySignedWord:
    case K::MultiplyUnsignedWord:
    case K::DoubleMultiplySignedLong:
    case K::DoubleMultiplyUnsignedLong:
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong:
    case K::DivideInitializeSigned:
    case K::DivideStep:
    case K::AndRegister:
    case K::OrRegister:
    case K::XorRegister:
    case K::CompareEqualRegister:
    case K::CompareHigherOrSame:
    case K::CompareGreaterOrEqual:
    case K::CompareHigher:
    case K::CompareGreaterThan:
    case K::CompareString:
    case K::TestRegister:
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        return static_cast<std::uint16_t>(register_bit(instruction.source_register) |
                                          register_bit(instruction.destination_register));

    case K::AndImmediate:
    case K::OrImmediate:
    case K::XorImmediate:
    case K::CompareEqualImmediate:
    case K::TestImmediate:
    case K::TestByteImmediate:
    case K::AndByteImmediate:
    case K::XorByteImmediate:
    case K::OrByteImmediate:
        return register_bit(0u);

    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed:
        return static_cast<std::uint16_t>(register_bit(0u) |
                                          register_bit(instruction.source_register) |
                                          register_bit(instruction.destination_register));

    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::FmovLoadR0Indexed:
        return static_cast<std::uint16_t>(register_bit(0u) |
                                          register_bit(instruction.source_register));

    case K::MovByteStoreGbrDisplacement:
    case K::MovWordStoreGbrDisplacement:
    case K::MovLongStoreGbrDisplacement:
        return register_bit(instruction.source_register);

    case K::MovByteLoadGbrDisplacement:
    case K::MovWordLoadGbrDisplacement:
    case K::MovLongLoadGbrDisplacement:
        return std::uint16_t{0u};

    case K::Braf:
    case K::Bsrf:
    case K::Jmp:
    case K::Jsr:
        return register_bit(instruction.branch_register);

    case K::Unknown:
    default:
        return std::nullopt;
    }
}

using ForwardedRegisterReadMap =
    std::unordered_map<std::uint32_t, std::uint16_t>;

[[nodiscard]] std::uint16_t entry_register_read_before_def_mask(
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const ForwardedRegisterReadMap* const forwarded_register_reads,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>* const
        inventory_indirect_callees,
    const TailIngressMap* const inventory_tail_ingresses) {
    std::unordered_set<std::uint32_t> members;
    members.reserve(function.block_addresses.size());
    members.insert(function.block_addresses.begin(), function.block_addresses.end());
    if (!members.contains(function.entry_address))
        return std::numeric_limits<std::uint16_t>::max();

    std::unordered_map<std::uint32_t, std::uint16_t> incoming_definitions;
    incoming_definitions.reserve(function.block_addresses.size());
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> queued;
    queued.reserve(function.block_addresses.size());
    incoming_definitions.emplace(function.entry_address, std::uint16_t{0u});
    pending.push_back(function.entry_address);
    queued.insert(function.entry_address);
    auto reads = std::uint16_t{0u};

    const auto retain_undefined_registers =
        [&](const std::uint16_t mask,
            const std::uint16_t definitions) {
            reads = static_cast<std::uint16_t>(
                reads | (mask & static_cast<std::uint16_t>(~definitions)));
        };
    const auto target_register_reads =
        [&](const std::span<const std::uint32_t> targets,
            const bool complete,
            const bool guarded) {
            // This map is only a candidate-conditioned pruning contract for
            // guarded inventory contexts. A known target behind its runtime
            // guard can therefore contribute its exact live-ins even when the
            // site's global target enumeration is partial.
            if ((!complete && !guarded) || targets.empty() ||
                forwarded_register_reads == nullptr)
                return std::numeric_limits<std::uint16_t>::max();
            auto target_reads = std::uint16_t{0u};
            for (const auto target : targets) {
                const auto found = forwarded_register_reads->find(target);
                if (found == forwarded_register_reads->end())
                    return std::numeric_limits<std::uint16_t>::max();
                target_reads = static_cast<std::uint16_t>(
                    target_reads | found->second);
            }
            return target_reads;
        };
    struct RegisterCallContract {
        std::vector<std::uint32_t> targets;
        bool complete = false;
        bool guarded = false;
    };
    const auto call_contract =
        [&](const katana::sh4::DisassemblyLine& line) {
            RegisterCallContract contract;
            if (line.target_address.has_value()) {
                contract.targets.push_back(*line.target_address);
                contract.complete = true;
            } else if (inventory_indirect_callees != nullptr) {
                const auto candidates =
                    inventory_indirect_callees->find(line.address);
                if (candidates != inventory_indirect_callees->end()) {
                    contract.targets = candidates->second.targets;
                    contract.complete = candidates->second.complete;
                    contract.guarded = candidates->second.guarded;
                }
            }
            return contract;
        };
    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        const auto block = blocks.find(address);
        if (block == blocks.end() || block->second->lines.empty())
            return std::numeric_limits<std::uint16_t>::max();
        auto definitions = incoming_definitions.at(address);
        std::optional<RegisterCallContract> delayed_call;
        for (const auto& line : block->second->lines) {
            const auto instruction_reads = known_general_register_read_mask(line.instruction);
            if (!instruction_reads.has_value())
                return std::numeric_limits<std::uint16_t>::max();
            retain_undefined_registers(*instruction_reads, definitions);
            definitions = static_cast<std::uint16_t>(
                definitions |
                general_register_write_mask(line.instruction));
            if (delayed_call.has_value()) {
                retain_undefined_registers(
                    target_register_reads(
                        delayed_call->targets,
                        delayed_call->complete,
                        delayed_call->guarded),
                    definitions);
                definitions = static_cast<std::uint16_t>(
                    definitions | 0x00FFu);
                delayed_call.reset();
            }
            const auto call =
                line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::IndirectCall;
            if (call) {
                auto contract = call_contract(line);
                if (line.instruction.has_delay_slot) {
                    delayed_call = std::move(contract);
                } else {
                    retain_undefined_registers(
                        target_register_reads(
                            contract.targets,
                            contract.complete,
                            contract.guarded),
                        definitions);
                    definitions = static_cast<std::uint16_t>(
                        definitions | 0x00FFu);
                }
            }
            if (reads == std::numeric_limits<std::uint16_t>::max())
                return reads;
        }

        const auto& control = controlling_line(*block->second);
        const auto& instruction = control.instruction;
        const auto control_position = std::find_if(
            block->second->lines.begin(), block->second->lines.end(),
            [&control](const auto& line) { return line.address == control.address; });
        const auto paired_delay_slot =
            !instruction.has_delay_slot ||
            (control_position != block->second->lines.end() &&
             std::next(control_position) != block->second->lines.end() &&
             std::next(control_position)->is_delay_slot &&
             std::next(control_position)->address == control.address + 2u);
        if (!paired_delay_slot)
            return std::numeric_limits<std::uint16_t>::max();
        if (delayed_call.has_value())
            return std::numeric_limits<std::uint16_t>::max();

        const auto internal_successor =
            [&members](const std::uint32_t successor) { return members.contains(successor); };
        const auto has_external_successor =
            std::any_of(block->second->successors.begin(), block->second->successors.end(),
                        [&internal_successor](const auto successor) {
                            return !internal_successor(successor);
                        });
        const auto ingress =
            inventory_tail_ingresses == nullptr
                ? TailIngressMap::const_iterator{}
                : inventory_tail_ingresses->find(control.address);
        const auto has_contract_ingress =
            inventory_tail_ingresses != nullptr &&
            ingress != inventory_tail_ingresses->end();
        if (has_contract_ingress) {
            retain_undefined_registers(
                target_register_reads(
                    ingress->second.targets,
                    ingress->second.complete,
                    ingress->second.guarded),
                definitions);
        }
        if (reads == std::numeric_limits<std::uint16_t>::max())
            return reads;
        switch (instruction.control_flow) {
        case katana::sh4::ControlFlowKind::Call:
        case katana::sh4::ControlFlowKind::IndirectCall:
            // The call contract and r0-r7 clobber were applied immediately
            // after the paired delay slot above.
            break;
        case katana::sh4::ControlFlowKind::IndirectBranch:
        case katana::sh4::ControlFlowKind::UnconditionalBranch:
        case katana::sh4::ControlFlowKind::ConditionalBranch: {
            if (!has_contract_ingress &&
                (instruction.control_flow ==
                     katana::sh4::ControlFlowKind::IndirectBranch ||
                 block->second->has_indirect_successor ||
                 has_external_successor ||
                 block->second->successors.empty())) {
                retain_undefined_registers(
                    std::numeric_limits<std::uint16_t>::max(),
                    definitions);
            }
            break;
        }
        case katana::sh4::ControlFlowKind::Trap:
        case katana::sh4::ControlFlowKind::ExceptionReturn:
        case katana::sh4::ControlFlowKind::Halt:
            retain_undefined_registers(
                std::numeric_limits<std::uint16_t>::max(),
                definitions);
            break;
        case katana::sh4::ControlFlowKind::Return:
            retain_undefined_registers(register_bit(0u), definitions);
            break;
        case katana::sh4::ControlFlowKind::None:
            if (!has_contract_ingress &&
                (block->second->has_indirect_successor ||
                 block->second->successors.empty()))
                retain_undefined_registers(
                    std::numeric_limits<std::uint16_t>::max(),
                    definitions);
            break;
        }
        if (reads == std::numeric_limits<std::uint16_t>::max())
            return reads;
        for (const auto successor : block->second->successors) {
            if (!members.contains(successor)) continue;
            const auto [stored, inserted] = incoming_definitions.emplace(successor, definitions);
            const auto merged = !inserted &&
                                stored->second != static_cast<std::uint16_t>(
                                                      stored->second & definitions);
            if (merged)
                stored->second = static_cast<std::uint16_t>(
                    stored->second & definitions);
            if ((inserted || merged) && queued.insert(successor).second)
                pending.push_back(successor);
        }
    }
    return reads;
}

AbstractState isolated_store_input(const std::uint32_t call_site,
                                   const AbstractState& observation,
                                   const std::uint16_t entry_read_mask =
                                       abi_entry_general_register_mask,
                                   const bool retain_stack_inputs = true,
                                   const AbiStackArgumentReadSet* const
                                       required_stack_reads = nullptr) {
    AbstractState input;
    input.stack_offsets[15u] = 0;
    input.inventory_unresolved_saved_stack_alias_sources =
        retain_stack_inputs
            ? observation
                  .inventory_unresolved_saved_stack_alias_sources
            : 0u;
    input.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        retain_stack_inputs &&
        observation
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
    input.inventory_current_stack_epoch_alias_watcher =
        retain_stack_inputs &&
        observation.inventory_current_stack_epoch_alias_watcher;
    input.inventory_detached_stack_epoch_alias_watcher =
        retain_stack_inputs &&
        observation.inventory_detached_stack_epoch_alias_watcher;
    input.inventory_unresolved_stack_callback_loss =
        retain_stack_inputs &&
        observation.inventory_unresolved_stack_callback_loss;
    input.inventory_stack_callback_loss_identity_truncated =
        observation
            .inventory_stack_callback_loss_identity_truncated;
    for (std::uint8_t index = 0u; index < observation.size(); ++index) {
        if ((entry_read_mask & register_bit(index)) == 0u) continue;
        input[index] = observation[index];
        input[index].complete = false;
        input[index].guarded = input[index].known;
        input[index].call_sites.insert(call_site);
        input.stack_offsets[index] =
            callee_relative_stack_offset(observation, index);
        static_cast<void>(set_inventory_stack_coordinates(
            input,
            index,
            inventory_stack_slots(observation, index)));
        input.stack_may_alias[index] = observation.stack_may_alias[index];
        input.inventory_stack_may_alias[index] =
            observation.inventory_stack_may_alias[index];
        input.inventory_vbr_relative[index] =
            observation.inventory_vbr_relative[index];
        input.inventory_fixed_storage_reference[index] =
            observation.inventory_fixed_storage_reference[index];
        if (has_inventory_candidate_values(input[index]) ||
            input[index].values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(input[index]);
    }
    if (retain_stack_inputs) {
        if (required_stack_reads != nullptr &&
            required_stack_reads->complete) {
            for (const auto& [slot, value] :
                 observation.stack_values) {
                static_cast<void>(slot);
                if (has_saved_stack_epoch(value) &&
                    value.inventory_saved_stack_epoch
                        .tracks_current_epoch)
                    input.inventory_current_stack_epoch_alias_watcher =
                        true;
            }
            for (const auto slot : required_stack_reads->slots) {
                const auto value = observation.stack_values.find(slot);
                if (value != observation.stack_values.end())
                    input.stack_values.emplace(slot, value->second);
            }
        } else {
            input.stack_values = observation.stack_values;
            collapse_payload_free_stack_aliases(input);
        }
        for (auto& [slot, value] : input.stack_values) {
            static_cast<void>(slot);
            value.complete = false;
            value.guarded = value.known;
            value.call_sites.insert(call_site);
            if (has_inventory_candidate_values(value) ||
                value.values.size() > maximum_summary_values)
                make_unknown_preserving_provenance(value);
        }
    }
    // Scalar persistent-memory facts normally come from the global summary.
    // Saved-stack provenance cannot: it records either an exact finite epoch
    // or the exact cell where that epoch became unresolved. Preserve only
    // those cells so a later isolated helper can restore the finite payload or
    // fail closed on the true marker without broadening the rest of context.
    for (const auto& [address, value] : observation.memory_values) {
        if (!has_saved_stack_epoch(value) &&
            !carries_unresolved_stack_callback(value))
            continue;
        auto retained = value;
        make_unknown_preserving_provenance(retained);
        retained.guarded = true;
        retained.complete = false;
        retained.call_sites.insert(call_site);
        input.memory_values.emplace(address, std::move(retained));
    }
    // This is deliberately post-admission canonicalization. The caller has
    // already proved that an isolated harvest is required. ABI registers
    // proven dead before their first read are erased; forwarded stack inputs
    // survive only when the exact persistent-store source slice can consume
    // them.
    return input;
}

AbstractState tail_store_input(
    const AbstractState& observation,
    const AbiStackArgumentReadSet* const required_stack_reads = nullptr,
    const std::uint16_t* const entry_register_reads = nullptr) {
    auto input = observation;
    if (entry_register_reads != nullptr) {
        for (std::uint8_t index = 0u; index < input.size(); ++index) {
            if ((*entry_register_reads & register_bit(index)) != 0u)
                continue;
            input[index] = AbstractValue{};
            input.stack_offsets[index].reset();
            clear_inventory_stack_coordinates(input, index);
            input.stack_may_alias[index] = true;
            input.inventory_stack_may_alias[index] = true;
            input.inventory_vbr_relative[index] = false;
            input.inventory_fixed_storage_reference[index] = false;
        }
        input.stack_offsets[15u] = 0;
        static_cast<void>(set_inventory_stack_coordinates(
            input, 15u, std::vector<std::int32_t>{0}));
    }
    for (auto& value : input) {
        if (!value.known && !has_inventory_candidate_values(value) &&
            !inventory_candidate_values_truncated(value))
            continue;
        value.guarded = true;
        value.complete = false;
        if (has_inventory_candidate_values(value) ||
            value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    if (required_stack_reads != nullptr &&
        required_stack_reads->complete) {
        for (const auto& [slot, value] : input.stack_values) {
            static_cast<void>(slot);
            if (has_saved_stack_epoch(value) &&
                value.inventory_saved_stack_epoch
                    .tracks_current_epoch)
                input.inventory_current_stack_epoch_alias_watcher =
                    true;
        }
        std::erase_if(input.stack_values, [&](const auto& stored) {
            return !std::binary_search(
                required_stack_reads->slots.begin(),
                required_stack_reads->slots.end(),
                stored.first);
        });
    } else {
        collapse_payload_free_stack_aliases(input);
    }
    for (auto& [offset, value] : input.stack_values) {
        static_cast<void>(offset);
        value.guarded = true;
        value.complete = false;
        if (has_inventory_candidate_values(value) ||
            value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    for (auto& [address, value] : input.memory_values) {
        static_cast<void>(address);
        value.guarded = true;
        value.complete = false;
        if (has_inventory_candidate_values(value) ||
            value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    return input;
}

bool requires_forwarded_isolated_store_harvest(const katana::io::ExecutableImage& image,
                                               const AbstractState& observation,
                                               const AbstractState& merged_input) {
    if (observation.inventory_unresolved_saved_stack_alias_sources !=
            merged_input
                .inventory_unresolved_saved_stack_alias_sources ||
        observation
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch !=
            merged_input
                .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        observation.inventory_current_stack_epoch_alias_watcher !=
            merged_input.inventory_current_stack_epoch_alias_watcher ||
        observation.inventory_detached_stack_epoch_alias_watcher !=
            merged_input.inventory_detached_stack_epoch_alias_watcher ||
        observation.inventory_unresolved_stack_callback_loss !=
            merged_input.inventory_unresolved_stack_callback_loss ||
        observation.inventory_stack_callback_loss_identity_truncated !=
            merged_input
                .inventory_stack_callback_loss_identity_truncated)
        return true;
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        const auto& observed = observation[index];
        if (!same_stack_callback_provenance(
                observed, merged_input[index]))
            return true;
        if (observed.inventory_code_pointer_values.empty())
            continue;
        const auto& merged = merged_input[index];
        for (const auto value : observed.inventory_code_pointer_values) {
            if (!validate_decode_candidate(image, value).valid()) continue;
            if (!has_inventory_code_pointer_value(merged, value))
                return true;
        }
    }
    for (const auto& [slot, observed] : observation.stack_values) {
        static_cast<void>(slot);
        if (observed.inventory_code_pointer_values.empty())
            continue;
        const auto merged = merged_input.stack_values.find(slot);
        for (const auto value : observed.inventory_code_pointer_values) {
            if (!validate_decode_candidate(image, value).valid()) continue;
            if (merged == merged_input.stack_values.end() ||
                !has_inventory_code_pointer_value(merged->second, value))
                return true;
        }
    }
    const auto unresolved_map_differs =
        [](const auto& observed_values,
           const auto& merged_values) {
            const auto provenance_at = [](const auto& values,
                                          const auto key) {
                const auto found = values.find(key);
                return found == values.end()
                           ? AbstractValue{}
                           : found->second;
            };
            for (const auto& [key, value] : observed_values) {
                if (!same_stack_callback_provenance(
                        value,
                        provenance_at(merged_values, key)))
                    return true;
            }
            for (const auto& [key, value] : merged_values) {
                if (!same_stack_callback_provenance(
                        value,
                        provenance_at(observed_values, key)))
                    return true;
            }
            return false;
        };
    if (unresolved_map_differs(observation.stack_values,
                               merged_input.stack_values) ||
        unresolved_map_differs(observation.memory_values,
                               merged_input.memory_values))
        return true;
    return false;
}

[[nodiscard]] EvaluationLens select_evaluation_lens(
    const ResolutionCollectionMode resolution_mode,
    const bool collect_guarded_inventory,
    const std::set<std::uint32_t>* const isolated_inventory_call_sites,
    const std::map<std::uint32_t, FunctionValueSummary>* const
        contextual_summaries) noexcept {
    if (isolated_inventory_call_sites != nullptr)
        return EvaluationLens::IsolatedObservation;
    if (contextual_summaries != nullptr)
        return EvaluationLens::ContextualReturn;
    if (resolution_mode == ResolutionCollectionMode::Semantic)
        return EvaluationLens::CandidateContract;
    if (collect_guarded_inventory ||
        resolution_mode == ResolutionCollectionMode::GuardedInventory)
        return EvaluationLens::GuardedInventory;
    return EvaluationLens::Summary;
}

struct FunctionEvaluationProjection final {
    EvaluationLens requested_lens = EvaluationLens::FullState;
    EvaluationLens effective_lens = EvaluationLens::FullState;
    bool full_state_fallback = false;
    bool register_contract_present = false;
    bool stack_contract_present = false;
    std::uint16_t register_read_mask =
        std::numeric_limits<std::uint16_t>::max();
    AbiStackArgumentReadSet stack_reads;
    AbstractState ingress;
};

[[nodiscard]] bool valid_complete_stack_read_contract(
    const AbiStackArgumentReadSet& reads) noexcept {
    if (!reads.complete ||
        reads.slots.size() > maximum_abi_stack_argument_slots ||
        !std::is_sorted(reads.slots.begin(), reads.slots.end()) ||
        std::adjacent_find(reads.slots.begin(), reads.slots.end()) !=
            reads.slots.end())
        return false;
    return std::all_of(
        reads.slots.begin(), reads.slots.end(), [](const auto slot) {
            return slot >= 0 && slot <= maximum_stack_distance &&
                   (slot & 3) == 0;
        });
}

[[nodiscard]] std::optional<AbstractState>
project_evaluation_ingress(
    const AbstractState& source,
    const std::uint16_t register_read_mask,
    const AbiStackArgumentReadSet& stack_reads) {
    // 0xffff is the explicit fail-closed register Top produced for unknown
    // blocks/opcodes/ingress. It must never be mistaken for a precise lens.
    if (register_read_mask ==
            std::numeric_limits<std::uint16_t>::max() ||
        !valid_complete_stack_read_contract(stack_reads))
        return std::nullopt;
    constexpr auto stack_pointer_bit =
        static_cast<std::uint16_t>(1u << 15u);
    if (!stack_reads.slots.empty() &&
        (register_read_mask & stack_pointer_bit) == 0u)
        return std::nullopt;

    AbstractState projected;
    // Memory remains FullState until an address-read contract exists. The
    // state-wide inventory loss/watchers are likewise observable independent
    // of one concrete register or stack-slot identity.
    projected.memory_values = source.memory_values;
    projected.inventory_unresolved_saved_stack_alias_sources =
        source.inventory_unresolved_saved_stack_alias_sources;
    projected.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        source
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch;
    projected.inventory_current_stack_epoch_alias_watcher =
        source.inventory_current_stack_epoch_alias_watcher;
    projected.inventory_detached_stack_epoch_alias_watcher =
        source.inventory_detached_stack_epoch_alias_watcher;
    projected.inventory_unresolved_stack_callback_loss =
        source.inventory_unresolved_stack_callback_loss;
    projected.inventory_stack_callback_loss_identity_truncated =
        source.inventory_stack_callback_loss_identity_truncated;
    for (std::size_t index = 0u;
         index < projected.registers.size();
         ++index) {
        const auto bit = static_cast<std::uint16_t>(1u << index);
        if ((register_read_mask & bit) != 0u) {
            projected.registers[index] = source.registers[index];
            projected.stack_offsets[index] =
                source.stack_offsets[index];
            projected.inventory_stack_offsets[index] =
                source.inventory_stack_offsets[index];
            projected.inventory_stack_offset_candidates[index] =
                source.inventory_stack_offset_candidates[index];
            projected.stack_may_alias[index] =
                source.stack_may_alias[index];
            projected.inventory_stack_may_alias[index] =
                source.inventory_stack_may_alias[index];
            projected.inventory_vbr_relative[index] =
                source.inventory_vbr_relative[index];
            projected.inventory_fixed_storage_reference[index] =
                source.inventory_fixed_storage_reference[index];
            continue;
        }
        projected.registers[index] = {};
        projected.stack_offsets[index].reset();
        projected.inventory_stack_offsets[index].reset();
        projected.inventory_stack_offset_candidates[index].clear();
        projected.stack_may_alias[index] = true;
        projected.inventory_stack_may_alias[index] = true;
        projected.inventory_vbr_relative[index] = false;
        projected.inventory_fixed_storage_reference[index] = false;
    }

    for (const auto& [slot, value] : source.stack_values) {
        if (std::binary_search(stack_reads.slots.begin(),
                               stack_reads.slots.end(),
                               slot)) {
            projected.stack_values.emplace(slot, value);
            continue;
        }
        if (has_saved_stack_epoch(value) &&
            value.inventory_saved_stack_epoch
                .tracks_current_epoch)
            projected.inventory_current_stack_epoch_alias_watcher =
                true;
    }
    // No complete address-read contract exists yet. Retaining the entire
    // memory domain is therefore part of this lens's fail-closed semantics.
    return projected;
}

[[nodiscard]] bool canonicalize_evaluation_ingress_in_place(
    AbstractState& state,
    const std::uint16_t register_read_mask,
    const AbiStackArgumentReadSet& stack_reads) {
    if (register_read_mask ==
            std::numeric_limits<std::uint16_t>::max() ||
        !valid_complete_stack_read_contract(stack_reads))
        return false;
    constexpr auto stack_pointer_bit =
        static_cast<std::uint16_t>(1u << 15u);
    if (!stack_reads.slots.empty() &&
        (register_read_mask & stack_pointer_bit) == 0u)
        return false;
    for (std::size_t index = 0u;
         index < state.registers.size();
         ++index) {
        const auto bit = static_cast<std::uint16_t>(1u << index);
        if ((register_read_mask & bit) != 0u) continue;
        state.registers[index] = {};
        state.stack_offsets[index].reset();
        state.inventory_stack_offsets[index].reset();
        state.inventory_stack_offset_candidates[index].clear();
        state.stack_may_alias[index] = true;
        state.inventory_stack_may_alias[index] = true;
        state.inventory_vbr_relative[index] = false;
        state.inventory_fixed_storage_reference[index] = false;
    }
    for (auto stored = state.stack_values.begin();
         stored != state.stack_values.end();) {
        if (std::binary_search(stack_reads.slots.begin(),
                               stack_reads.slots.end(),
                               stored->first)) {
            ++stored;
            continue;
        }
        if (has_saved_stack_epoch(stored->second) &&
            stored->second.inventory_saved_stack_epoch
                .tracks_current_epoch)
            state.inventory_current_stack_epoch_alias_watcher = true;
        stored = state.stack_values.erase(stored);
    }
    return true;
}

[[nodiscard]] FunctionEvaluationProjection
make_function_evaluation_projection(
    const AbstractState& initial_state,
    const EvaluationLens requested_lens,
    const std::uint32_t function_entry,
    const ForwardedRegisterReadMap& forwarded_register_reads,
    const AbiStackArgumentReadMap& abi_stack_argument_reads,
    const bool contracts_available) {
    FunctionEvaluationProjection projection;
    projection.requested_lens = requested_lens;

    const auto register_reads =
        forwarded_register_reads.find(function_entry);
    projection.register_contract_present =
        register_reads != forwarded_register_reads.end();
    if (projection.register_contract_present)
        projection.register_read_mask = register_reads->second;

    const auto stack_reads =
        abi_stack_argument_reads.find(function_entry);
    projection.stack_contract_present =
        stack_reads != abi_stack_argument_reads.end();
    if (projection.stack_contract_present)
        projection.stack_reads = stack_reads->second;

    if (requested_lens == EvaluationLens::FullState) {
        projection.ingress = initial_state;
        return projection;
    }
    auto projected =
        contracts_available && projection.register_contract_present &&
                projection.stack_contract_present
            ? project_evaluation_ingress(
                  initial_state,
                  projection.register_read_mask,
                  projection.stack_reads)
            : std::nullopt;
    if (!projected.has_value()) {
        projection.full_state_fallback = true;
        projection.ingress = initial_state;
        return projection;
    }
    projection.effective_lens = requested_lens;
    projection.ingress = std::move(*projected);
    return projection;
}

void canonicalize_evaluation_outputs(
    FunctionEvaluation& evaluation,
    const ForwardedRegisterReadMap& forwarded_register_reads,
    const AbiStackArgumentReadMap& abi_stack_argument_reads) {
    // Function summaries encode ABI-preserved values as passthroughs. Keep
    // those symbolic summaries and canonicalize concrete outgoing states for
    // the next target's complete ingress contract.
    const auto reconstruct_state =
        [&](const std::uint32_t target, AbstractState& state) {
            const auto register_reads =
                forwarded_register_reads.find(target);
            const auto stack_reads =
                abi_stack_argument_reads.find(target);
            if (register_reads == forwarded_register_reads.end() ||
                stack_reads == abi_stack_argument_reads.end())
                return;
            static_cast<void>(
                canonicalize_evaluation_ingress_in_place(
                    state,
                    register_reads->second,
                    stack_reads->second));
        };
    for (auto& call : evaluation.call_arguments)
        reconstruct_state(call.callee, call.state);
    for (auto& transfer : evaluation.inventory_transfers)
        reconstruct_state(transfer.target, transfer.state);
}

enum class EvaluationKeyComponent : std::uint8_t {
    FunctionShape,
    ProjectedIngress,
    SummaryDependency,
    AbiContract,
    ResolutionLens,
    InventorySink,
    IsolationPartition,
    ContextualSummary,
    TailIngress,
    Count,
};

inline constexpr auto evaluation_key_component_count =
    static_cast<std::size_t>(EvaluationKeyComponent::Count);
inline constexpr std::uint64_t evaluation_key_hash_basis =
    1469598103934665603ull;
inline constexpr std::uint64_t evaluation_key_hash_prime =
    1099511628211ull;

enum class EvaluationKeyInternDomain : std::uint8_t {
    SemanticCandidates,
    InventoryCandidates,
    Evidence,
    Count,
};

inline constexpr auto evaluation_key_intern_domain_count =
    static_cast<std::size_t>(EvaluationKeyInternDomain::Count);

class EvaluationKeyEncoder {
  public:
    explicit EvaluationKeyEncoder(
        const bool collect_component_hashes = false) noexcept
        : collect_component_hashes_(collect_component_hashes) {
        component_hashes_.fill(evaluation_key_hash_basis);
    }

    void select_component(
        const EvaluationKeyComponent component) noexcept {
        active_component_ = component;
    }

    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    void append(const T value) {
        using U = std::make_unsigned_t<T>;
        auto bits = static_cast<U>(value);
        for (std::size_t byte = 0u; byte < sizeof(U); ++byte) {
            append_byte(static_cast<std::uint8_t>(
                bits & static_cast<U>(0xffu)));
            bits >>= 8u;
        }
    }

    void append(const bool value) {
        append_byte(value ? 1u : 0u);
    }

    template <typename E>
        requires std::is_enum_v<E>
    void append(const E value) {
        append(static_cast<std::underlying_type_t<E>>(value));
    }

    void append_size(const std::size_t value) {
        append(static_cast<std::uint64_t>(value));
    }

    void append(const std::string_view value) {
        append_size(value.size());
        for (const auto character : value)
            append_byte(static_cast<std::uint8_t>(character));
    }

    template <typename T>
    void append_optional(const std::optional<T>& value) {
        append(value.has_value());
        if (value.has_value()) append(*value);
    }

    template <typename Range, typename Append>
    void append_range(const Range& values, Append&& append_value) {
        append_size(values.size());
        for (const auto& value : values)
            append_value(value);
    }

    template <typename Range>
    void append_interned_u32_range(
        const EvaluationKeyInternDomain domain,
        const Range& values) {
        std::vector<std::uint32_t> canonical(
            values.begin(), values.end());
        std::sort(canonical.begin(), canonical.end());
        canonical.erase(
            std::unique(canonical.begin(), canonical.end()),
            canonical.end());

        // Intern tables are component-local. This preserves exact component
        // invalidation telemetry while still replacing repeated candidate and
        // evidence sets inside the large ingress/summary components with a
        // deterministic reference.
        append(domain);
        auto& table = intern_tables_[
            static_cast<std::size_t>(active_component_) *
                evaluation_key_intern_domain_count +
            static_cast<std::size_t>(domain)];
        const auto found = table.find(canonical);
        if (found != table.end()) {
            append(false);
            append(found->second);
            ++interned_references_;
            return;
        }
        const auto identifier =
            static_cast<std::uint64_t>(table.size());
        table.emplace(canonical, identifier);
        append(true);
        append(identifier);
        append_size(canonical.size());
        for (const auto value : canonical) append(value);
        ++interned_sets_;
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

    [[nodiscard]] const std::array<
        std::uint64_t,
        evaluation_key_component_count>&
    component_hashes() const noexcept {
        return component_hashes_;
    }

    [[nodiscard]] std::uint64_t hash() const noexcept {
        return hash_;
    }

    [[nodiscard]] std::size_t interned_sets() const noexcept {
        return interned_sets_;
    }

    [[nodiscard]] std::size_t interned_references() const noexcept {
        return interned_references_;
    }

  private:
    void append_byte(const std::uint8_t byte) {
        bytes_.push_back(byte);
        hash_ ^= byte;
        hash_ *= evaluation_key_hash_prime;
        if (!collect_component_hashes_) return;
        auto& hash =
            component_hashes_[static_cast<std::size_t>(
                active_component_)];
        hash ^= byte;
        hash *= evaluation_key_hash_prime;
    }

    std::vector<std::uint8_t> bytes_;
    std::array<
        std::map<std::vector<std::uint32_t>, std::uint64_t>,
        evaluation_key_component_count *
            evaluation_key_intern_domain_count>
        intern_tables_;
    std::array<std::uint64_t, evaluation_key_component_count>
        component_hashes_{};
    std::uint64_t hash_ = evaluation_key_hash_basis;
    EvaluationKeyComponent active_component_ =
        EvaluationKeyComponent::FunctionShape;
    bool collect_component_hashes_ = false;
    std::size_t interned_sets_ = 0u;
    std::size_t interned_references_ = 0u;
};

void encode(EvaluationKeyEncoder& key,
            const InventorySavedStackSlot& slot) {
    key.append(slot.relative_slot);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        slot.inventory_code_pointer_values);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        slot.inventory_pc_relative_code_literal_values);
    key.append(slot.inventory_code_pointer_values_truncated);
    key.append(
        slot.inventory_pc_relative_code_literal_values_truncated);
    key.append(slot.contextual_candidate_dependency);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence, slot.call_sites);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence, slot.callees);
}

void encode(EvaluationKeyEncoder& key,
            const InventorySavedStackEpoch& epoch) {
    key.append(epoch.present);
    key.append(epoch.unresolved);
    key.append(epoch.tracks_current_epoch);
    key.append(epoch.candidate_payload_lost);
    key.append_range(epoch.slots,
                     [&](const auto& slot) { encode(key, slot); });
}

void encode(EvaluationKeyEncoder& key,
            const AbstractValue& value) {
    key.append(value.known);
    key.append(value.guarded);
    key.append(value.complete);
    key.append(value.inventory_stack_derived);
    key.append(value.inventory_code_pointer);
    key.append(value.inventory_pc_relative_code_literal);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        value.inventory_code_pointer_values);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        value.inventory_pc_relative_code_literal_values);
    key.append(value.inventory_code_pointer_values_truncated);
    key.append(
        value.inventory_pc_relative_code_literal_values_truncated);
    key.append(value.contextual_candidate_dependency);
    key.append(value.inventory_stack_callback_loss_unresolved);
    encode(key, value.inventory_saved_stack_epoch);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::SemanticCandidates,
        value.values);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence, value.call_sites);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence, value.callees);
}

void encode(EvaluationKeyEncoder& key,
            const AbstractState& state) {
    key.append_range(state.registers,
                     [&](const auto& value) { encode(key, value); });
    key.append_range(
        state.stack_offsets,
        [&](const auto& offset) { key.append_optional(offset); });
    key.append_range(
        state.inventory_stack_offsets,
        [&](const auto& offset) { key.append_optional(offset); });
    key.append_range(
        state.inventory_stack_offset_candidates,
        [&](const auto& coordinates) {
            key.append_range(
                coordinates,
                [&](const auto coordinate) {
                    key.append(coordinate);
                });
        });
    key.append_range(state.stack_may_alias,
                     [&](const auto value) { key.append(value); });
    key.append_range(
        state.inventory_stack_may_alias,
        [&](const auto value) { key.append(value); });
    key.append_range(
        state.inventory_vbr_relative,
        [&](const auto value) { key.append(value); });
    key.append_range(
        state.inventory_fixed_storage_reference,
        [&](const auto value) { key.append(value); });
    key.append_range(
        state.stack_values,
        [&](const auto& stored) {
            key.append(stored.first);
            encode(key, stored.second);
        });
    key.append_range(
        state.memory_values,
        [&](const auto& stored) {
            key.append(stored.first);
            encode(key, stored.second);
        });
    key.append(
        state.inventory_unresolved_saved_stack_alias_sources);
    key.append(
        state.inventory_unresolved_saved_stack_alias_tracks_current_epoch);
    key.append(state.inventory_current_stack_epoch_alias_watcher);
    key.append(state.inventory_detached_stack_epoch_alias_watcher);
    key.append(state.inventory_unresolved_stack_callback_loss);
    key.append(
        state.inventory_stack_callback_loss_identity_truncated);
}

void encode(EvaluationKeyEncoder& key,
            const FunctionRegisterValueSummary& summary) {
    key.append(summary.register_index);
    key.append(summary.complete);
    key.append(summary.guarded);
    key.append(summary.abi_preserved);
    key.append(summary.may_alias_stack);
    key.append(summary.inventory_code_pointer);
    key.append(summary.inventory_pc_relative_code_literal);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        summary.inventory_code_pointer_values);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::InventoryCandidates,
        summary.inventory_pc_relative_code_literal_values);
    key.append(summary.inventory_code_pointer_values_truncated);
    key.append(
        summary.inventory_pc_relative_code_literal_values_truncated);
    key.append(summary.contextual_candidate_dependency);
    key.append(summary.inventory_stack_callback_loss_unresolved);
    key.append(summary.inventory_saved_stack_alias_latent);
    key.append(
        summary.inventory_saved_stack_alias_tracks_current_epoch);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::SemanticCandidates,
        summary.values);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence, summary.return_sites);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::Evidence,
        summary.evidence_callees);
    key.append(std::string_view{summary.reason});
}

void encode(EvaluationKeyEncoder& key,
            const FunctionMemoryValueSummary& summary) {
    key.append(summary.address);
    key.append(summary.complete);
    key.append(summary.guarded);
    key.append(summary.inventory_stack_callback_loss_unresolved);
    key.append(summary.inventory_saved_stack_alias_latent);
    key.append(
        summary.inventory_saved_stack_alias_tracks_current_epoch);
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::SemanticCandidates,
        summary.values);
}

void encode(EvaluationKeyEncoder& key,
            const FunctionValueSummary& summary) {
    key.append(summary.function_address);
    key.append_range(summary.registers,
                     [&](const auto& value) { encode(key, value); });
    key.append(summary.memory_complete);
    key.append_range(summary.memory_values,
                     [&](const auto& value) { encode(key, value); });
    key.append(
        summary.inventory_unresolved_saved_stack_alias_sources);
    key.append(
        summary.inventory_unresolved_saved_stack_alias_tracks_current_epoch);
    key.append(summary.inventory_unresolved_stack_callback_loss);
    key.append(
        summary.inventory_stack_callback_loss_identity_truncated);
}

void encode(EvaluationKeyEncoder& key,
            const IndirectCalleeCandidates& candidates) {
    key.append_interned_u32_range(
        EvaluationKeyInternDomain::SemanticCandidates,
        candidates.targets);
    key.append(candidates.guarded);
    key.append(candidates.complete);
    key.append(candidates.requires_code_pointer);
    key.append(candidates.observes_abi_arguments);
}

void encode(EvaluationKeyEncoder& key,
            const AbiStackArgumentReadSet& reads) {
    key.append_range(reads.slots,
                     [&](const auto slot) { key.append(slot); });
    key.append(reads.complete);
    key.append_range(
        reads.top_chain,
        [&](const auto& frame) {
            key.append(frame.reason);
            key.append(frame.owner);
            key.append(frame.site);
            key.append(frame.target);
            key.append(frame.contract_present);
            key.append(frame.contract_complete);
            key.append(frame.ingress_present);
            key.append(frame.ingress_guarded);
            key.append(frame.ingress_complete);
            key.append(frame.residual_indirect);
            key.append(frame.external_successor);
        });
    key.append(reads.top_chain_truncated);
}

void encode(EvaluationKeyEncoder& key,
            const FunctionInfo& function) {
    key.append(function.entry_address);
    key.append(function.size);
    key.append_range(function.block_addresses,
                     [&](const auto value) { key.append(value); });
    key.append_range(function.direct_callees,
                     [&](const auto value) { key.append(value); });
    key.append_range(function.indirect_call_sites,
                     [&](const auto value) { key.append(value); });
    key.append_range(function.shared_block_addresses,
                     [&](const auto value) { key.append(value); });
    key.append_range(function.tail_jump_targets,
                     [&](const auto value) { key.append(value); });
    key.append(function.evidence);
}

void encode(EvaluationKeyEncoder& key,
            const katana::sh4::DisassemblyLine& line) {
    const auto& instruction = line.instruction;
    key.append(line.address);
    key.append(line.opcode);
    key.append(line.is_delay_slot);
    key.append_optional(line.target_address);
    key.append(instruction.opcode);
    key.append(instruction.kind);
    key.append(instruction.destination_register);
    key.append(instruction.source_register);
    key.append(instruction.branch_register);
    key.append(instruction.immediate);
    key.append(instruction.displacement);
    key.append(instruction.special_register);
    key.append(instruction.control_flow);
    key.append(instruction.has_delay_slot);
    key.append(instruction.is_privileged);
}

void encode(EvaluationKeyEncoder& key,
            const BasicBlock& block) {
    key.append(block.start_address);
    key.append(block.end_address);
    key.append_range(block.lines,
                     [&](const auto& line) { encode(key, line); });
    key.append_range(block.successors,
                     [&](const auto successor) {
                         key.append(successor);
                     });
    key.append(block.has_indirect_successor);
}

struct FunctionEvaluationCacheKey {
    std::uint64_t hash = 0u;
    std::uint64_t diagnostic_fingerprint = 0u;
    std::uint32_t function_entry = 0u;
    std::array<std::uint64_t, evaluation_key_component_count>
        component_hashes{};
    std::vector<std::uint8_t> bytes;
    std::size_t interned_sets = 0u;
    std::size_t interned_references = 0u;
};

[[nodiscard]] std::uint64_t evaluation_key_hash(
    const std::span<const std::uint8_t> bytes) noexcept {
    auto hash = evaluation_key_hash_basis;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= evaluation_key_hash_prime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t evaluation_key_diagnostic_fingerprint(
    const std::span<const std::uint8_t> bytes) noexcept {
    auto hash = std::uint64_t{7809847782465536322ull};
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte) +
                0x9e3779b97f4a7c15ull;
        hash *= 14029467366897019727ull;
        hash ^= hash >> 29u;
    }
    hash ^= static_cast<std::uint64_t>(bytes.size()) *
            0x94d049bb133111ebull;
    return hash;
}

[[nodiscard]] FunctionEvaluationCacheKey
make_function_evaluation_cache_key(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>&
        indirect_callees,
    const TailIngressMap& tail_ingresses,
    const std::map<std::uint32_t, FunctionValueSummary>& summaries,
    const FunctionEvaluationProjection& projection,
    const ResolutionCollectionMode resolution_mode,
    const bool may_merge_stack_inventory,
    const bool collect_guarded_inventory,
    const std::set<std::uint32_t>* const isolated_inventory_call_sites,
    const std::map<std::uint32_t, FunctionValueSummary>*
        contextual_summaries,
    const TailIngressMap* const local_tail_ingresses,
    const bool collect_walk_diagnostics,
    const AbiStackArgumentReadMap* const abi_stack_argument_reads,
    const std::uint8_t inventory_sink_sources,
    const bool collect_component_hashes) {
    EvaluationKeyEncoder key(collect_component_hashes);
    // Local schema guard for the exact in-process representation.
    key.select_component(EvaluationKeyComponent::FunctionShape);
    key.append(std::uint32_t{3u});
    key.append(image.analysis_instance_identity());
    key.append(image.analysis_revision());
    key.append(image.guest_call_abi());
    encode(key, function);
    key.select_component(EvaluationKeyComponent::ProjectedIngress);
    encode(key, projection.ingress);
    key.select_component(EvaluationKeyComponent::ResolutionLens);
    key.append(evaluation_lens_schema_version);
    key.append(projection.requested_lens);
    key.append(projection.effective_lens);
    key.append(projection.full_state_fallback);
    key.append(resolution_mode);
    key.append(may_merge_stack_inventory);
    key.select_component(EvaluationKeyComponent::InventorySink);
    key.append(collect_guarded_inventory);
    key.select_component(EvaluationKeyComponent::IsolationPartition);
    key.append(isolated_inventory_call_sites != nullptr);
    if (isolated_inventory_call_sites != nullptr) {
        key.append_range(
            *isolated_inventory_call_sites,
            [&](const auto site) { key.append(site); });
    }
    key.select_component(EvaluationKeyComponent::ContextualSummary);
    key.append(contextual_summaries != nullptr);
    key.select_component(EvaluationKeyComponent::TailIngress);
    key.append(local_tail_ingresses != nullptr);
    key.select_component(EvaluationKeyComponent::ResolutionLens);
    key.append(collect_walk_diagnostics);
    key.select_component(EvaluationKeyComponent::AbiContract);
    key.append(abi_stack_argument_reads != nullptr);
    key.append(function.entry_address);
    key.append(projection.register_contract_present);
    if (projection.register_contract_present)
        key.append(projection.register_read_mask);
    key.append(projection.stack_contract_present);
    if (projection.stack_contract_present)
        encode(key, projection.stack_reads);
    key.select_component(EvaluationKeyComponent::InventorySink);
    key.append(inventory_sink_sources);

    std::set<std::uint32_t> summary_dependencies;
    std::set<std::uint32_t> abi_dependencies;
    auto evaluation_block_addresses =
        function.block_addresses;
    evaluation_block_addresses.push_back(
        function.entry_address);
    normalize(evaluation_block_addresses);
    key.select_component(EvaluationKeyComponent::FunctionShape);
    key.append_size(evaluation_block_addresses.size());
    for (const auto block_address :
         evaluation_block_addresses) {
        key.select_component(EvaluationKeyComponent::FunctionShape);
        key.append(block_address);
        const auto block = blocks.find(block_address);
        key.append(block != blocks.end());
        if (block == blocks.end()) continue;
        encode(key, *block->second);
        for (const auto& line : block->second->lines) {
            const bool call =
                line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::IndirectCall;
            if (call) {
                key.select_component(
                    EvaluationKeyComponent::FunctionShape);
                key.append(line.address);
                if (line.target_address.has_value()) {
                    key.append(true);
                    key.append(*line.target_address);
                    summary_dependencies.insert(
                        *line.target_address);
                    abi_dependencies.insert(
                        *line.target_address);
                } else {
                    key.append(false);
                    const auto candidates =
                        indirect_callees.find(line.address);
                    key.append(candidates !=
                               indirect_callees.end());
                    if (candidates !=
                        indirect_callees.end()) {
                        encode(key, candidates->second);
                        summary_dependencies.insert(
                            candidates->second.targets.begin(),
                            candidates->second.targets.end());
                        abi_dependencies.insert(
                            candidates->second.targets.begin(),
                            candidates->second.targets.end());
                    }
                }
                continue;
            }

            const auto ingress = find_tail_ingress(
                tail_ingresses,
                local_tail_ingresses,
                line.address);
            key.select_component(
                EvaluationKeyComponent::TailIngress);
            key.append(line.address);
            key.append(ingress.has_value());
            if (!ingress.has_value()) continue;
            encode(key, *ingress);
            abi_dependencies.insert(ingress->targets.begin(),
                                    ingress->targets.end());
        }
    }

    key.select_component(
        EvaluationKeyComponent::SummaryDependency);
    key.append_range(
        summary_dependencies,
        [&](const auto address) {
            key.select_component(
                EvaluationKeyComponent::SummaryDependency);
            key.append(address);
            const auto global = summaries.find(address);
            key.append(global != summaries.end());
            if (global != summaries.end())
                encode(key, global->second);
            if (contextual_summaries == nullptr) return;
            const auto contextual =
                contextual_summaries->find(address);
            key.select_component(
                EvaluationKeyComponent::ContextualSummary);
            key.append(contextual !=
                       contextual_summaries->end());
            if (contextual != contextual_summaries->end())
                encode(key, contextual->second);
        });
    key.select_component(
        EvaluationKeyComponent::AbiContract);
    key.append_range(
        abi_dependencies,
        [&](const auto address) {
            key.select_component(
                EvaluationKeyComponent::AbiContract);
            key.append(address);
            if (abi_stack_argument_reads == nullptr) return;
            const auto reads =
                abi_stack_argument_reads->find(address);
            key.append(reads !=
                       abi_stack_argument_reads->end());
            if (reads != abi_stack_argument_reads->end())
                encode(key, reads->second);
        });

    const auto component_hashes = key.component_hashes();
    const auto interned_sets = key.interned_sets();
    const auto interned_references = key.interned_references();
    const auto hash = key.hash();
    auto bytes = std::move(key).finish();
    return {hash,
            collect_component_hashes
                ? evaluation_key_diagnostic_fingerprint(bytes)
                : hash,
            function.entry_address,
            component_hashes,
            std::move(bytes),
            interned_sets,
            interned_references};
}

[[nodiscard]] std::size_t retained_heap_bytes(
    const AbstractValue& value) noexcept {
    std::size_t bytes = 0u;
    bytes += value.inventory_code_pointer_values.capacity() *
             sizeof(std::uint32_t);
    bytes +=
        value.inventory_pc_relative_code_literal_values.capacity() *
        sizeof(std::uint32_t);
    bytes += value.values.capacity() * sizeof(std::uint32_t);
    bytes += (value.call_sites.size() + value.callees.size()) *
             (sizeof(std::uint32_t) + 3u * sizeof(void*));
    bytes += value.inventory_saved_stack_epoch.slots.capacity() *
             sizeof(InventorySavedStackSlot);
    for (const auto& slot :
         value.inventory_saved_stack_epoch.slots) {
        bytes += slot.inventory_code_pointer_values.capacity() *
                 sizeof(std::uint32_t);
        bytes +=
            slot.inventory_pc_relative_code_literal_values.capacity() *
            sizeof(std::uint32_t);
        bytes += (slot.call_sites.size() + slot.callees.size()) *
                 (sizeof(std::uint32_t) + 3u * sizeof(void*));
    }
    return bytes;
}

[[nodiscard]] std::size_t retained_heap_bytes(
    const std::string& value) noexcept {
    if (value.empty()) return 0u;
    const auto object_begin = reinterpret_cast<std::uintptr_t>(&value);
    const auto object_end = object_begin + sizeof(value);
    const auto data = reinterpret_cast<std::uintptr_t>(value.data());
    // A short-string buffer is part of the already-accounted inline owner.
    // Comparing integer representations avoids relational pointer operations
    // across unrelated objects.
    if (data >= object_begin && data < object_end) return 0u;
    return value.capacity() + 1u;
}

[[nodiscard]] std::size_t retained_heap_bytes(
    const AbstractState& state) noexcept {
    std::size_t bytes = 0u;
    for (const auto& value : state.registers)
        bytes += retained_heap_bytes(value);
    for (const auto& coordinates :
         state.inventory_stack_offset_candidates) {
        bytes += coordinates.capacity() * sizeof(std::int32_t);
    }
    for (const auto& [slot, value] : state.stack_values) {
        static_cast<void>(slot);
        bytes += 3u * sizeof(void*) +
                 sizeof(std::pair<const std::int32_t, AbstractValue>) +
                 retained_heap_bytes(value);
    }
    for (const auto& [address, value] : state.memory_values) {
        static_cast<void>(address);
        bytes += 3u * sizeof(void*) +
                 sizeof(std::pair<const std::uint32_t, AbstractValue>) +
                 retained_heap_bytes(value);
    }
    return bytes;
}

[[nodiscard]] std::size_t retained_heap_bytes(
    const FunctionEvaluation& evaluation) noexcept {
    std::size_t bytes =
        evaluation.summary.registers.capacity() *
        sizeof(FunctionRegisterValueSummary);
    for (const auto& summary : evaluation.summary.registers) {
        bytes +=
            (summary.inventory_code_pointer_values.capacity() +
             summary
                 .inventory_pc_relative_code_literal_values.capacity() +
             summary.values.capacity() +
             summary.return_sites.capacity() +
             summary.evidence_callees.capacity()) *
            sizeof(std::uint32_t);
        bytes += retained_heap_bytes(summary.reason);
    }
    bytes += evaluation.summary.memory_values.capacity() *
             sizeof(FunctionMemoryValueSummary);
    for (const auto& summary :
         evaluation.summary.memory_values) {
        bytes += summary.values.capacity() *
                 sizeof(std::uint32_t);
    }
    bytes += evaluation.resolutions.capacity() *
             sizeof(InterproceduralTargetResolution);
    for (const auto& resolution : evaluation.resolutions) {
        bytes +=
            (resolution.targets.capacity() +
             resolution.call_sites.capacity() +
             resolution.callees.capacity()) *
            sizeof(std::uint32_t);
        bytes += retained_heap_bytes(resolution.reason);
    }
    bytes += evaluation.call_arguments.capacity() *
             sizeof(FunctionEvaluation::CallArguments);
    for (const auto& call : evaluation.call_arguments)
        bytes += retained_heap_bytes(call.state);
    bytes += evaluation.inventory_transfers.capacity() *
             sizeof(FunctionEvaluation::InventoryTransfer);
    for (const auto& transfer :
         evaluation.inventory_transfers)
        bytes += retained_heap_bytes(transfer.state);
    return bytes;
}

struct CachedFunctionEvaluation {
    FunctionEvaluation evaluation;
    GuardedCodeInventoryCollector inventory{true};
    GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
    // Run-local producer cost used only to value exact cache reuse. It is not
    // part of canonical analysis output or any cache identity.
    mutable std::uint64_t physical_evaluation_nanoseconds = 0u;

    [[nodiscard]] std::size_t
    retained_payload_budget_bytes() const noexcept {
        return sizeof(*this) +
               ::katana::analysis::retained_heap_bytes(evaluation) +
               inventory.retained_heap_bytes() +
               walk_diagnostics
                       .forwarded_store_context_limit_diagnostics
                       .capacity() *
                   sizeof(ForwardedStoreContextLimitDiagnostic);
    }
};

enum class EvaluationActivityKind : std::uint8_t {
    Request,
    KeyBuild,
    CacheWait,
    ExactReplay,
    PhysicalInterpreter,
    CacheCommit,
    Count,
};

struct EvaluationActivityMetric final {
    std::atomic_size_t active = 0u;
    std::atomic_size_t count = 0u;
    std::atomic_uint64_t cumulative_nanoseconds = 0u;
    std::atomic_uint64_t maximum_nanoseconds = 0u;
};

struct EvaluationActivityTelemetry final {
    std::array<EvaluationActivityMetric,
               static_cast<std::size_t>(EvaluationActivityKind::Count)>
        metrics;

    [[nodiscard]] EvaluationActivityMetric& metric(
        const EvaluationActivityKind kind) noexcept {
        return metrics[static_cast<std::size_t>(kind)];
    }

    [[nodiscard]] const EvaluationActivityMetric& metric(
        const EvaluationActivityKind kind) const noexcept {
        return metrics[static_cast<std::size_t>(kind)];
    }
};

void atomic_saturating_add(std::atomic_uint64_t& destination,
                           const std::uint64_t value) noexcept {
    auto current = destination.load(std::memory_order_relaxed);
    for (;;) {
        const auto replacement =
            value > std::numeric_limits<std::uint64_t>::max() - current
                ? std::numeric_limits<std::uint64_t>::max()
                : current + value;
        if (destination.compare_exchange_weak(
                current,
                replacement,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
            return;
    }
}

class EvaluationActivityScope final {
  public:
    EvaluationActivityScope(
        EvaluationActivityTelemetry* const telemetry,
        const EvaluationActivityKind kind) noexcept
        : metric_(telemetry == nullptr
                      ? nullptr
                      : &telemetry->metric(kind)) {
        if (metric_ == nullptr) return;
        metric_->count.fetch_add(1u, std::memory_order_relaxed);
        metric_->active.fetch_add(1u, std::memory_order_release);
        started_ = std::chrono::steady_clock::now();
    }

    EvaluationActivityScope(const EvaluationActivityScope&) = delete;
    EvaluationActivityScope& operator=(const EvaluationActivityScope&) =
        delete;

    ~EvaluationActivityScope() {
        if (metric_ == nullptr) return;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_);
        const auto nanoseconds = static_cast<std::uint64_t>(
            std::max(elapsed, std::chrono::nanoseconds{1}).count());
        atomic_saturating_add(
            metric_->cumulative_nanoseconds, nanoseconds);
        auto maximum = metric_->maximum_nanoseconds.load(
            std::memory_order_relaxed);
        while (maximum < nanoseconds &&
               !metric_->maximum_nanoseconds.compare_exchange_weak(
                   maximum,
                   nanoseconds,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
        metric_->active.fetch_sub(1u, std::memory_order_release);
    }

  private:
    EvaluationActivityMetric* metric_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
};

class FunctionEvaluationCache {
  public:
    FunctionEvaluationCache(const std::size_t maximum_entries,
                            const std::size_t
                                maximum_retained_payload_bytes,
                            const bool detailed_telemetry,
                            detail::FunctionEvaluationCacheDecisionObserver
                                decision_observer = {})
        : maximum_entries_(maximum_entries),
          maximum_retained_payload_bytes_(
              maximum_retained_payload_bytes),
          detailed_telemetry_(detailed_telemetry),
          decision_observer_(std::move(decision_observer)) {}

    void clear() {
        const std::lock_guard lock(mutex_);
        buckets_.clear();
        ready_lru_.clear();
        entries_ = 0u;
        retained_payload_bytes_ = 0u;
        clock_ = 0u;
        serial_ = 0u;
        lookups_ = 0u;
        ready_hits_ = 0u;
        in_flight_coalesces_ = 0u;
        misses_ = 0u;
        evictions_ = 0u;
        miss_reasons_.fill(0u);
        evaluation_lenses_ = {};
        component_histories_by_function_.clear();
        component_history_order_.clear();
        absent_key_histories_.clear();
        absent_key_order_.clear();
        absent_key_history_accounted_bytes_ = 0u;
        next_history_serial_ = 0u;
    }

    [[nodiscard]]
    detail::FunctionValueAnalysisSessionStatistics
    statistics() const {
        const std::lock_guard lock(mutex_);
        const auto hits =
            ready_hits_ + in_flight_coalesces_;
        return {lookups_,
                ready_hits_,
                in_flight_coalesces_,
                hits,
                misses_,
                evictions_,
                entries_,
                retained_payload_bytes_,
                miss_reasons_,
                evaluation_lenses_};
    }

    [[nodiscard]] bool detailed_telemetry_enabled() const noexcept {
        return detailed_telemetry_;
    }

    [[nodiscard]] std::size_t component_history_entries_for_testing(
        const std::uint32_t function_entry) const {
        const std::lock_guard lock(mutex_);
        const auto found =
            component_histories_by_function_.find(function_entry);
        return found == component_histories_by_function_.end()
                   ? 0u
                   : found->second.size();
    }

    [[nodiscard]] std::size_t
    absent_history_entries_for_testing() const {
        const std::lock_guard lock(mutex_);
        return absent_key_order_.size();
    }

    [[nodiscard]] std::size_t
    absent_history_accounted_bytes_for_testing() const {
        const std::lock_guard lock(mutex_);
        return absent_key_history_accounted_bytes_;
    }

    [[nodiscard]] static constexpr std::size_t
    absent_history_byte_limit_for_testing() noexcept {
        return maximum_absent_key_history_bytes;
    }

    template <typename Compute>
    [[nodiscard]] std::pair<
        std::shared_ptr<const CachedFunctionEvaluation>,
        bool>
    get_or_compute(FunctionEvaluationCacheKey key,
                   Compute&& compute,
                   EvaluationActivityTelemetry* const activity = nullptr) {
        return get_or_compute(
            std::move(key),
            EvaluationLens::FullState,
            true,
            std::forward<Compute>(compute),
            activity);
    }

    template <typename Compute>
    [[nodiscard]] std::pair<
        std::shared_ptr<const CachedFunctionEvaluation>,
        bool>
    get_or_compute(FunctionEvaluationCacheKey key,
                   const EvaluationLens requested_lens,
                   const bool full_state_fallback,
                   Compute&& compute,
                   EvaluationActivityTelemetry* const activity = nullptr) {
        using Result =
            std::shared_ptr<const CachedFunctionEvaluation>;
        std::shared_future<Result> future;
        Result ready_result;
        std::shared_ptr<std::promise<Result>> producer;
        Entry* producer_entry = nullptr;
        detail::FunctionEvaluationCacheDecision decision;
        decision.function_entry = key.function_entry;
        const auto requested_lens_index =
            static_cast<std::size_t>(requested_lens);
        const auto lens = requested_lens_index < evaluation_lens_count
                              ? requested_lens
                              : EvaluationLens::FullState;
        const auto lens_index = static_cast<std::size_t>(lens);
        const auto fell_back_to_full_state =
            full_state_fallback ||
            requested_lens_index >= evaluation_lens_count;
        decision.lens = lens;
        decision.full_state_fallback = fell_back_to_full_state;
        const auto saturating_add_nanoseconds = [](
            std::uint64_t& destination,
            const std::uint64_t value) noexcept {
            destination =
                value > std::numeric_limits<std::uint64_t>::max() -
                            destination
                    ? std::numeric_limits<std::uint64_t>::max()
                    : destination + value;
        };
        {
            const std::lock_guard lock(mutex_);
            ++lookups_;
            ++evaluation_lenses_.requests[lens_index];
            if (fell_back_to_full_state)
                ++evaluation_lenses_.full_state_fallbacks;
            evaluation_lenses_.key_interned_sets += key.interned_sets;
            evaluation_lenses_.key_interned_references +=
                key.interned_references;
            const auto bucket = buckets_.find(key.hash);
            if (bucket != buckets_.end()) {
                const auto found = std::find_if(
                    bucket->second.begin(),
                    bucket->second.end(),
                    [&](const auto& candidate) {
                        return candidate->key.bytes == key.bytes;
                    });
                if (found != bucket->second.end()) {
                    auto* const entry = found->get();
                    if (entry->ready) {
                        ready_result = entry->ready_result;
                        ++ready_hits_;
                        ++evaluation_lenses_.cache_hits[lens_index];
                        decision.avoided_evaluation_nanoseconds =
                            ready_result
                                ->physical_evaluation_nanoseconds;
                        saturating_add_nanoseconds(
                            evaluation_lenses_
                                .avoided_evaluation_nanoseconds[
                                    lens_index],
                            decision
                                .avoided_evaluation_nanoseconds);
                        decision.outcome = detail::
                            FunctionEvaluationCacheLookupOutcome::ReadyHit;
                        if (!touch_ready(*entry)) {
                            remember_absent_key(
                                entry->key,
                                detail::
                                    FunctionEvaluationCacheMissReason::
                                        OversizeOrNoExactReplay);
                            erase(entry, false);
                        }
                    } else {
                        future = entry->result;
                        ++in_flight_coalesces_;
                        ++evaluation_lenses_.cache_hits[lens_index];
                        decision.outcome = detail::
                            FunctionEvaluationCacheLookupOutcome::
                                InFlightCoalesce;
                    }
                }
            }
            if (!future.valid() && ready_result == nullptr) {
                ++misses_;
                if (!fell_back_to_full_state &&
                    lens != EvaluationLens::FullState)
                    ++evaluation_lenses_.projected_evaluations;
                const auto entry_base_payload_bytes =
                    sizeof(Entry) + key.bytes.capacity();
                make_room_for(entry_base_payload_bytes, true);
                const auto retainable_key =
                    maximum_entries_ != 0u &&
                    maximum_retained_payload_bytes_ >=
                        entry_base_payload_bytes &&
                    entries_ < maximum_entries_ &&
                    retained_payload_bytes_ <=
                        maximum_retained_payload_bytes_ -
                            entry_base_payload_bytes;
                const auto miss_reason =
                    retainable_key
                        ? classify_miss(key)
                        : detail::
                              FunctionEvaluationCacheMissReason::
                                  OversizeOrNoExactReplay;
                increment_miss_reason(miss_reason);
                decision.outcome = detail::
                    FunctionEvaluationCacheLookupOutcome::Miss;
                decision.miss_reason = miss_reason;
                remember_components(key);
                if (retainable_key) {
                    producer =
                        std::make_shared<std::promise<Result>>();
                    future = producer->get_future().share();
                    auto entry = std::make_unique<Entry>();
                    entry->key = std::move(key);
                    entry->result = future;
                    entry->serial = ++serial_;
                    entry->retained_payload_bytes =
                        entry_base_payload_bytes;
                    entry->miss_reason = miss_reason;
                    producer_entry = entry.get();
                    buckets_[producer_entry->key.hash].push_back(
                        std::move(entry));
                    ++entries_;
                    retained_payload_bytes_ +=
                        entry_base_payload_bytes;
                }
            }
        }
        if (ready_result != nullptr) {
            observe_decision(decision);
            return {std::move(ready_result), true};
        }
        if (future.valid() && producer == nullptr) {
            const EvaluationActivityScope wait{
                decision.outcome == detail::
                        FunctionEvaluationCacheLookupOutcome::
                            InFlightCoalesce
                    ? activity
                    : nullptr,
                EvaluationActivityKind::CacheWait};
            auto& executor = global_analysis_executor();
            if (executor.current_thread_is_worker()) {
                executor.help_until([&] {
                    return future.wait_for(
                               std::chrono::seconds{0}) ==
                           std::future_status::ready;
                });
            }
            try {
                auto result = future.get();
                decision.avoided_evaluation_nanoseconds =
                    result->physical_evaluation_nanoseconds;
                {
                    const std::lock_guard lock(mutex_);
                    saturating_add_nanoseconds(
                        evaluation_lenses_
                            .avoided_evaluation_nanoseconds[
                                lens_index],
                        decision.avoided_evaluation_nanoseconds);
                }
                observe_decision(decision);
                return {std::move(result), true};
            } catch (...) {
                observe_decision(decision);
                throw;
            }
        }

        if (producer == nullptr) {
            const auto started = std::chrono::steady_clock::now();
            try {
                auto result = compute();
                result->physical_evaluation_nanoseconds =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<
                            std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count());
                observe_decision(decision);
                return {std::move(result), false};
            } catch (...) {
                observe_decision(decision);
                throw;
            }
        }

        Result result;
        const auto compute_started =
            std::chrono::steady_clock::now();
        try {
            result = compute();
            result->physical_evaluation_nanoseconds =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        compute_started)
                        .count());
        } catch (...) {
            const auto error = std::current_exception();
            {
                const std::lock_guard lock(mutex_);
                erase(producer_entry, false);
            }
            try {
                producer->set_exception(error);
            } catch (...) {
                // The original compute failure remains authoritative.
            }
            observe_decision(decision);
            global_analysis_executor().notify_waiters();
            std::rethrow_exception(error);
        }

        {
            const EvaluationActivityScope commit{
                activity, EvaluationActivityKind::CacheCommit};
            std::unique_lock lock(mutex_);
            producer_entry->ready = true;
            producer_entry->ready_result = result;
            const auto artifact_payload_bytes =
                result->retained_payload_budget_bytes();
            const auto artifact_size_overflow =
                artifact_payload_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        producer_entry->retained_payload_bytes ||
                artifact_payload_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        retained_payload_bytes_;
            if (!artifact_size_overflow) {
                producer_entry->retained_payload_bytes +=
                    artifact_payload_bytes;
                retained_payload_bytes_ +=
                    artifact_payload_bytes;
            }
            const auto artifact_retainable =
                !artifact_size_overflow &&
                result->inventory.exact_replay_available() &&
                producer_entry->retained_payload_bytes <=
                    maximum_retained_payload_bytes_;
            const auto retained =
                artifact_retainable &&
                touch_ready(*producer_entry);
            if (!retained) {
                reclassify_miss(
                    producer_entry->miss_reason,
                    detail::FunctionEvaluationCacheMissReason::
                        OversizeOrNoExactReplay);
                decision.miss_reason = detail::
                    FunctionEvaluationCacheMissReason::
                        OversizeOrNoExactReplay;
                remember_absent_key(
                    producer_entry->key,
                    detail::FunctionEvaluationCacheMissReason::
                        OversizeOrNoExactReplay);
                erase(producer_entry, false);
            }
            try {
                // Publish only after every fallible LRU operation completed.
                // Waiters may wake while the cache mutex is held, but they do
                // not need it after copying the shared future.
                producer->set_value(result);
                if (retained)
                    producer_entry->result = {};
            } catch (...) {
                const auto error = std::current_exception();
                if (retained)
                    erase(producer_entry, false);
                lock.unlock();
                try {
                    producer->set_exception(error);
                } catch (...) {
                    // Preserve the original promise publication failure.
                }
                observe_decision(decision);
                global_analysis_executor().notify_waiters();
                std::rethrow_exception(error);
            }
            if (retained)
                make_room_for(0u, false);
        }
        observe_decision(decision);
        global_analysis_executor().notify_waiters();
        return {std::move(result), false};
    }

    void record_reconstructed_result() noexcept {
        const std::lock_guard lock(mutex_);
        ++evaluation_lenses_.reconstructed_results;
    }

  private:
    struct Entry {
        FunctionEvaluationCacheKey key;
        std::shared_future<
            std::shared_ptr<const CachedFunctionEvaluation>>
            result;
        std::shared_ptr<const CachedFunctionEvaluation>
            ready_result;
        std::uint64_t serial = 0u;
        std::uint64_t last_use = 0u;
        std::size_t retained_payload_bytes = 0u;
        bool ready = false;
        detail::FunctionEvaluationCacheMissReason miss_reason =
            detail::FunctionEvaluationCacheMissReason::Cold;
    };

    struct FunctionComponentHistory final {
        std::array<std::uint64_t,
                   evaluation_key_component_count>
            hashes{};
        std::uint64_t serial = 0u;
    };

    struct AbsentKeyHistory final {
        std::uint32_t function_entry = 0u;
        std::array<char, 64u> sha256{};
        detail::FunctionEvaluationCacheMissReason reason =
            detail::FunctionEvaluationCacheMissReason::Cold;
        std::uint64_t serial = 0u;
    };

    static constexpr std::size_t maximum_absent_key_tombstones =
        65'536u;
    static constexpr std::size_t maximum_absent_key_history_bytes =
        1u * 1024u * 1024u;
    static constexpr std::size_t absent_key_history_entry_charge =
        256u;
    static_assert(maximum_absent_key_history_bytes >=
                  absent_key_history_entry_charge);
    static_assert(
        absent_key_history_entry_charge >=
        2u * sizeof(AbsentKeyHistory) +
            sizeof(std::pair<std::uint64_t, std::uint64_t>) +
            5u * sizeof(void*));

    [[nodiscard]] static std::optional<std::array<char, 64u>>
    absent_key_digest(const FunctionEvaluationCacheKey& key) noexcept {
        try {
            const auto bytes = key.bytes.empty()
                                   ? std::string_view{}
                                   : std::string_view(
                                         reinterpret_cast<const char*>(
                                             key.bytes.data()),
                                         key.bytes.size());
            const auto digest = katana::io::sha256_bytes(bytes);
            if (digest.size() != 64u) return std::nullopt;
            std::array<char, 64u> result{};
            std::copy(digest.begin(), digest.end(), result.begin());
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static std::size_t miss_reason_index(
        const detail::FunctionEvaluationCacheMissReason reason) noexcept {
        return static_cast<std::size_t>(reason);
    }

    void increment_miss_reason(
        const detail::FunctionEvaluationCacheMissReason reason) noexcept {
        ++miss_reasons_[miss_reason_index(reason)];
    }

    void observe_decision(
        const detail::FunctionEvaluationCacheDecision& decision) const
        noexcept {
        if (!decision_observer_) return;
        try {
            decision_observer_(decision);
        } catch (...) {
            // The observer is evidence-only. It can neither perturb cache
            // reuse nor make canonical analysis fail.
        }
    }

    void reclassify_miss(
        detail::FunctionEvaluationCacheMissReason& current,
        const detail::FunctionEvaluationCacheMissReason replacement) noexcept {
        if (current == replacement) return;
        auto& previous = miss_reasons_[miss_reason_index(current)];
        if (previous != 0u) --previous;
        ++miss_reasons_[miss_reason_index(replacement)];
        current = replacement;
    }

    [[nodiscard]] detail::FunctionEvaluationCacheMissReason
    classify_miss(const FunctionEvaluationCacheKey& key) const noexcept {
        const auto absent =
            absent_key_histories_.find(key.diagnostic_fingerprint);
        if (absent != absent_key_histories_.end()) {
            const auto digest = absent_key_digest(key);
            if (digest) {
                const auto exact = std::find_if(
                    absent->second.begin(),
                    absent->second.end(),
                    [&](const auto& candidate) {
                        return candidate.function_entry ==
                                   key.function_entry &&
                               candidate.sha256 == *digest;
                    });
                if (exact != absent->second.end())
                    return exact->reason;
            }
        }
        if (!detailed_telemetry_)
            return detail::FunctionEvaluationCacheMissReason::Cold;
        const auto histories =
            component_histories_by_function_.find(
                key.function_entry);
        if (histories == component_histories_by_function_.end() ||
            histories->second.empty())
            return detail::FunctionEvaluationCacheMissReason::Cold;
        constexpr std::array priority{
            std::pair{
                EvaluationKeyComponent::FunctionShape,
                detail::FunctionEvaluationCacheMissReason::
                    FunctionShapeChanged},
            std::pair{
                EvaluationKeyComponent::ProjectedIngress,
                detail::FunctionEvaluationCacheMissReason::
                    ProjectedIngressChanged},
            std::pair{
                EvaluationKeyComponent::SummaryDependency,
                detail::FunctionEvaluationCacheMissReason::
                    SummaryDependencyChanged},
            std::pair{
                EvaluationKeyComponent::AbiContract,
                detail::FunctionEvaluationCacheMissReason::
                    AbiContractChanged},
            std::pair{
                EvaluationKeyComponent::ResolutionLens,
                detail::FunctionEvaluationCacheMissReason::
                    ResolutionLensChanged},
            std::pair{
                EvaluationKeyComponent::InventorySink,
                detail::FunctionEvaluationCacheMissReason::
                    InventorySinkChanged},
            std::pair{
                EvaluationKeyComponent::IsolationPartition,
                detail::FunctionEvaluationCacheMissReason::
                    IsolationPartitionChanged},
            std::pair{
                EvaluationKeyComponent::ContextualSummary,
                detail::FunctionEvaluationCacheMissReason::
                    ContextualSummaryChanged},
            std::pair{
                EvaluationKeyComponent::TailIngress,
                detail::FunctionEvaluationCacheMissReason::
                    TailIngressChanged},
        };
        const FunctionComponentHistory* baseline = nullptr;
        std::size_t baseline_distance =
            std::numeric_limits<std::size_t>::max();
        std::size_t baseline_primary = priority.size();
        for (const auto& candidate : histories->second) {
            std::size_t distance = 0u;
            std::size_t primary = priority.size();
            for (std::size_t priority_index = 0u;
                 priority_index < priority.size();
                 ++priority_index) {
                const auto index = static_cast<std::size_t>(
                    priority[priority_index].first);
                if (candidate.hashes[index] ==
                    key.component_hashes[index])
                    continue;
                ++distance;
                primary = std::min(primary, priority_index);
            }
            // A nearest historical context is the only causal baseline. On
            // equal component distance, prefer the primary reason with the
            // strongest documented priority, then the newest observation.
            if (baseline == nullptr ||
                std::tuple{distance, primary,
                           std::numeric_limits<std::uint64_t>::max() -
                               candidate.serial} <
                    std::tuple{baseline_distance, baseline_primary,
                               std::numeric_limits<std::uint64_t>::max() -
                                   baseline->serial}) {
                baseline = &candidate;
                baseline_distance = distance;
                baseline_primary = primary;
            }
        }
        if (baseline == nullptr)
            return detail::FunctionEvaluationCacheMissReason::Cold;
        for (const auto& [component, reason] : priority) {
            const auto index =
                static_cast<std::size_t>(component);
            if (baseline->hashes[index] !=
                key.component_hashes[index])
                return reason;
        }
        // Image identity/revision and local key-schema bytes belong to the
        // conservative function-shape component. This fallback also keeps a
        // diagnostic hash collision from escaping the one-reason invariant.
        return detail::FunctionEvaluationCacheMissReason::
            FunctionShapeChanged;
    }

    void remember_components(
        const FunctionEvaluationCacheKey& key) noexcept {
        if (!detailed_telemetry_) return;
        constexpr std::size_t maximum_contexts_per_function = 64u;
        constexpr std::size_t maximum_component_history = 65'536u;
        try {
            auto& histories =
                component_histories_by_function_[key.function_entry];
            if (std::any_of(
                    histories.begin(), histories.end(),
                    [&](const auto& candidate) {
                        return candidate.hashes ==
                               key.component_hashes;
                    }))
                return;
            const auto serial = ++next_history_serial_;
            histories.push_back(
                {key.component_hashes, serial});
            component_history_order_.emplace_back(
                key.function_entry, serial);
            if (histories.size() > maximum_contexts_per_function)
                histories.pop_front();
            while (component_history_order_.size() >
                   maximum_component_history) {
                const auto [function_entry, oldest_serial] =
                    component_history_order_.front();
                component_history_order_.pop_front();
                const auto found =
                    component_histories_by_function_.find(
                        function_entry);
                if (found == component_histories_by_function_.end())
                    continue;
                auto& candidates = found->second;
                const auto oldest = std::find_if(
                    candidates.begin(), candidates.end(),
                    [&](const auto& candidate) {
                        return candidate.serial == oldest_serial;
                    });
                if (oldest != candidates.end())
                    candidates.erase(oldest);
                if (candidates.empty())
                    component_histories_by_function_.erase(found);
            }
        } catch (...) {
            // Component history only refines a diagnostic miss reason. It
            // must never make telemetry-enabled analysis less reliable than
            // the canonical telemetry-off path.
        }
    }

    void remember_absent_key(
        const FunctionEvaluationCacheKey& key,
        const detail::FunctionEvaluationCacheMissReason reason) noexcept {
        if (!detailed_telemetry_) return;
        try {
            const auto digest = absent_key_digest(key);
            if (!digest) return;
            const auto fingerprint =
                key.diagnostic_fingerprint;
            if (const auto found =
                    absent_key_histories_.find(fingerprint);
                found != absent_key_histories_.end()) {
                const auto existing = std::find_if(
                    found->second.begin(), found->second.end(),
                    [&](const auto& candidate) {
                        return candidate.function_entry ==
                                   key.function_entry &&
                               candidate.sha256 == *digest;
                    });
                if (existing != found->second.end()) {
                    existing->reason = reason;
                    return;
                }
            }
            const auto evict_oldest = [&]() noexcept {
                if (absent_key_order_.empty()) return false;
                const auto [oldest_fingerprint, oldest_serial] =
                    absent_key_order_.front();
                absent_key_order_.pop_front();
                const auto found =
                    absent_key_histories_.find(oldest_fingerprint);
                if (found == absent_key_histories_.end()) return true;
                auto& candidates = found->second;
                const auto oldest = std::find_if(
                    candidates.begin(), candidates.end(),
                    [&](const auto& candidate) {
                        return candidate.serial == oldest_serial;
                    });
                if (oldest != candidates.end()) {
                    candidates.erase(oldest);
                    absent_key_history_accounted_bytes_ =
                        absent_key_history_accounted_bytes_ >=
                                absent_key_history_entry_charge
                            ? absent_key_history_accounted_bytes_ -
                                  absent_key_history_entry_charge
                            : 0u;
                }
                if (candidates.empty())
                    absent_key_histories_.erase(found);
                return true;
            };
            while (absent_key_order_.size() >=
                       maximum_absent_key_tombstones ||
                   absent_key_history_accounted_bytes_ >
                       maximum_absent_key_history_bytes -
                           absent_key_history_entry_charge) {
                if (!evict_oldest()) return;
            }
            const auto serial = ++next_history_serial_;
            const auto [bucket, inserted_bucket] =
                absent_key_histories_.try_emplace(fingerprint);
            auto& histories = bucket->second;
            try {
                histories.push_back(
                    {key.function_entry, *digest, reason, serial});
            } catch (...) {
                if (inserted_bucket)
                    absent_key_histories_.erase(bucket);
                throw;
            }
            try {
                absent_key_order_.emplace_back(
                    fingerprint, serial);
            } catch (...) {
                histories.pop_back();
                if (histories.empty())
                    absent_key_histories_.erase(fingerprint);
                throw;
            }
            absent_key_history_accounted_bytes_ +=
                absent_key_history_entry_charge;
        } catch (...) {
            // Diagnostic history is observational and must never affect
            // cache reuse or canonical analysis.
        }
    }

    [[nodiscard]] bool touch_ready(Entry& entry) noexcept {
        if (entry.last_use != 0u)
            ready_lru_.erase(
                {entry.last_use, entry.serial});
        entry.last_use = ++clock_;
        if (entry.last_use == 0u)
            entry.last_use = ++clock_;
        try {
            const auto [position, inserted] =
                ready_lru_.emplace(
                std::pair{entry.last_use, entry.serial},
                &entry);
            static_cast<void>(position);
            if (inserted) return true;
            entry.last_use = 0u;
            return false;
        } catch (...) {
            entry.last_use = 0u;
            return false;
        }
    }

    void erase(Entry* const entry,
               const bool eviction) noexcept {
        if (entry == nullptr) return;
        if (entry->last_use != 0u)
            ready_lru_.erase(
                {entry->last_use, entry->serial});
        const auto bucket = buckets_.find(entry->key.hash);
        if (bucket == buckets_.end()) return;
        const auto found = std::find_if(
            bucket->second.begin(),
            bucket->second.end(),
            [&](const auto& candidate) {
                return candidate.get() == entry;
            });
        if (found == bucket->second.end()) return;
        retained_payload_bytes_ -=
            (*found)->retained_payload_bytes;
        --entries_;
        if (eviction) {
            ++evictions_;
            remember_absent_key(
                (*found)->key,
                detail::FunctionEvaluationCacheMissReason::Evicted);
        }
        bucket->second.erase(found);
        if (bucket->second.empty())
            buckets_.erase(bucket);
    }

    void make_room_for(
                       const std::size_t incoming_payload_bytes,
                       const bool reserve_entry) noexcept {
        while (!ready_lru_.empty() &&
               ((reserve_entry
                     ? entries_ >= maximum_entries_
                     : entries_ > maximum_entries_) ||
                retained_payload_bytes_ >
                    maximum_retained_payload_bytes_ ||
                (incoming_payload_bytes <=
                     maximum_retained_payload_bytes_ &&
                 retained_payload_bytes_ >
                     maximum_retained_payload_bytes_ -
                         incoming_payload_bytes))) {
            erase(ready_lru_.begin()->second, true);
        }
    }

    std::unordered_map<
        std::uint64_t,
        std::vector<std::unique_ptr<Entry>>>
        buckets_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Entry*>
        ready_lru_;
    mutable std::mutex mutex_;
    std::size_t maximum_entries_ = 0u;
    std::size_t maximum_retained_payload_bytes_ = 0u;
    std::size_t entries_ = 0u;
    std::size_t retained_payload_bytes_ = 0u;
    std::uint64_t clock_ = 0u;
    std::uint64_t serial_ = 0u;
    std::size_t lookups_ = 0u;
    std::size_t ready_hits_ = 0u;
    std::size_t in_flight_coalesces_ = 0u;
    std::size_t misses_ = 0u;
    std::size_t evictions_ = 0u;
    std::array<std::size_t,
               detail::function_evaluation_cache_miss_reason_count>
        miss_reasons_{};
    EvaluationLensTelemetry evaluation_lenses_;
    std::unordered_map<
        std::uint32_t,
        std::deque<FunctionComponentHistory>>
        component_histories_by_function_;
    std::deque<std::pair<std::uint32_t, std::uint64_t>>
        component_history_order_;
    std::unordered_map<
        std::uint64_t,
        std::vector<AbsentKeyHistory>>
        absent_key_histories_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>>
        absent_key_order_;
    std::size_t absent_key_history_accounted_bytes_ = 0u;
    std::uint64_t next_history_serial_ = 0u;
    bool detailed_telemetry_ = false;
    detail::FunctionEvaluationCacheDecisionObserver decision_observer_;
};

} // namespace

detail::FunctionEvaluationCacheTelemetryProbe
detail::probe_function_evaluation_cache_telemetry_for_testing() {
    FunctionEvaluationCacheTelemetryProbe probe;
    std::atomic_size_t physical_computations = 0u;
    const auto add_statistics_to =
        [](FunctionValueAnalysisSessionStatistics& destination,
           const FunctionValueAnalysisSessionStatistics& source) {
            destination.lookups += source.lookups;
            destination.ready_hits += source.ready_hits;
            destination.in_flight_coalesces +=
                source.in_flight_coalesces;
            destination.hits =
                destination.ready_hits +
                destination.in_flight_coalesces;
            destination.misses += source.misses;
            destination.evictions += source.evictions;
            destination.entries += source.entries;
            destination.retained_payload_bytes +=
                source.retained_payload_bytes;
            for (std::size_t index = 0u;
                 index < destination.miss_reasons.size();
                 ++index)
                destination.miss_reasons[index] +=
                    source.miss_reasons[index];
            for (std::size_t index = 0u;
                 index < evaluation_lens_count;
                 ++index) {
                destination.evaluation_lenses.requests[index] +=
                    source.evaluation_lenses.requests[index];
                destination.evaluation_lenses.cache_hits[index] +=
                    source.evaluation_lenses.cache_hits[index];
                destination.evaluation_lenses
                    .avoided_evaluation_nanoseconds[index] +=
                    source.evaluation_lenses
                        .avoided_evaluation_nanoseconds[index];
            }
            destination.evaluation_lenses.full_state_fallbacks +=
                source.evaluation_lenses.full_state_fallbacks;
            destination.evaluation_lenses.projected_evaluations +=
                source.evaluation_lenses.projected_evaluations;
            destination.evaluation_lenses.reconstructed_results +=
                source.evaluation_lenses.reconstructed_results;
            destination.evaluation_lenses.key_interned_sets +=
                source.evaluation_lenses.key_interned_sets;
            destination.evaluation_lenses.key_interned_references +=
                source.evaluation_lenses.key_interned_references;
        };
    const auto add_statistics =
        [&](const FunctionValueAnalysisSessionStatistics& source) {
            add_statistics_to(probe.statistics, source);
        };
    const auto make_key =
        [](const std::uint32_t function_entry,
           const std::uint8_t variant,
           const std::optional<EvaluationKeyComponent>
               changed_component = std::nullopt) {
            FunctionEvaluationCacheKey key;
            key.function_entry = function_entry;
            key.component_hashes.fill(
                evaluation_key_hash_basis);
            if (changed_component) {
                key.component_hashes[
                    static_cast<std::size_t>(
                        *changed_component)] ^=
                    static_cast<std::uint64_t>(variant) +
                    0x9e3779b97f4a7c15ull;
            }
            key.bytes = {
                static_cast<std::uint8_t>(
                    function_entry & 0xffu),
                static_cast<std::uint8_t>(
                    (function_entry >> 8u) & 0xffu),
                variant,
                changed_component
                    ? static_cast<std::uint8_t>(
                          *changed_component)
                    : std::uint8_t{0xffu}};
            key.hash = evaluation_key_hash(key.bytes);
            key.diagnostic_fingerprint =
                evaluation_key_diagnostic_fingerprint(
                    key.bytes);
            return key;
        };
    const auto artifact = [] {
        return std::make_shared<
            const CachedFunctionEvaluation>();
    };
    probe.inline_only_artifact_owner_bytes =
        sizeof(CachedFunctionEvaluation);
    probe.inline_only_artifact_bytes =
        artifact()->retained_payload_budget_bytes();
    EvaluationActivityTelemetry probe_activity;

    {
        FunctionEvaluationCache cache(
            64u, 64u * 1024u * 1024u, true);
        std::promise<void> producer_entered;
        auto producer_entered_future =
            producer_entered.get_future();
        std::promise<void> release_producer;
        const auto release_future =
            release_producer.get_future().share();
        const auto shared_key =
            make_key(1u, 0u);
        auto first = std::async(
            std::launch::async,
            [&] {
                return cache.get_or_compute(
                    shared_key,
                    [&] {
                        physical_computations.fetch_add(
                            1u, std::memory_order_relaxed);
                        producer_entered.set_value();
                        release_future.wait();
                        return artifact();
                    },
                    &probe_activity);
            });
        producer_entered_future.wait();
        auto second = std::async(
            std::launch::async,
            [&] {
                return cache.get_or_compute(
                    shared_key,
                    [&] {
                        physical_computations.fetch_add(
                            1u, std::memory_order_relaxed);
                        return artifact();
                    },
                    &probe_activity);
            });
        const auto coalesce_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (cache.statistics()
                       .in_flight_coalesces == 0u &&
               std::chrono::steady_clock::now() <
                   coalesce_deadline)
            std::this_thread::yield();
        release_producer.set_value();
        static_cast<void>(first.get());
        static_cast<void>(second.get());
        static_cast<void>(cache.get_or_compute(
            shared_key,
            [&] {
                physical_computations.fetch_add(
                    1u, std::memory_order_relaxed);
                return artifact();
            },
            &probe_activity));

        for (std::size_t component_index = 0u;
             component_index <
             evaluation_key_component_count;
             ++component_index) {
            const auto function_entry =
                static_cast<std::uint32_t>(
                    100u + component_index);
            static_cast<void>(cache.get_or_compute(
                make_key(function_entry, 0u),
                [&] {
                    physical_computations.fetch_add(
                        1u, std::memory_order_relaxed);
                    return artifact();
                }));
            static_cast<void>(cache.get_or_compute(
                make_key(
                    function_entry,
                    1u,
                    static_cast<EvaluationKeyComponent>(
                        component_index)),
                [&] {
                    physical_computations.fetch_add(
                        1u, std::memory_order_relaxed);
                    return artifact();
                }));
        }
        const auto statistics = cache.statistics();
        add_statistics(statistics);
    }
    const auto& wait_metric = probe_activity.metric(
        EvaluationActivityKind::CacheWait);
    probe.in_flight_waits = wait_metric.count.load(
        std::memory_order_relaxed);
    probe.in_flight_wait_nanoseconds =
        wait_metric.cumulative_nanoseconds.load(
            std::memory_order_relaxed);
    probe.maximum_in_flight_wait_nanoseconds =
        wait_metric.maximum_nanoseconds.load(
            std::memory_order_relaxed);

    {
        FunctionEvaluationCache cache(
            1u, 64u * 1024u * 1024u, true);
        const auto first_key = make_key(500u, 0u);
        static_cast<void>(cache.get_or_compute(
            first_key,
            [&] {
                physical_computations.fetch_add(
                    1u, std::memory_order_relaxed);
                return artifact();
            }));
        static_cast<void>(cache.get_or_compute(
            make_key(501u, 0u),
            [&] {
                physical_computations.fetch_add(
                    1u, std::memory_order_relaxed);
                return artifact();
            }));
        static_cast<void>(cache.get_or_compute(
            first_key,
            [&] {
                physical_computations.fetch_add(
                    1u, std::memory_order_relaxed);
                return artifact();
            }));
        const auto statistics = cache.statistics();
        add_statistics(statistics);
    }

    {
        FunctionEvaluationCache cache(0u, 0u, true);
        static_cast<void>(cache.get_or_compute(
            make_key(600u, 0u),
            [&] {
                physical_computations.fetch_add(
                    1u, std::memory_order_relaxed);
                return artifact();
            }));
        add_statistics(cache.statistics());
    }

    // Alternate two contexts for one function, then mutate a different
    // component relative to each respective context. A last-context-only
    // classifier reports the ingress delta for both follow-ups; nearest
    // compatible history identifies the actual summary/ABI causes.
    {
        std::mutex decisions_mutex;
        FunctionEvaluationCache cache(
            64u,
            64u * 1024u * 1024u,
            true,
            [&](const FunctionEvaluationCacheDecision& decision) {
                const std::lock_guard lock(decisions_mutex);
                probe.decisions.push_back(decision);
            });
        const auto contextual_key =
            [&](const std::uint8_t variant,
                const bool alternate_ingress,
                const std::optional<EvaluationKeyComponent> mutation) {
                auto key = make_key(700u, variant);
                if (alternate_ingress)
                    key.component_hashes[static_cast<std::size_t>(
                        EvaluationKeyComponent::ProjectedIngress)] ^=
                        0x1111111111111111ull;
                if (mutation)
                    key.component_hashes[static_cast<std::size_t>(
                        *mutation)] ^=
                        0x2222222222222222ull;
                key.bytes.push_back(alternate_ingress ? 1u : 0u);
                key.bytes.push_back(
                    mutation ? static_cast<std::uint8_t>(*mutation)
                             : std::uint8_t{0xffu});
                key.hash = evaluation_key_hash(key.bytes);
                key.diagnostic_fingerprint =
                    evaluation_key_diagnostic_fingerprint(key.bytes);
                return key;
            };
        for (auto key : {
                 contextual_key(0u, false, std::nullopt),
                 contextual_key(1u, true, std::nullopt),
                 contextual_key(
                     2u,
                     false,
                     EvaluationKeyComponent::SummaryDependency),
                 contextual_key(
                     3u,
                     true,
                     EvaluationKeyComponent::AbiContract)}) {
            static_cast<void>(cache.get_or_compute(
                std::move(key),
                [&] {
                    physical_computations.fetch_add(
                        1u, std::memory_order_relaxed);
                    return artifact();
                }));
        }
        const auto statistics = cache.statistics();
        add_statistics(statistics);
        probe.observer_statistics = statistics;
    }

    {
        constexpr std::uint32_t bounded_function = 750u;
        constexpr std::size_t context_limit = 64u;
        FunctionEvaluationCache cache(
            128u, 64u * 1024u * 1024u, true);
        static_cast<void>(cache.get_or_compute(
            make_key(bounded_function, 0u), artifact));
        for (std::uint8_t variant = 1u; variant <= 80u; ++variant) {
            static_cast<void>(cache.get_or_compute(
                make_key(
                    bounded_function,
                    variant,
                    EvaluationKeyComponent::ContextualSummary),
                artifact));
        }
        probe.bounded_context_history_entries =
            cache.component_history_entries_for_testing(
                bounded_function);
        probe.bounded_context_history_limit = context_limit;
    }

    {
        FunctionEvaluationCache cache(
            1u, 64u * 1024u * 1024u, true);
        for (std::uint32_t index = 0u; index < 5'000u; ++index) {
            static_cast<void>(cache.get_or_compute(
                make_key(10'000u + index, 0u), artifact));
        }
        probe.bounded_absent_history_entries =
            cache.absent_history_entries_for_testing();
        probe.bounded_absent_history_accounted_bytes =
            cache.absent_history_accounted_bytes_for_testing();
        probe.bounded_absent_history_byte_limit =
            cache.absent_history_byte_limit_for_testing();
    }

    // Observer exceptions are observational loss only. They must not turn a
    // retained miss into a recomputation or suppress the following hit.
    {
        FunctionEvaluationCache cache(
            4u,
            64u * 1024u * 1024u,
            true,
            [](const FunctionEvaluationCacheDecision&) {
                throw std::runtime_error("synthetic-cache-observer");
            });
        const auto key = make_key(800u, 0u);
        const auto first = cache.get_or_compute(key, artifact);
        const auto second = cache.get_or_compute(key, artifact);
        probe.throwing_observer_semantics_preserved =
            !first.second && second.second &&
            first.first == second.first &&
            cache.statistics().balanced();
    }

    // The empty artifact proves that embedded evaluation, register-summary
    // and collector owners are not counted again by recursive estimators.
    // A controlled artifact then proves exact byte-limit admission.
    {
        auto controlled =
            std::make_shared<CachedFunctionEvaluation>();
        controlled->evaluation.summary.registers.reserve(5u);
        controlled->evaluation.summary.registers.push_back({});
        controlled->evaluation.summary.registers.back().values.reserve(7u);
        controlled->evaluation.summary.registers.back().values.push_back(1u);
        controlled->evaluation.resolutions.reserve(3u);
        controlled->evaluation.resolutions.push_back({});
        controlled->evaluation.resolutions.back().targets.reserve(11u);
        controlled->evaluation.resolutions.back().targets.push_back(2u);
        const auto controlled_artifact =
            std::shared_ptr<const CachedFunctionEvaluation>{controlled};
        probe.controlled_artifact_bytes =
            controlled_artifact->retained_payload_budget_bytes();
        const auto key = make_key(900u, 0u);
        FunctionEvaluationCache measuring(
            1u, 64u * 1024u * 1024u, true);
        static_cast<void>(measuring.get_or_compute(
            key, [&] { return controlled_artifact; }));
        probe.controlled_entry_retained_payload_bytes =
            measuring.statistics().retained_payload_bytes;

        FunctionEvaluationCache exact(
            1u,
            probe.controlled_entry_retained_payload_bytes,
            true);
        static_cast<void>(exact.get_or_compute(
            key, [&] { return controlled_artifact; }));
        probe.exact_limit_entries = exact.statistics().entries;
        probe.exact_limit_retained_payload_bytes =
            exact.statistics().retained_payload_bytes;

        FunctionEvaluationCache short_limit(
            1u,
            probe.controlled_entry_retained_payload_bytes == 0u
                ? 0u
                : probe.controlled_entry_retained_payload_bytes - 1u,
            true,
            [&](const FunctionEvaluationCacheDecision& decision) {
                probe.decisions.push_back(decision);
            });
        static_cast<void>(short_limit.get_or_compute(
            key, [&] { return controlled_artifact; }));
        probe.one_byte_short_entries =
            short_limit.statistics().entries;
        probe.one_byte_short_retained_payload_bytes =
            short_limit.statistics().retained_payload_bytes;
        add_statistics_to(
            probe.observer_statistics,
            short_limit.statistics());
    }

    if (!probe.statistics.balanced())
        throw std::logic_error(
            "Function-Evaluation-Cache-Telemetrie ist unausgeglichen.");
    probe.physical_computations =
        physical_computations.load(
            std::memory_order_relaxed);
    return probe;
}

detail::FunctionValueProgressRuntimeStatistics
detail::function_value_progress_runtime_statistics_for_testing() noexcept {
    return {
        function_value_progress_callback_activations.load(
            std::memory_order_relaxed),
        function_value_progress_pulse_threads_started.load(
            std::memory_order_relaxed),
        function_value_detailed_cache_sessions_started.load(
            std::memory_order_relaxed)};
}

struct detail::FunctionValueAnalysisSession::Impl {
    Impl(const std::size_t maximum_entries,
         const std::size_t maximum_retained_payload_bytes,
         const bool detailed_telemetry,
         FunctionEvaluationCacheDecisionObserver decision_observer)
        : evaluations(maximum_entries,
                      maximum_retained_payload_bytes,
                      detailed_telemetry,
                      std::move(decision_observer)) {
        if (detailed_telemetry) {
            function_value_detailed_cache_sessions_started.fetch_add(
                1u, std::memory_order_relaxed);
        }
    }

    void bind(const katana::io::ExecutableImage& image) {
        const auto identity =
            image.analysis_instance_identity();
        const auto revision = image.analysis_revision();
        if (bound_image_identity == identity &&
            bound_image_revision == revision)
            return;
        evaluations.clear();
        bound_image_identity = identity;
        bound_image_revision = revision;
    }

    std::mutex analysis_mutex;
    FunctionEvaluationCache evaluations;
    std::uint64_t bound_image_identity = 0u;
    std::uint64_t bound_image_revision = 0u;
};

detail::FunctionValueAnalysisSession::FunctionValueAnalysisSession(
    const std::size_t maximum_entries,
    const std::size_t maximum_retained_payload_bytes,
    const bool detailed_telemetry,
    FunctionEvaluationCacheDecisionObserver decision_observer)
    : impl_(std::make_unique<Impl>(
          maximum_entries,
          maximum_retained_payload_bytes,
          detailed_telemetry,
          std::move(decision_observer))) {}

detail::FunctionValueAnalysisSession::~FunctionValueAnalysisSession() =
    default;

detail::FunctionValueAnalysisSession::FunctionValueAnalysisSession(
    FunctionValueAnalysisSession&&) noexcept = default;

detail::FunctionValueAnalysisSession&
detail::FunctionValueAnalysisSession::operator=(
    FunctionValueAnalysisSession&&) noexcept = default;

detail::FunctionValueAnalysisSessionStatistics
detail::FunctionValueAnalysisSession::statistics() const {
    return impl_->evaluations.statistics();
}

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    return analyze_function_values(
        image,
        lines,
        function_entries,
        resolved_edges,
        FunctionValueAnalysisProgressCallback{});
}

FunctionValueAnalysisResult
analyze_function_values(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    return analyze_function_values(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        FunctionValueAnalysisProgressCallback{});
}

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback) {
    std::vector<FunctionBoundary> boundaries;
    boundaries.reserve(function_entries.size());
    for (const auto entry : function_entries)
        boundaries.push_back({entry, 0u});
    return analyze_function_values(
        image, lines, boundaries, resolved_edges, progress_callback);
}

FunctionValueAnalysisResult
analyze_function_values(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback) {
    detail::GuardedNativeEntryShapeCache guarded_native_entry_shapes(image);
    detail::FunctionValueAnalysisSession session(
        16'384u,
        1'024u * 1024u * 1024u,
        false);
    return detail::analyze_function_values_with_guarded_entry_cache(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        progress_callback,
        guarded_native_entry_shapes,
        session);
}

FunctionValueAnalysisResult
detail::analyze_function_values_with_abi_contract_observer_for_testing(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const AbiContractObserver& observer) {
    detail::GuardedNativeEntryShapeCache guarded_native_entry_shapes(image);
    detail::FunctionValueAnalysisSession session;
    return detail::analyze_function_values_with_guarded_entry_cache(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        {},
        guarded_native_entry_shapes,
        session,
        observer);
}

FunctionValueAnalysisResult
detail::analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    detail::GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
    const AbiContractObserver& abi_contract_observer) {
    detail::FunctionValueAnalysisSession session(
        16'384u,
        1'024u * 1024u * 1024u,
        false);
    return detail::analyze_function_values_with_guarded_entry_cache(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        progress_callback,
        guarded_native_entry_shapes,
        session,
        abi_contract_observer);
}

FunctionValueAnalysisResult
detail::analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    detail::GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
    detail::FunctionValueAnalysisSession& session,
    const AbiContractObserver& abi_contract_observer) {
    const std::unique_lock session_analysis_lock(
        session.impl_->analysis_mutex);
    guarded_native_entry_shapes.bind(image);
    session.impl_->bind(image);
    const auto session_statistics_at_start =
        session.statistics();
    begin_detailed_analyzer_diagnostic_epoch();
    FunctionValueAnalysisResult result;
    result.iteration_budget = maximum_fixpoint_iterations;
    std::size_t summarized_functions = 0u;
    std::size_t resolution_functions_committed = 0u;
    std::size_t resolution_count = 0u;
    std::size_t block_count = 0u;
    std::size_t function_count = 0u;
    std::size_t pending_count = 0u;
    std::size_t resolution_functions_total = 0u;
    std::atomic_size_t logical_evaluations = 0u;
    std::atomic_size_t physical_evaluations = 0u;
    std::atomic_size_t cache_replay_fallback_recomputes = 0u;
    std::atomic_size_t cache_diagnostic_bypass_evaluations = 0u;
    EvaluationActivityTelemetry evaluation_activity;
    ParallelWorkActivity function_value_parallel_activity;
    auto* const evaluation_activity_if_observed =
        progress_callback ? &evaluation_activity : nullptr;
    auto* const function_value_parallel_activity_if_observed =
        progress_callback ? &function_value_parallel_activity : nullptr;
    std::atomic_bool progress_callback_failed = false;
    std::atomic_size_t progress_function_count = 0u;
    std::atomic_size_t progress_block_count = 0u;
    std::atomic_size_t progress_fixpoint_iterations = 0u;
    std::atomic_size_t progress_summarized_functions = 0u;
    std::atomic_size_t progress_pending_count = 0u;
    std::atomic_size_t progress_resolution_count = 0u;
    std::atomic_size_t progress_resolution_functions_total = 0u;
    std::atomic_size_t progress_resolution_functions_started = 0u;
    std::atomic_size_t progress_resolution_functions_ready = 0u;
    std::atomic_size_t progress_resolution_functions_committed = 0u;
    std::atomic_size_t progress_resolution_head_of_line_index = 0u;
    std::atomic<std::int64_t>
        progress_resolution_head_started_nanoseconds{0};
    const auto configured_workers =
        progress_callback
            ? global_analysis_executor().worker_count()
            : 1u;
    std::mutex progress_callback_mutex;
    std::string progress_subphase = "initialization";
    std::atomic_size_t progress_subphase_planned = 0u;
    std::atomic_size_t progress_subphase_processed = 0u;
    std::atomic_size_t progress_subphase_queued = 0u;
    std::atomic_size_t progress_subphase_iterations = 0u;
    const auto emit_progress_snapshot_locked =
        [&](const std::string_view phase) noexcept {
        if (!progress_callback) return;
        try {
            const auto session_statistics =
                session.statistics();
            const auto cache_delta =
                [](const std::size_t current,
                   const std::size_t initial) noexcept {
                    return current >= initial
                               ? current - initial
                               : 0u;
                };
            const auto cache_delta_u64 =
                [](const std::uint64_t current,
                   const std::uint64_t initial) noexcept {
                    return current >= initial
                               ? current - initial
                               : std::uint64_t{0u};
                };
            const auto miss_reason_delta =
                [&](const detail::
                        FunctionEvaluationCacheMissReason reason) noexcept {
                    const auto index =
                        static_cast<std::size_t>(reason);
                    return cache_delta(
                        session_statistics.miss_reasons[index],
                        session_statistics_at_start
                            .miss_reasons[index]);
                };
            FunctionValueAnalysisProgress progress;
            progress.phase = phase;
            progress.subphase = progress_subphase;
            progress.subphase_planned =
                progress_subphase_planned.load(
                    std::memory_order_relaxed);
            progress.subphase_processed =
                progress_subphase_processed.load(
                    std::memory_order_relaxed);
            progress.subphase_queued =
                progress_subphase_queued.load(
                    std::memory_order_relaxed);
            progress.subphase_iterations =
                progress_subphase_iterations.load(
                    std::memory_order_relaxed);
            progress.functions = progress_function_count.load(
                std::memory_order_relaxed);
            progress.blocks = progress_block_count.load(
                std::memory_order_relaxed);
            progress.fixpoint_iterations =
                progress_fixpoint_iterations.load(
                    std::memory_order_relaxed);
            progress.summarized_functions =
                progress_summarized_functions.load(
                    std::memory_order_relaxed);
            progress.pending = progress_pending_count.load(
                std::memory_order_relaxed);
            progress.resolutions =
                progress_resolution_count.load(
                    std::memory_order_relaxed);
            progress.active_workers =
                function_value_parallel_activity
                    .active_worker_count();
            progress.logical_evaluations =
                logical_evaluations.load(
                    std::memory_order_relaxed);
            progress.physical_evaluations =
                physical_evaluations.load(
                    std::memory_order_relaxed);
            const auto load_activity =
                [&](const EvaluationActivityKind kind) {
                    const auto& metric =
                        evaluation_activity.metric(kind);
                    const auto maximum =
                        metric.maximum_nanoseconds.load(
                            std::memory_order_acquire);
                    const auto cumulative =
                        metric.cumulative_nanoseconds.load(
                            std::memory_order_acquire);
                    const auto active = metric.active.load(
                        std::memory_order_acquire);
                    const auto count = metric.count.load(
                        std::memory_order_acquire);
                    return std::array<std::uint64_t, 4u>{
                        active, count, cumulative, maximum};
                };
            const auto request = load_activity(
                EvaluationActivityKind::Request);
            progress.active_evaluation_requests = request[0u];
            progress.logical_evaluations = request[1u];
            progress.evaluation_request_nanoseconds = request[2u];
            progress.maximum_evaluation_request_nanoseconds =
                request[3u];
            const auto key_build = load_activity(
                EvaluationActivityKind::KeyBuild);
            progress.active_cache_key_builds = key_build[0u];
            progress.cache_key_builds = key_build[1u];
            progress.cache_key_build_nanoseconds = key_build[2u];
            progress.maximum_cache_key_build_nanoseconds =
                key_build[3u];
            const auto wait = load_activity(
                EvaluationActivityKind::CacheWait);
            progress.active_cache_waits = wait[0u];
            progress.cache_waits = wait[1u];
            progress.cache_wait_nanoseconds = wait[2u];
            progress.maximum_cache_wait_nanoseconds = wait[3u];
            const auto replay = load_activity(
                EvaluationActivityKind::ExactReplay);
            progress.active_cache_replays = replay[0u];
            progress.cache_replays = replay[1u];
            progress.cache_replay_nanoseconds = replay[2u];
            progress.maximum_cache_replay_nanoseconds = replay[3u];
            const auto physical = load_activity(
                EvaluationActivityKind::PhysicalInterpreter);
            progress.active_physical_evaluations = physical[0u];
            progress.physical_evaluations = physical[1u];
            progress.physical_evaluation_nanoseconds = physical[2u];
            progress.maximum_physical_evaluation_nanoseconds =
                physical[3u];
            const auto commit = load_activity(
                EvaluationActivityKind::CacheCommit);
            progress.active_cache_commits = commit[0u];
            progress.cache_commits = commit[1u];
            progress.cache_commit_nanoseconds = commit[2u];
            progress.maximum_cache_commit_nanoseconds = commit[3u];
            progress.cache_replay_fallback_recomputes =
                cache_replay_fallback_recomputes.load(
                    std::memory_order_relaxed);
            progress.cache_diagnostic_bypass_evaluations =
                cache_diagnostic_bypass_evaluations.load(
                    std::memory_order_relaxed);
            progress.resolution_functions_total =
                progress_resolution_functions_total.load(
                    std::memory_order_relaxed);
            progress.resolution_functions_started =
                progress_resolution_functions_started.load(
                    std::memory_order_relaxed);
            progress.resolution_functions_ready =
                progress_resolution_functions_ready.load(
                    std::memory_order_relaxed);
            progress.resolution_functions_committed =
                progress_resolution_functions_committed.load(
                    std::memory_order_relaxed);
            progress.resolution_head_of_line_index =
                progress_resolution_head_of_line_index.load(
                    std::memory_order_relaxed);
            const auto head_started =
                progress_resolution_head_started_nanoseconds.load(
                    std::memory_order_relaxed);
            if (head_started != 0) {
                const auto now =
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch())
                        .count();
                if (now >= head_started) {
                    progress
                        .resolution_head_of_line_elapsed_milliseconds =
                        static_cast<std::size_t>(
                            (now - head_started) / 1'000'000);
                }
            }
            progress.configured_workers = configured_workers;
            progress.session_cache_lookups = cache_delta(
                session_statistics.lookups,
                session_statistics_at_start.lookups);
            progress.session_cache_ready_hits = cache_delta(
                session_statistics.ready_hits,
                session_statistics_at_start.ready_hits);
            progress.session_cache_in_flight_coalesces =
                cache_delta(
                    session_statistics.in_flight_coalesces,
                    session_statistics_at_start
                        .in_flight_coalesces);
            progress.session_cache_hits = cache_delta(
                session_statistics.hits,
                session_statistics_at_start.hits);
            progress.session_cache_misses = cache_delta(
                session_statistics.misses,
                session_statistics_at_start.misses);
            progress.session_cache_evictions = cache_delta(
                session_statistics.evictions,
                session_statistics_at_start.evictions);
            progress.session_cache_entries =
                session_statistics.entries;
            progress.session_cache_retained_payload_bytes =
                session_statistics.retained_payload_bytes;
            progress.session_cache_miss_cold =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        Cold);
            progress.session_cache_miss_evicted =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        Evicted);
            progress
                .session_cache_miss_oversize_or_no_exact_replay =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        OversizeOrNoExactReplay);
            progress
                .session_cache_miss_function_shape_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        FunctionShapeChanged);
            progress
                .session_cache_miss_projected_ingress_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        ProjectedIngressChanged);
            progress
                .session_cache_miss_summary_dependency_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        SummaryDependencyChanged);
            progress.session_cache_miss_abi_contract_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        AbiContractChanged);
            progress
                .session_cache_miss_resolution_lens_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        ResolutionLensChanged);
            progress
                .session_cache_miss_inventory_sink_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        InventorySinkChanged);
            progress
                .session_cache_miss_isolation_partition_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        IsolationPartitionChanged);
            progress
                .session_cache_miss_contextual_summary_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        ContextualSummaryChanged);
            progress
                .session_cache_miss_tail_ingress_changed =
                miss_reason_delta(
                    detail::FunctionEvaluationCacheMissReason::
                        TailIngressChanged);
            for (std::size_t index = 0u;
                 index < evaluation_lens_count;
                 ++index) {
                progress.evaluation_lenses.requests[index] =
                    cache_delta(
                        session_statistics.evaluation_lenses
                            .requests[index],
                        session_statistics_at_start.evaluation_lenses
                            .requests[index]);
                progress.evaluation_lenses.cache_hits[index] =
                    cache_delta(
                        session_statistics.evaluation_lenses
                            .cache_hits[index],
                        session_statistics_at_start.evaluation_lenses
                            .cache_hits[index]);
                progress.evaluation_lenses
                    .avoided_evaluation_nanoseconds[index] =
                    cache_delta_u64(
                        session_statistics.evaluation_lenses
                            .avoided_evaluation_nanoseconds[index],
                        session_statistics_at_start.evaluation_lenses
                            .avoided_evaluation_nanoseconds[index]);
            }
            progress.evaluation_lenses.full_state_fallbacks =
                cache_delta(
                    session_statistics.evaluation_lenses
                        .full_state_fallbacks,
                    session_statistics_at_start.evaluation_lenses
                        .full_state_fallbacks);
            progress.evaluation_lenses.projected_evaluations =
                cache_delta(
                    session_statistics.evaluation_lenses
                        .projected_evaluations,
                    session_statistics_at_start.evaluation_lenses
                        .projected_evaluations);
            progress.evaluation_lenses.reconstructed_results =
                cache_delta(
                    session_statistics.evaluation_lenses
                        .reconstructed_results,
                    session_statistics_at_start.evaluation_lenses
                        .reconstructed_results);
            progress.evaluation_lenses.key_interned_sets =
                cache_delta(
                    session_statistics.evaluation_lenses
                        .key_interned_sets,
                    session_statistics_at_start.evaluation_lenses
                        .key_interned_sets);
            progress.evaluation_lenses.key_interned_references =
                cache_delta(
                    session_statistics.evaluation_lenses
                        .key_interned_references,
                    session_statistics_at_start.evaluation_lenses
                        .key_interned_references);
            progress_callback(progress);
        } catch (...) {
            // Progress is observational. A failing UI/logger callback must
            // never alter analysis output or abort a product build.
            progress_callback_failed.store(
                true, std::memory_order_relaxed);
        }
    };
    const auto emit_progress_snapshot =
        [&](const std::string_view phase) noexcept {
        const std::lock_guard lock(
            progress_callback_mutex);
        emit_progress_snapshot_locked(phase);
    };
    const auto report_progress = [&](const std::string_view phase) {
        const std::lock_guard lock(
            progress_callback_mutex);
        progress_subphase = phase;
        progress_subphase_planned.store(
            0u, std::memory_order_relaxed);
        progress_subphase_processed.store(
            0u, std::memory_order_relaxed);
        progress_subphase_queued.store(
            0u, std::memory_order_relaxed);
        progress_subphase_iterations.store(
            0u, std::memory_order_relaxed);
        progress_function_count.store(
            function_count, std::memory_order_relaxed);
        progress_block_count.store(
            block_count, std::memory_order_relaxed);
        progress_fixpoint_iterations.store(
            result.fixpoint_iterations,
            std::memory_order_relaxed);
        progress_summarized_functions.store(
            summarized_functions,
            std::memory_order_relaxed);
        progress_resolution_functions_committed.store(
            resolution_functions_committed,
            std::memory_order_relaxed);
        progress_pending_count.store(
            pending_count, std::memory_order_relaxed);
        progress_resolution_count.store(
            resolution_count, std::memory_order_relaxed);
        progress_resolution_functions_total.store(
            resolution_functions_total,
            std::memory_order_relaxed);
        emit_progress_snapshot_locked(phase);
    };
    const auto report_subphase_progress =
        [&](const std::string_view phase,
            const std::string_view subphase,
            const std::size_t planned,
            const std::size_t processed,
            const std::size_t queued,
            const std::size_t iterations) {
            const std::lock_guard lock(progress_callback_mutex);
            progress_subphase = subphase;
            progress_subphase_planned.store(
                planned, std::memory_order_relaxed);
            progress_subphase_processed.store(
                processed, std::memory_order_relaxed);
            progress_subphase_queued.store(
                queued, std::memory_order_relaxed);
            progress_subphase_iterations.store(
                iterations, std::memory_order_relaxed);
            progress_function_count.store(
                function_count, std::memory_order_relaxed);
            progress_block_count.store(
                block_count, std::memory_order_relaxed);
            progress_fixpoint_iterations.store(
                result.fixpoint_iterations,
                std::memory_order_relaxed);
            progress_summarized_functions.store(
                summarized_functions,
                std::memory_order_relaxed);
            progress_resolution_functions_committed.store(
                resolution_functions_committed,
                std::memory_order_relaxed);
            progress_pending_count.store(
                pending_count, std::memory_order_relaxed);
            progress_resolution_count.store(
                resolution_count, std::memory_order_relaxed);
            progress_resolution_functions_total.store(
                resolution_functions_total,
                std::memory_order_relaxed);
            emit_progress_snapshot_locked(phase);
        };
    if (lines.empty() || function_boundaries.empty() ||
        image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC)
        return result;
    if (progress_callback) {
        function_value_progress_callback_activations.fetch_add(
            1u, std::memory_order_relaxed);
    }
    report_progress("start");
    std::condition_variable_any progress_pulse_condition;
    std::mutex progress_pulse_mutex;
    std::jthread progress_pulse;
    if (progress_callback) {
        try {
            progress_pulse = std::jthread(
                [&](const std::stop_token stop) {
                    std::unique_lock wait_lock(
                        progress_pulse_mutex);
                    while (!stop.stop_requested()) {
                        static_cast<void>(
                            progress_pulse_condition.wait_for(
                                wait_lock,
                                stop,
                                std::chrono::seconds{1},
                                [] { return false; }));
                        if (stop.stop_requested()) break;
                        wait_lock.unlock();
                        emit_progress_snapshot("heartbeat");
                        wait_lock.lock();
                    }
                });
            function_value_progress_pulse_threads_started.fetch_add(
                1u, std::memory_order_relaxed);
        } catch (...) {
            // A platform thread-limit must only disable the optional live
            // pulse; synchronous progress and the analysis remain valid.
        }
    }
    const auto stop_progress_pulse = [&] {
        if (!progress_pulse.joinable()) return;
        progress_pulse.request_stop();
        progress_pulse_condition.notify_all();
        progress_pulse.join();
    };
    struct CandidateTailCarrier {
        std::uint32_t transfer_site = 0u;
        std::uint32_t target = 0u;
    };
    struct CandidateCallCarrier {
        std::uint32_t call_site = 0u;
        std::uint32_t target = 0u;
    };
    std::vector<CandidateCallCarrier> candidate_call_carriers;
    std::vector<CandidateTailCarrier> candidate_tail_carriers;
    for (const auto& edge : resolved_edges) {
        if (!edge.analysis_candidate_carrier ||
            edge.kind != ResolvedControlFlowKind::Call ||
            resolved_edge_evidence(edge) != ControlFlowEvidence::GuardedPartial)
            continue;
        const auto line = std::lower_bound(
            lines.begin(),
            lines.end(),
            edge.instruction_address,
            [](const auto& candidate, const std::uint32_t address) {
                return candidate.address < address;
            });
        const auto target =
            std::lower_bound(lines.begin(),
                             lines.end(),
                             edge.target_address,
                             [](const auto& candidate, const std::uint32_t address) {
                                 return candidate.address < address;
                             });
        if (line == lines.end() || line->address != edge.instruction_address ||
            target == lines.end() || target->address != edge.target_address ||
            target->is_delay_slot)
            continue;
        if (line->instruction.control_flow ==
            katana::sh4::ControlFlowKind::IndirectCall) {
            candidate_call_carriers.push_back(
                {edge.instruction_address, edge.target_address});
            continue;
        }
        if (line->instruction.control_flow !=
            katana::sh4::ControlFlowKind::IndirectBranch)
            continue;
        if (std::find(edge.evidence_origins.begin(),
                      edge.evidence_origins.end(),
                      AnalysisEvidenceOrigin::JumpTable) != edge.evidence_origins.end())
            continue;
        candidate_tail_carriers.push_back(
            {edge.instruction_address, edge.target_address});
    }
    std::sort(candidate_call_carriers.begin(),
              candidate_call_carriers.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.call_site, left.target) <
                         std::tie(right.call_site, right.target);
              });
    candidate_call_carriers.erase(
        std::unique(candidate_call_carriers.begin(),
                    candidate_call_carriers.end(),
                    [](const auto& left, const auto& right) {
                        return left.call_site == right.call_site &&
                               left.target == right.target;
                    }),
        candidate_call_carriers.end());
    std::sort(candidate_tail_carriers.begin(),
              candidate_tail_carriers.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.transfer_site, left.target) <
                         std::tie(right.transfer_site, right.target);
              });
    candidate_tail_carriers.erase(
        std::unique(candidate_tail_carriers.begin(),
                    candidate_tail_carriers.end(),
                    [](const auto& left, const auto& right) {
                        return left.transfer_site == right.transfer_site &&
                               left.target == right.target;
                    }),
        candidate_tail_carriers.end());
    std::vector<ResolvedControlFlowEdge> function_edges;
    function_edges.reserve(resolved_edges.size());
    for (const auto& edge : resolved_edges) {
        if (!edge.analysis_candidate_carrier)
            function_edges.push_back(edge);
    }

    std::vector<std::uint32_t> block_leaders;
    block_leaders.reserve(function_boundaries.size() * 2u +
                          candidate_tail_carriers.size());
    for (const auto& boundary : function_boundaries) {
        block_leaders.push_back(boundary.entry_address);
        if (boundary.size != 0u) {
            const auto end =
                static_cast<std::uint64_t>(boundary.entry_address) +
                boundary.size;
            if (end <= std::numeric_limits<std::uint32_t>::max())
                block_leaders.push_back(static_cast<std::uint32_t>(end));
        }
    }
    for (const auto& carrier : candidate_tail_carriers)
        block_leaders.push_back(carrier.target);
    const auto blocks =
        build_basic_blocks(lines, function_edges, block_leaders);
    block_count = blocks.size();
    report_progress("blocks-complete");
    std::unordered_map<std::uint32_t, const BasicBlock*> block_index;
    block_index.reserve(blocks.size());
    for (const auto& block : blocks)
        block_index.emplace(block.start_address, &block);
    const auto functions = discover_functions_from_blocks(
        blocks, function_boundaries, function_edges);
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        function_owners_by_block;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        function_owners_by_control;
    function_owners_by_block.reserve(blocks.size());
    function_owners_by_control.reserve(blocks.size());
    for (const auto& function : functions) {
        for (const auto block_address : function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end() || block->second->lines.empty())
                continue;
            function_owners_by_block[block_address].push_back(
                function.entry_address);
            function_owners_by_control
                [controlling_line(*block->second).address]
                    .push_back(function.entry_address);
        }
    }
    for (auto& [block, owners] : function_owners_by_block) {
        static_cast<void>(block);
        normalize(owners);
    }
    for (auto& [control, owners] : function_owners_by_control) {
        static_cast<void>(control);
        normalize(owners);
    }
    const auto components = strong_components(functions);
    function_count = functions.size();
    result.strongly_connected_components = components.size();
    report_progress("functions-complete");
    // Candidate-only calls are an inventory input, not a proven member of the
    // semantic call graph.  Feeding them into the summary fixpoint makes an
    // incomplete live-target family recursively refine ordinary function
    // inputs and summaries.  Apart from being unsound for the unknown family
    // members, that can create a non-converging replacement cycle.  Keep the
    // proven call graph for summaries and add the private carriers only to the
    // bounded final inventory pass.
    std::unordered_map<std::uint32_t, IndirectCalleeCandidates>
        summary_indirect_callees;
    summary_indirect_callees.reserve(function_edges.size());
    for (const auto& edge : function_edges) {
        if (edge.kind != ResolvedControlFlowKind::Call) continue;
        auto& candidates =
            summary_indirect_callees[edge.instruction_address];
        candidates.targets.push_back(edge.target_address);
        const auto evidence = resolved_edge_evidence(edge);
        candidates.guarded = candidates.guarded || evidence != ControlFlowEvidence::ProvenComplete;
        candidates.complete = candidates.complete && control_flow_evidence_complete(evidence);
    }
    for (auto& [call_site, candidates] : summary_indirect_callees) {
        static_cast<void>(call_site);
        normalize(candidates.targets);
    }
    auto inventory_indirect_callees = summary_indirect_callees;
    inventory_indirect_callees.reserve(summary_indirect_callees.size() +
                                       candidate_call_carriers.size());
    for (const auto& carrier : candidate_call_carriers) {
        auto& candidates = inventory_indirect_callees[carrier.call_site];
        candidates.targets.push_back(carrier.target);
        candidates.guarded = true;
        candidates.complete = false;
    }
    for (auto& [call_site, candidates] : inventory_indirect_callees) {
        static_cast<void>(call_site);
        normalize(candidates.targets);
    }
    std::unordered_map<std::uint32_t, IndirectCalleeCandidates> indirect_jump_candidates;
    indirect_jump_candidates.reserve(function_edges.size());
    std::unordered_set<std::uint32_t> jump_table_jump_sites;
    for (const auto& edge : function_edges) {
        if (edge.kind != ResolvedControlFlowKind::Jump) continue;
        if (std::find(edge.evidence_origins.begin(),
                      edge.evidence_origins.end(),
                      AnalysisEvidenceOrigin::JumpTable) != edge.evidence_origins.end())
            jump_table_jump_sites.insert(edge.instruction_address);
        auto& candidates = indirect_jump_candidates[edge.instruction_address];
        candidates.targets.push_back(edge.target_address);
        const auto evidence = resolved_edge_evidence(edge);
        candidates.guarded = candidates.guarded || evidence != ControlFlowEvidence::ProvenComplete;
        candidates.complete = candidates.complete && control_flow_evidence_complete(evidence);
    }
    for (auto& [transfer_site, candidates] : indirect_jump_candidates) {
        static_cast<void>(transfer_site);
        normalize(candidates.targets);
    }
    struct InventoryRegionTailIngress {
        std::uint32_t transfer_site = 0u;
        std::uint32_t target = 0u;
        bool guarded = true;
        bool complete = false;
        bool requires_code_pointer = false;
        bool observes_abi_arguments = false;
        std::uint32_t source_region = 0u;
    };
    struct InventoryRegion {
        FunctionInfo function;
    };
    std::vector<InventoryRegion> inventory_regions;
    inventory_regions.reserve(std::min(candidate_tail_carriers.size(),
                                       maximum_inventory_regions));
    std::vector<InventoryRegionTailIngress> inventory_region_tail_ingresses;
    std::deque<std::uint32_t> pending_inventory_regions;
    std::unordered_set<std::uint32_t> queued_inventory_regions;
    GuardedCodeInventoryWalkDiagnostics inventory_walk_diagnostics;
    inventory_walk_diagnostics.inventory_region_budget = maximum_inventory_regions;
    inventory_walk_diagnostics.inventory_region_block_budget =
        maximum_inventory_region_blocks;
    inventory_walk_diagnostics.forwarded_store_context_budget =
        maximum_forwarded_store_contexts;
    inventory_walk_diagnostics.contextual_return_context_budget =
        functions.size();
    inventory_walk_diagnostics.contextual_return_evaluation_budget =
        maximum_contextual_return_evaluations;
    inventory_walk_diagnostics.abi_stack_argument_slot_budget =
        maximum_abi_stack_argument_slots;
    inventory_walk_diagnostics.local_fixpoint_iteration_budget =
        maximum_local_fixpoint_iterations;
    const auto enqueue_inventory_region =
        [&](const std::uint32_t target) {
            if (!block_index.contains(target) ||
                !queued_inventory_regions.insert(target).second)
                return;
            pending_inventory_regions.push_back(target);
        };
    for (const auto& carrier : candidate_tail_carriers)
        enqueue_inventory_region(carrier.target);
    for (const auto& [transfer_site, candidates] : indirect_jump_candidates) {
        if (!candidates.guarded || candidates.complete ||
            candidates.targets.size() != 1u ||
            jump_table_jump_sites.contains(transfer_site))
            continue;
        enqueue_inventory_region(candidates.targets.front());
    }

    const std::vector<std::uint32_t> no_function_owners;
    const auto owners_for_block =
        [&](const std::uint32_t address) -> const std::vector<std::uint32_t>& {
            const auto owners = function_owners_by_block.find(address);
            return owners == function_owners_by_block.end() ? no_function_owners
                                                            : owners->second;
        };
    std::size_t inventory_region_iterations = 0u;
    report_subphase_progress(
        "inventory-region-closure-start",
        "inventory-region-closure",
        queued_inventory_regions.size(),
        0u,
        pending_inventory_regions.size(),
        0u);
    while (!pending_inventory_regions.empty() &&
           inventory_regions.size() < maximum_inventory_regions) {
        const auto target = pending_inventory_regions.front();
        pending_inventory_regions.pop_front();
        ++inventory_region_iterations;
        if (inventory_region_iterations <= 16u ||
            (inventory_region_iterations &
             (inventory_region_iterations - 1u)) == 0u ||
            inventory_region_iterations % 128u == 0u) {
            report_subphase_progress(
                "inventory-region-closure-progress",
                "inventory-region-closure",
                queued_inventory_regions.size(),
                inventory_region_iterations - 1u,
                pending_inventory_regions.size() + 1u,
                inventory_region_iterations);
        }
        const auto& target_owners = owners_for_block(target);
        const auto block_within_owner_domain =
            [&](const std::uint32_t address) {
                const auto& owners = owners_for_block(address);
                if (target_owners.empty())
                    return owners.empty();
                // A target shared by several discovered functions is still a
                // valid inventory ingress.  Stay in the common shared tail
                // while every original owner also owns the successor; a
                // later owner-specific split becomes a separate region.
                return std::includes(owners.begin(),
                                     owners.end(),
                                     target_owners.begin(),
                                     target_owners.end());
            };
        std::deque<std::uint32_t> pending_blocks{target};
        std::unordered_set<std::uint32_t> visited_blocks;
        visited_blocks.reserve(maximum_inventory_region_blocks);
        std::vector<InventoryRegionTailIngress> local_tail_ingresses;
        bool region_budget_exhausted = false;
        while (!pending_blocks.empty()) {
            const auto block_address = pending_blocks.front();
            pending_blocks.pop_front();
            if (visited_blocks.contains(block_address) ||
                !block_within_owner_domain(block_address))
                continue;
            const auto block = block_index.find(block_address);
            if (block == block_index.end() || block->second->lines.empty())
                continue;
            if (visited_blocks.size() >= maximum_inventory_region_blocks) {
                region_budget_exhausted = true;
                break;
            }
            visited_blocks.insert(block_address);
            const auto& control = controlling_line(*block->second);
            const auto flow = control.instruction.control_flow;
            const auto indirect_call =
                inventory_indirect_callees.find(control.address);
            for (const auto successor : block->second->successors) {
                const bool direct_call_target =
                    flow == katana::sh4::ControlFlowKind::Call &&
                    control.target_address.has_value() &&
                    successor == *control.target_address;
                const bool indirect_call_target =
                    flow == katana::sh4::ControlFlowKind::IndirectCall &&
                    indirect_call != inventory_indirect_callees.end() &&
                    std::binary_search(indirect_call->second.targets.begin(),
                                       indirect_call->second.targets.end(),
                                       successor);
                if (direct_call_target || indirect_call_target)
                    continue;
                if (block_within_owner_domain(successor)) {
                    pending_blocks.push_back(successor);
                    continue;
                }
                if (flow == katana::sh4::ControlFlowKind::ConditionalBranch) {
                    // Both conditional successors are real but path-guarded.
                    // Crossing an owner boundary starts a separate ephemeral
                    // inventory region instead of silently truncating the walk.
                    local_tail_ingresses.push_back(
                        {control.address, successor, true, true, false, false});
                    continue;
                }
                if (flow !=
                        katana::sh4::ControlFlowKind::UnconditionalBranch &&
                    flow != katana::sh4::ControlFlowKind::IndirectBranch) {
                    // This is a real non-callee successor: ordinary
                    // fallthrough, a call continuation, or another statically
                    // known control-flow continuation.  An owner-domain split
                    // must not make it disappear from the inventory walk.
                    local_tail_ingresses.push_back(
                        {control.address, successor, false, true, false});
                    continue;
                }
                if (control.target_address.has_value() &&
                    successor == *control.target_address) {
                    local_tail_ingresses.push_back(
                        {control.address, successor, false, true, false, true});
                    continue;
                }
                const auto candidates =
                    indirect_jump_candidates.find(control.address);
                if (candidates == indirect_jump_candidates.end() ||
                    !std::binary_search(candidates->second.targets.begin(),
                                        candidates->second.targets.end(),
                                        successor))
                    continue;
                local_tail_ingresses.push_back(
                    {control.address,
                     successor,
                     candidates->second.guarded,
                     candidates->second.complete,
                     false,
                     true});
            }
        }
        if (region_budget_exhausted) {
            ++inventory_walk_diagnostics.inventory_region_block_limited_regions;
        }
        if (visited_blocks.empty() || region_budget_exhausted)
            continue;
        InventoryRegion region;
        region.function.entry_address = target;
        region.function.evidence = ControlFlowEvidence::GuardedPartial;
        region.function.block_addresses.assign(visited_blocks.begin(),
                                               visited_blocks.end());
        normalize(region.function.block_addresses);
        inventory_regions.push_back(std::move(region));
        for (auto ingress : local_tail_ingresses) {
            ingress.source_region = target;
            inventory_region_tail_ingresses.push_back(ingress);
            enqueue_inventory_region(ingress.target);
        }
    }
    report_subphase_progress(
        "inventory-region-closure-complete",
        "inventory-region-closure",
        queued_inventory_regions.size(),
        inventory_region_iterations,
        pending_inventory_regions.size(),
        inventory_region_iterations);
    inventory_walk_diagnostics.inventory_region_count = inventory_regions.size();
    inventory_walk_diagnostics.pending_inventory_region_count =
        pending_inventory_regions.size();
    std::unordered_map<std::uint32_t, const FunctionInfo*>
        inventory_region_by_address;
    inventory_region_by_address.reserve(inventory_regions.size());
    for (const auto& region : inventory_regions)
        inventory_region_by_address.emplace(region.function.entry_address,
                                            &region.function);

    std::unordered_map<std::uint32_t, IndirectCalleeCandidates> tail_ingresses;
    tail_ingresses.reserve(indirect_jump_candidates.size() +
                           candidate_tail_carriers.size() +
                           inventory_region_tail_ingresses.size());
    const auto add_tail_ingress =
        [&tail_ingresses,
         &inventory_region_by_address](const std::uint32_t transfer_site,
                                       const std::span<const std::uint32_t> targets,
                                       const bool guarded,
                                       const bool complete,
                                       const bool requires_code_pointer = false,
                                       const bool observes_abi_arguments = false) {
        if (targets.empty()) return;
        auto accepted = std::vector<std::uint32_t>{};
        accepted.reserve(targets.size());
        for (const auto target : targets) {
            if (inventory_region_by_address.contains(target))
                accepted.push_back(target);
        }
        if (accepted.empty()) return;
        const auto [stored, inserted] =
            tail_ingresses.try_emplace(transfer_site);
        auto& ingress = stored->second;
        ingress.targets.insert(ingress.targets.end(), accepted.begin(), accepted.end());
        ingress.guarded = ingress.guarded || guarded;
        ingress.complete = ingress.complete && complete;
        ingress.requires_code_pointer =
            inserted ? requires_code_pointer
                     : ingress.requires_code_pointer &&
                           requires_code_pointer;
        ingress.observes_abi_arguments =
            ingress.observes_abi_arguments || observes_abi_arguments;
    };
    for (const auto& carrier : candidate_tail_carriers) {
        const std::array target{carrier.target};
        add_tail_ingress(
            carrier.transfer_site, target, true, false, true, true);
    }
    for (const auto& [transfer_site, candidates] : indirect_jump_candidates) {
        if (!candidates.guarded || candidates.complete ||
            candidates.targets.size() != 1u ||
            jump_table_jump_sites.contains(transfer_site))
            continue;
        const auto region =
            inventory_region_by_address.find(candidates.targets.front());
        if (region == inventory_region_by_address.end() ||
            !function_contains_non_stack_inventory_store_shape(
                *region->second,
                block_index))
            continue;
        // A region with a local PC-relative callback literal still needs an
        // ingress even when no ABI value reaches its store. Only demand and
        // promote ABI code-pointer evidence when the region actually forwards
        // an incoming ABI argument to a persistent store.
        const auto forwards_abi_argument =
            function_forwards_abi_argument_to_persistent_inventory_store(
                image, *region->second, block_index);
        add_tail_ingress(transfer_site,
                         candidates.targets,
                         candidates.guarded,
                         candidates.complete,
                         forwards_abi_argument,
                         forwards_abi_argument);
    }
    for (const auto& function : functions) {
        for (const auto block_address : function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end() || block->second->lines.empty()) continue;
            const auto& control = controlling_line(*block->second);
            const auto flow = control.instruction.control_flow;
            if (flow != katana::sh4::ControlFlowKind::UnconditionalBranch &&
                flow != katana::sh4::ControlFlowKind::IndirectBranch)
                continue;
            if (control.target_address.has_value() &&
                std::binary_search(function.tail_jump_targets.begin(),
                                   function.tail_jump_targets.end(),
                                   *control.target_address)) {
                const std::array target{*control.target_address};
                add_tail_ingress(
                    control.address, target, false, true, false, true);
            }
            const auto candidates = indirect_jump_candidates.find(control.address);
            if (candidates == indirect_jump_candidates.end()) continue;
            std::vector<std::uint32_t> tail_targets;
            tail_targets.reserve(candidates->second.targets.size());
            for (const auto target : candidates->second.targets) {
                if (std::binary_search(function.tail_jump_targets.begin(),
                                       function.tail_jump_targets.end(),
                                       target))
                    tail_targets.push_back(target);
            }
            add_tail_ingress(control.address,
                             tail_targets,
                             candidates->second.guarded,
                             candidates->second.complete,
                             false,
                             true);
        }
    }
    std::unordered_map<std::uint32_t, TailIngressMap>
        inventory_region_tail_ingresses_by_entry;
    inventory_region_tail_ingresses_by_entry.reserve(inventory_regions.size());
    const auto add_region_tail_ingress =
        [&inventory_region_by_address,
         &inventory_region_tail_ingresses_by_entry](
            const InventoryRegionTailIngress& source) {
            if (!inventory_region_by_address.contains(source.source_region) ||
                !inventory_region_by_address.contains(source.target))
                return;
            auto& regional = inventory_region_tail_ingresses_by_entry[source.source_region];
            const auto [stored, inserted] =
                regional.try_emplace(source.transfer_site);
            auto& ingress = stored->second;
            ingress.targets.push_back(source.target);
            ingress.guarded = ingress.guarded || source.guarded;
            ingress.complete =
                inserted ? source.complete : ingress.complete && source.complete;
            ingress.requires_code_pointer =
                inserted ? source.requires_code_pointer
                         : ingress.requires_code_pointer &&
                               source.requires_code_pointer;
            ingress.observes_abi_arguments =
                ingress.observes_abi_arguments || source.observes_abi_arguments;
        };
    for (const auto& ingress : inventory_region_tail_ingresses) {
        add_region_tail_ingress(ingress);
    }
    for (auto& [transfer_site, ingress] : tail_ingresses) {
        static_cast<void>(transfer_site);
        normalize(ingress.targets);
    }
    for (auto& [region_entry, regional] : inventory_region_tail_ingresses_by_entry) {
        static_cast<void>(region_entry);
        for (auto& [transfer_site, ingress] : regional) {
            static_cast<void>(transfer_site);
            normalize(ingress.targets);
        }
    }
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>
        no_tail_ingresses;
    std::map<std::uint32_t, FunctionValueSummary> summaries;
    std::map<std::uint32_t, CandidateInput> candidate_inputs;
    std::unordered_map<std::uint32_t, const FunctionInfo*> function_by_address;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> callers_by_callee;
    function_by_address.reserve(functions.size());
    callers_by_callee.reserve(functions.size());
    for (const auto& function : functions)
        summaries.emplace(function.entry_address, FunctionValueSummary{function.entry_address, {}});
    for (const auto& function : functions)
        candidate_inputs.emplace(function.entry_address, CandidateInput{});
    for (const auto& function : functions) {
        function_by_address.emplace(function.entry_address, &function);
        for (const auto callee : function.direct_callees)
            callers_by_callee[callee].push_back(function.entry_address);
    }
    for (auto& [callee, callers] : callers_by_callee) {
        static_cast<void>(callee);
        normalize(callers);
    }

    // The final inventory graph intentionally keeps only tail regions which
    // can contribute a persistent callback store. ABI live-in contracts need
    // a different graph: a pure epilogue or forwarding tail can consume stack
    // and register inputs even when it has no local inventory store. Reusing
    // the store-filtered graph turns those known tails into false Top.
    std::unordered_map<std::uint32_t, const FunctionInfo*>
        abi_contract_function_by_address = function_by_address;
    abi_contract_function_by_address.reserve(
        function_by_address.size() + inventory_regions.size());
    for (const auto& region : inventory_regions)
        abi_contract_function_by_address.try_emplace(
            region.function.entry_address, &region.function);

    TailIngressMap abi_contract_tail_ingresses;
    abi_contract_tail_ingresses.reserve(
        tail_ingresses.size() + indirect_jump_candidates.size() +
        inventory_region_tail_ingresses.size());
    const auto add_abi_contract_tail_ingress =
        [&](const std::uint32_t transfer_site,
            const std::span<const std::uint32_t> targets,
            const bool guarded,
            const bool complete) {
            if (targets.empty()) return;
            std::vector<std::uint32_t> accepted;
            accepted.reserve(targets.size());
            for (const auto target : targets) {
                if (abi_contract_function_by_address.contains(target))
                    accepted.push_back(target);
            }
            const auto all_targets_accepted =
                accepted.size() == targets.size();
            const auto edge_contract_known =
                all_targets_accepted && (complete || guarded);
            const auto [stored, inserted] =
                abi_contract_tail_ingresses.try_emplace(transfer_site);
            auto& ingress = stored->second;
            const auto existing_contract_known =
                inserted || ingress.complete || ingress.guarded;
            ingress.targets.insert(
                ingress.targets.end(), accepted.begin(), accepted.end());
            const auto merged_contract_known =
                existing_contract_known && edge_contract_known;
            ingress.complete =
                inserted
                    ? all_targets_accepted && complete
                    : ingress.complete &&
                          all_targets_accepted && complete;
            ingress.guarded =
                merged_contract_known && !ingress.complete;
        };
    for (const auto& carrier : candidate_tail_carriers) {
        const std::array target{carrier.target};
        add_abi_contract_tail_ingress(
            carrier.transfer_site, target, true, false);
    }
    for (const auto& [transfer_site, candidates] :
         indirect_jump_candidates) {
        if (!candidates.guarded || candidates.complete ||
            candidates.targets.size() != 1u ||
            jump_table_jump_sites.contains(transfer_site))
            continue;
        add_abi_contract_tail_ingress(
            transfer_site,
            candidates.targets,
            candidates.guarded,
            candidates.complete);
    }
    for (const auto& ingress : inventory_region_tail_ingresses) {
        const std::array target{ingress.target};
        add_abi_contract_tail_ingress(
            ingress.transfer_site,
            target,
            ingress.guarded,
            ingress.complete);
    }
    for (const auto& function : functions) {
        for (const auto block_address : function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end() ||
                block->second->lines.empty())
                continue;
            const auto& control = controlling_line(*block->second);
            const auto flow = control.instruction.control_flow;
            if (flow !=
                    katana::sh4::ControlFlowKind::UnconditionalBranch &&
                flow != katana::sh4::ControlFlowKind::IndirectBranch)
                continue;
            if (control.target_address.has_value() &&
                std::binary_search(
                    function.tail_jump_targets.begin(),
                    function.tail_jump_targets.end(),
                    *control.target_address)) {
                const std::array target{*control.target_address};
                add_abi_contract_tail_ingress(
                    control.address, target, false, true);
            }
            const auto candidates =
                indirect_jump_candidates.find(control.address);
            if (candidates == indirect_jump_candidates.end()) continue;
            std::vector<std::uint32_t> tail_targets;
            for (const auto target : candidates->second.targets) {
                if (std::binary_search(
                        function.tail_jump_targets.begin(),
                        function.tail_jump_targets.end(),
                        target))
                    tail_targets.push_back(target);
            }
            add_abi_contract_tail_ingress(
                control.address,
                tail_targets,
                candidates->second.guarded,
                candidates->second.complete);
        }
    }
    for (auto& [transfer_site, ingress] :
         abi_contract_tail_ingresses) {
        static_cast<void>(transfer_site);
        normalize(ingress.targets);
    }

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        abi_contract_callers_by_callee;
    abi_contract_callers_by_callee.reserve(
        abi_contract_function_by_address.size());
    for (const auto& [caller, function] :
         abi_contract_function_by_address) {
        for (const auto block_address : function->block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end() ||
                block->second->lines.empty())
                continue;
            const auto& control = controlling_line(*block->second);
            const auto call =
                control.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::Call ||
                control.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::IndirectCall;
            if (call && control.target_address.has_value()) {
                abi_contract_callers_by_callee[
                    *control.target_address]
                    .push_back(caller);
            } else if (call) {
                const auto candidates =
                    inventory_indirect_callees.find(control.address);
                if (candidates != inventory_indirect_callees.end()) {
                    for (const auto target :
                         candidates->second.targets)
                        abi_contract_callers_by_callee[target]
                            .push_back(caller);
                }
            }
            const auto tail =
                abi_contract_tail_ingresses.find(control.address);
            if (tail == abi_contract_tail_ingresses.end()) continue;
            for (const auto target : tail->second.targets)
                abi_contract_callers_by_callee[target]
                    .push_back(caller);
        }
    }
    for (auto& [callee, callers] :
         abi_contract_callers_by_callee) {
        static_cast<void>(callee);
        normalize(callers);
    }

    // A direct helper is only treated as forwarding an ABI value when its own
    // bounded signature proves that a formal register or stack argument can
    // reach R0. Unknown and indirect calls remain conservative; this removes
    // the old "every helper returns every argument" fan-out without inventing
    // a fixed guest control-flow edge.
    //
    // The return lattice starts at top and only narrows. A missing RTS or an
    // external/tail exit remains top, so the pruning pass never hides a
    // possible callback route merely because a function's return shape is not
    // statically complete.
    AbiReturnSourceMap abi_return_sources;
    std::unordered_map<std::uint32_t, std::uint8_t>
        abi_persistent_store_sources;
    AbiIndirectDispatchSourceMap abi_indirect_dispatch_sources;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        abi_return_callers_by_callee;
    abi_return_sources.reserve(functions.size());
    abi_persistent_store_sources.reserve(functions.size());
    abi_indirect_dispatch_sources.reserve(functions.size());
    abi_return_callers_by_callee.reserve(functions.size());
    for (const auto& function : functions) {
        abi_return_sources.emplace(function.entry_address, abi_argument_taint_mask);
        abi_persistent_store_sources.emplace(function.entry_address, std::uint8_t{0u});
        abi_indirect_dispatch_sources.emplace(function.entry_address,
                                              std::uint8_t{0u});
        for (const auto block_address : function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end() || block->second->lines.empty())
                continue;
            const auto& control = controlling_line(*block->second);
            if (control.instruction.control_flow !=
                    katana::sh4::ControlFlowKind::Call ||
                !control.target_address.has_value() ||
                !function_by_address.contains(*control.target_address))
                continue;
            abi_return_callers_by_callee[*control.target_address].push_back(
                function.entry_address);
        }
    }
    for (auto& [callee, callers] : abi_return_callers_by_callee) {
        static_cast<void>(callee);
        normalize(callers);
    }
    std::deque<std::uint32_t> pending_abi_signatures;
    std::unordered_set<std::uint32_t> queued_abi_signatures;
    queued_abi_signatures.reserve(functions.size());
    for (const auto& function : functions) {
        pending_abi_signatures.push_back(function.entry_address);
        queued_abi_signatures.insert(function.entry_address);
    }
    std::size_t abi_signature_iterations = 0u;
    report_subphase_progress(
        "abi-return-signatures-start",
        "abi-return-signatures",
        pending_abi_signatures.size(),
        0u,
        pending_abi_signatures.size(),
        0u);
    while (!pending_abi_signatures.empty()) {
        if (abi_signature_iterations >= maximum_fixpoint_iterations) {
            result.budget_exhausted = true;
            break;
        }
        ++abi_signature_iterations;
        const auto address = pending_abi_signatures.front();
        pending_abi_signatures.pop_front();
        if (abi_signature_iterations <= 16u ||
            (abi_signature_iterations &
             (abi_signature_iterations - 1u)) == 0u ||
            abi_signature_iterations % 128u == 0u) {
            report_subphase_progress(
                "abi-return-signatures-progress",
                "abi-return-signatures",
                abi_signature_iterations +
                    pending_abi_signatures.size(),
                abi_signature_iterations - 1u,
                pending_abi_signatures.size() + 1u,
                abi_signature_iterations);
        }
        queued_abi_signatures.erase(address);
        const auto function = function_by_address.find(address);
        if (function == function_by_address.end()) continue;
        const auto signature = analyze_abi_persistent_store_signature(
            image, *function->second, block_index, &abi_return_sources);
        auto& returned_sources = abi_return_sources.at(address);
        const auto narrowed_returned_sources = static_cast<std::uint8_t>(
            returned_sources & signature.returned_r0_sources);
        if (narrowed_returned_sources == returned_sources) continue;
        returned_sources = narrowed_returned_sources;
        const auto callers = abi_return_callers_by_callee.find(address);
        if (callers == abi_return_callers_by_callee.end()) continue;
        for (const auto caller : callers->second) {
            if (queued_abi_signatures.insert(caller).second)
                pending_abi_signatures.push_back(caller);
        }
    }
    report_subphase_progress(
        "abi-return-signatures-complete",
        "abi-return-signatures",
        abi_signature_iterations + pending_abi_signatures.size(),
        abi_signature_iterations,
        pending_abi_signatures.size(),
        abi_signature_iterations);

    // Stack arguments need an exact identity, not the legacy aggregate stack
    // taint bit. This positive interprocedural fixed point records only slots
    // which can be read before a definite overwrite, including slots consumed
    // by direct/guarded calls and known tail targets. Unknown callees or stack
    // coordinates yield Top and retain the old fail-closed full projection.
    AbiStackArgumentReadMap abi_stack_argument_reads;
    abi_stack_argument_reads.reserve(
        abi_contract_function_by_address.size());
    for (const auto& [address, function] :
         abi_contract_function_by_address) {
        static_cast<void>(function);
        abi_stack_argument_reads.emplace(
            address,
            AbiStackArgumentReadSet{});
    }
    if (!result.budget_exhausted) {
        std::deque<std::uint32_t> pending_abi_stack_reads;
        std::unordered_set<std::uint32_t> queued_abi_stack_reads;
        queued_abi_stack_reads.reserve(
            abi_contract_function_by_address.size());
        for (const auto& [address, function] :
             abi_contract_function_by_address) {
            static_cast<void>(function);
            pending_abi_stack_reads.push_back(address);
            queued_abi_stack_reads.insert(address);
        }
        std::size_t abi_stack_read_iterations = 0u;
        report_subphase_progress(
            "abi-stack-reads-start",
            "abi-stack-reads",
            pending_abi_stack_reads.size(),
            0u,
            pending_abi_stack_reads.size(),
            0u);
        while (!pending_abi_stack_reads.empty()) {
            if (abi_stack_read_iterations >= maximum_fixpoint_iterations) {
                result.budget_exhausted = true;
                break;
            }
            ++abi_stack_read_iterations;
            const auto address = pending_abi_stack_reads.front();
            pending_abi_stack_reads.pop_front();
            if (abi_stack_read_iterations <= 16u ||
                (abi_stack_read_iterations &
                 (abi_stack_read_iterations - 1u)) == 0u ||
                abi_stack_read_iterations % 128u == 0u) {
                report_subphase_progress(
                    "abi-stack-reads-progress",
                    "abi-stack-reads",
                    abi_stack_read_iterations +
                        pending_abi_stack_reads.size(),
                    abi_stack_read_iterations - 1u,
                    pending_abi_stack_reads.size() + 1u,
                    abi_stack_read_iterations);
            }
            queued_abi_stack_reads.erase(address);
            const auto function =
                abi_contract_function_by_address.find(address);
            if (function ==
                abi_contract_function_by_address.end())
                continue;
            const auto signature = analyze_abi_persistent_store_signature(
                image,
                *function->second,
                block_index,
                &abi_return_sources,
                nullptr,
                nullptr,
                &inventory_indirect_callees,
                &abi_stack_argument_reads,
                &abi_contract_tail_ingresses);
            auto& previous = abi_stack_argument_reads.at(address);
            const auto previous_was_complete = previous.complete;
            const auto source_is_complete =
                signature.stack_slots_read_before_definition.complete;
            const auto changed = merge_abi_stack_argument_reads(
                previous,
                signature.stack_slots_read_before_definition);
            if (previous_was_complete &&
                !previous.complete &&
                source_is_complete &&
                previous.top_chain.empty()) {
                AbiStackReadTopFrame frame;
                frame.reason =
                    AbiStackReadTopReason::FixpointSlotBudget;
                frame.owner = address;
                frame.site = address;
                record_abi_stack_read_top(previous, frame);
            }
            if (!changed)
                continue;
            const auto callers =
                abi_contract_callers_by_callee.find(address);
            if (callers ==
                abi_contract_callers_by_callee.end())
                continue;
            for (const auto caller : callers->second) {
                if (queued_abi_stack_reads.insert(caller).second)
                    pending_abi_stack_reads.push_back(caller);
            }
        }
        report_subphase_progress(
            "abi-stack-reads-complete",
            "abi-stack-reads",
            abi_stack_read_iterations + pending_abi_stack_reads.size(),
            abi_stack_read_iterations,
            pending_abi_stack_reads.size(),
            abi_stack_read_iterations);
    }

    // The forwarding key also needs the complete general-register live-in
    // contract. Unlike an ABI-only r4-r7 mask this preserves callee-saved
    // registers used by tail regions while removing values definitely
    // overwritten before their first read. Calls and known tails are composed
    // through the same positive fixed point as the stack read contract.
    ForwardedRegisterReadMap forwarded_register_reads;
    forwarded_register_reads.reserve(
        abi_contract_function_by_address.size());
    for (const auto& [address, function] :
         abi_contract_function_by_address) {
        static_cast<void>(function);
        forwarded_register_reads.emplace(
            address, std::uint16_t{0u});
    }
    if (!result.budget_exhausted) {
        std::deque<std::uint32_t> pending_forwarded_register_reads;
        std::unordered_set<std::uint32_t>
            queued_forwarded_register_reads;
        queued_forwarded_register_reads.reserve(
            abi_contract_function_by_address.size());
        for (const auto& [address, function] :
             abi_contract_function_by_address) {
            static_cast<void>(function);
            pending_forwarded_register_reads.push_back(
                address);
            queued_forwarded_register_reads.insert(
                address);
        }
        std::size_t forwarded_register_read_iterations = 0u;
        report_subphase_progress(
            "abi-register-reads-start",
            "abi-register-reads",
            pending_forwarded_register_reads.size(),
            0u,
            pending_forwarded_register_reads.size(),
            0u);
        while (!pending_forwarded_register_reads.empty()) {
            if (forwarded_register_read_iterations >=
                maximum_fixpoint_iterations) {
                result.budget_exhausted = true;
                break;
            }
            ++forwarded_register_read_iterations;
            const auto address =
                pending_forwarded_register_reads.front();
            pending_forwarded_register_reads.pop_front();
            if (forwarded_register_read_iterations <= 16u ||
                (forwarded_register_read_iterations &
                 (forwarded_register_read_iterations - 1u)) == 0u ||
                forwarded_register_read_iterations % 128u == 0u) {
                report_subphase_progress(
                    "abi-register-reads-progress",
                    "abi-register-reads",
                    forwarded_register_read_iterations +
                        pending_forwarded_register_reads.size(),
                    forwarded_register_read_iterations - 1u,
                    pending_forwarded_register_reads.size() + 1u,
                    forwarded_register_read_iterations);
            }
            queued_forwarded_register_reads.erase(address);
            const auto function =
                abi_contract_function_by_address.find(address);
            if (function ==
                abi_contract_function_by_address.end())
                continue;
            const auto observed =
                entry_register_read_before_def_mask(
                    *function->second,
                    block_index,
                    &forwarded_register_reads,
                    &inventory_indirect_callees,
                    &abi_contract_tail_ingresses);
            auto& previous = forwarded_register_reads.at(address);
            const auto expanded = static_cast<std::uint16_t>(
                previous | observed);
            if (expanded == previous) continue;
            previous = expanded;
            const auto callers =
                abi_contract_callers_by_callee.find(address);
            if (callers ==
                abi_contract_callers_by_callee.end())
                continue;
            for (const auto caller : callers->second) {
                if (queued_forwarded_register_reads
                        .insert(caller)
                        .second)
                    pending_forwarded_register_reads.push_back(caller);
            }
        }
        report_subphase_progress(
            "abi-register-reads-complete",
            "abi-register-reads",
            forwarded_register_read_iterations +
                pending_forwarded_register_reads.size(),
            forwarded_register_read_iterations,
            pending_forwarded_register_reads.size(),
            forwarded_register_read_iterations);
    }

    // Keep direct local sites separate from the broader forwarding mask.  The
    // later fast harvest may use only these sites: helper, guarded-indirect and
    // tail propagation remains on the existing full-context path until it has
    // its own exact contract.
    AbiPersistentStoreSiteMap direct_local_persistent_store_sites;
    direct_local_persistent_store_sites.reserve(functions.size());
    for (const auto& function : functions) {
        auto signature = analyze_abi_persistent_store_signature(
            image, function, block_index, &abi_return_sources);
        if (signature.local_persistent_store_sites.empty()) continue;
        direct_local_persistent_store_sites.emplace(
            function.entry_address,
            std::move(signature.local_persistent_store_sites));
    }

    // The store slice uses the same inventory-only indirect-call candidates as
    // the final guarded-AOT pass, but keeps them out of ordinary summaries and
    // the semantic call graph. A target narrowing therefore requeues every
    // direct or guarded inventory caller without inventing a fixed CFG edge.
    auto abi_store_callers_by_callee = callers_by_callee;
    for (const auto& [call_site, candidates] : inventory_indirect_callees) {
        const auto owners = function_owners_by_control.find(call_site);
        if (owners == function_owners_by_control.end()) continue;
        for (const auto target : candidates.targets) {
            auto& callers = abi_store_callers_by_callee[target];
            callers.insert(callers.end(), owners->second.begin(), owners->second.end());
        }
    }
    for (auto& [callee, callers] : abi_store_callers_by_callee) {
        static_cast<void>(callee);
        normalize(callers);
    }

    // Persistent-store source masks form a separate positive fixed point over
    // direct and inventory-only guarded calls. A callee mask is projected
    // through the caller's current ABI flow state, so a wrapper contributes
    // only the original formal arguments which can reach a persistent callback
    // store. Tail transfers stay outside this slice and remain conservative.
    if (!result.budget_exhausted) {
        std::deque<std::uint32_t> pending_abi_store_signatures;
        std::unordered_set<std::uint32_t> queued_abi_store_signatures;
        queued_abi_store_signatures.reserve(functions.size());
        for (const auto& function : functions) {
            pending_abi_store_signatures.push_back(function.entry_address);
            queued_abi_store_signatures.insert(function.entry_address);
        }
        std::size_t abi_store_signature_iterations = 0u;
        report_subphase_progress(
            "persistent-store-signatures-start",
            "persistent-store-signatures",
            pending_abi_store_signatures.size(),
            0u,
            pending_abi_store_signatures.size(),
            0u);
        while (!pending_abi_store_signatures.empty()) {
            if (abi_store_signature_iterations >= maximum_fixpoint_iterations) {
                result.budget_exhausted = true;
                break;
            }
            ++abi_store_signature_iterations;
            const auto address = pending_abi_store_signatures.front();
            pending_abi_store_signatures.pop_front();
            if (abi_store_signature_iterations <= 16u ||
                (abi_store_signature_iterations &
                 (abi_store_signature_iterations - 1u)) == 0u ||
                abi_store_signature_iterations % 128u == 0u) {
                report_subphase_progress(
                    "persistent-store-signatures-progress",
                    "persistent-store-signatures",
                    abi_store_signature_iterations +
                        pending_abi_store_signatures.size(),
                    abi_store_signature_iterations - 1u,
                    pending_abi_store_signatures.size() + 1u,
                    abi_store_signature_iterations);
            }
            queued_abi_store_signatures.erase(address);
            const auto function = function_by_address.find(address);
            if (function == function_by_address.end()) continue;
            const auto signature = analyze_abi_persistent_store_signature(
                image,
                *function->second,
                block_index,
                &abi_return_sources,
                &abi_persistent_store_sources,
                &abi_indirect_dispatch_sources,
                &inventory_indirect_callees);
            auto& persistent_sources = abi_persistent_store_sources.at(address);
            const auto expanded_persistent_sources = static_cast<std::uint8_t>(
                persistent_sources | signature.persistent_store_sources);
            auto& dispatch_sources =
                abi_indirect_dispatch_sources.at(address);
            const auto expanded_dispatch_sources = static_cast<std::uint8_t>(
                dispatch_sources | signature.indirect_dispatch_sources);
            if (expanded_persistent_sources == persistent_sources &&
                expanded_dispatch_sources == dispatch_sources)
                continue;
            persistent_sources = expanded_persistent_sources;
            dispatch_sources = expanded_dispatch_sources;
            const auto callers = abi_store_callers_by_callee.find(address);
            if (callers == abi_store_callers_by_callee.end()) continue;
            for (const auto caller : callers->second) {
                if (queued_abi_store_signatures.insert(caller).second)
                    pending_abi_store_signatures.push_back(caller);
            }
        }
        report_subphase_progress(
            "persistent-store-signatures-complete",
            "persistent-store-signatures",
            abi_store_signature_iterations +
                pending_abi_store_signatures.size(),
            abi_store_signature_iterations,
            pending_abi_store_signatures.size(),
            abi_store_signature_iterations);
    }
    if (abi_contract_observer) {
        for (const auto& function : functions) {
            const auto reads =
                abi_stack_argument_reads.find(
                    function.entry_address);
            if (reads == abi_stack_argument_reads.end())
                continue;
            abi_contract_observer(
                {function.entry_address,
                 reads->second.complete,
                 reads->second.slots,
                 abi_persistent_store_sources.at(
                     function.entry_address)});
        }
    }

    // Tail ingress itself is a control-flow contract, not proof that every
    // caller needs an isolated callback inventory harvest. Restrict that
    // expensive path to a guarded carrier or a target which can actually move
    // an incoming ABI value into a persistent store. This is intentionally
    // evaluated after the return-signature fixpoint so ordinary helper tails
    // cannot reintroduce the conservative pre-fixpoint fan-out.
    std::unordered_set<std::uint32_t>
        functions_with_guarded_abi_inventory_tail;
    functions_with_guarded_abi_inventory_tail.reserve(functions.size());
    std::unordered_set<std::uint32_t> candidate_tail_carrier_sites;
    candidate_tail_carrier_sites.reserve(candidate_tail_carriers.size());
    for (const auto& carrier : candidate_tail_carriers)
        candidate_tail_carrier_sites.insert(carrier.transfer_site);
    std::unordered_map<std::uint32_t, std::uint8_t>
        tail_target_abi_sink_sources;
    tail_target_abi_sink_sources.reserve(inventory_region_by_address.size());
    const auto cache_tail_target_abi_sink_sources =
        [&](const std::uint32_t target) {
            if (const auto cached = tail_target_abi_sink_sources.find(target);
                cached != tail_target_abi_sink_sources.end())
                return cached->second;
            auto sources = std::uint8_t{0u};
            if (const auto function = abi_persistent_store_sources.find(target);
                function != abi_persistent_store_sources.end())
                sources = function->second;
            if (const auto function =
                    abi_indirect_dispatch_sources.find(target);
                function != abi_indirect_dispatch_sources.end())
                sources = static_cast<std::uint8_t>(
                    sources | function->second);
            if (const auto region = inventory_region_by_address.find(target);
                region != inventory_region_by_address.end()) {
                const auto signature = analyze_abi_persistent_store_signature(
                    image,
                    *region->second,
                    block_index,
                    &abi_return_sources,
                    &abi_persistent_store_sources,
                    &abi_indirect_dispatch_sources,
                    &inventory_indirect_callees);
                sources = static_cast<std::uint8_t>(
                    sources | signature.persistent_store_sources |
                    signature.indirect_dispatch_sources);
            }
            tail_target_abi_sink_sources.emplace(target, sources);
            return sources;
        };
    if (!result.budget_exhausted) {
        for (const auto& [transfer_site, ingress] : tail_ingresses) {
            for (const auto target : ingress.targets)
                static_cast<void>(
                    cache_tail_target_abi_sink_sources(target));
            const auto target_forwards_abi_to_inventory_sink =
                std::any_of(
                    ingress.targets.begin(),
                    ingress.targets.end(),
                    [&](const auto target) {
                        return tail_target_abi_sink_sources.at(target) != 0u;
                    });
            auto requires_isolated_harvest =
                candidate_tail_carrier_sites.contains(transfer_site) ||
                target_forwards_abi_to_inventory_sink;
            if (!requires_isolated_harvest) continue;
            const auto owners = function_owners_by_control.find(transfer_site);
            if (owners == function_owners_by_control.end()) continue;
            functions_with_guarded_abi_inventory_tail.insert(
                owners->second.begin(), owners->second.end());
        }
    }
    const auto target_abi_inventory_sink_sources =
        [&](const std::uint32_t target) {
            auto sources = std::uint8_t{0u};
            if (const auto function =
                    abi_persistent_store_sources.find(target);
                function != abi_persistent_store_sources.end())
                sources = function->second;
            if (const auto function =
                    abi_indirect_dispatch_sources.find(target);
                function != abi_indirect_dispatch_sources.end())
                sources = static_cast<std::uint8_t>(
                    sources | function->second);
            if (const auto region =
                    tail_target_abi_sink_sources.find(target);
                region != tail_target_abi_sink_sources.end())
                sources = static_cast<std::uint8_t>(
                    sources | region->second);
            return sources;
        };
    const auto unresolved_stack_callback_loss_reaches_inventory_sink =
        [&](const AbstractState& state, const std::uint32_t target) {
            const auto sources =
                target_abi_inventory_sink_sources(target);
            for (std::uint8_t index = 0u; index < 4u; ++index) {
                if ((sources &
                     static_cast<std::uint8_t>(1u << index)) != 0u &&
                    carries_unresolved_stack_callback(
                        state[4u + index]))
                    return true;
            }
            if ((sources & abi_stack_argument_taint) == 0u)
                return false;
            if (state.inventory_unresolved_stack_callback_loss)
                return true;
            return std::any_of(
                state.stack_values.begin(),
                state.stack_values.end(),
                [](const auto& stored) {
                    return carries_unresolved_stack_callback(
                        stored.second);
                });
        };

    // Candidate call carriers are private inventory transport, not semantic
    // call-graph edges.  They still have to participate in the inventory-only
    // backwards reachability walk or a wrapper which merely forwards a
    // code-pointer to a guarded tail registrar is never evaluated.
    auto inventory_callers_by_callee = callers_by_callee;
    for (const auto& carrier : candidate_call_carriers) {
        const auto owners =
            function_owners_by_control.find(carrier.call_site);
        if (owners == function_owners_by_control.end()) continue;
        auto& callers = inventory_callers_by_callee[carrier.target];
        callers.insert(callers.end(),
                       owners->second.begin(),
                       owners->second.end());
    }
    for (auto& [callee, callers] : inventory_callers_by_callee) {
        static_cast<void>(callee);
        normalize(callers);
    }
    std::unordered_set<std::uint32_t> functions_reaching_guarded_inventory_sink;
    functions_reaching_guarded_inventory_sink.reserve(functions.size());
    std::deque<std::uint32_t> pending_inventory_reachability;
    const auto add_inventory_sink = [&](const std::uint32_t address) {
        if (functions_reaching_guarded_inventory_sink.insert(address).second)
            pending_inventory_reachability.push_back(address);
    };
    for (const auto& function : functions) {
        // Only functions that can carry an incoming ABI value into a persistent
        // callback store or an indirect dispatch are roots for the expensive
        // isolated forwarding walk. This keeps unrelated object/data stores
        // out of the guarded AOT inventory slice.
        if (abi_persistent_store_sources.at(function.entry_address) == 0u &&
            abi_indirect_dispatch_sources.at(function.entry_address) == 0u)
            continue;
        add_inventory_sink(function.entry_address);
    }
    for (const auto function : functions_with_guarded_abi_inventory_tail)
        add_inventory_sink(function);
    std::size_t inventory_reachability_iterations = 0u;
    report_subphase_progress(
        "inventory-reachability-start",
        "inventory-reachability",
        functions_reaching_guarded_inventory_sink.size(),
        0u,
        pending_inventory_reachability.size(),
        0u);
    while (!pending_inventory_reachability.empty()) {
        const auto callee = pending_inventory_reachability.front();
        pending_inventory_reachability.pop_front();
        ++inventory_reachability_iterations;
        if (inventory_reachability_iterations <= 16u ||
            (inventory_reachability_iterations &
             (inventory_reachability_iterations - 1u)) == 0u ||
            inventory_reachability_iterations % 128u == 0u) {
            report_subphase_progress(
                "inventory-reachability-progress",
                "inventory-reachability",
                functions_reaching_guarded_inventory_sink.size(),
                inventory_reachability_iterations - 1u,
                pending_inventory_reachability.size() + 1u,
                inventory_reachability_iterations);
        }
        const auto callers = inventory_callers_by_callee.find(callee);
        if (callers == inventory_callers_by_callee.end()) continue;
        for (const auto caller : callers->second) {
            add_inventory_sink(caller);
        }
    }
    report_subphase_progress(
        "inventory-reachability-complete",
        "inventory-reachability",
        functions_reaching_guarded_inventory_sink.size(),
        inventory_reachability_iterations,
        pending_inventory_reachability.size(),
        inventory_reachability_iterations);
    // `forwarded_register_reads` is the interprocedural target contract used
    // below for ordinary calls, guarded tail owners and root-isolated walks.
    // Missing targets deliberately retain the full input.
    for (const auto& line : lines) {
        if (line.instruction.control_flow != katana::sh4::ControlFlowKind::Call ||
            !line.target_address.has_value())
            continue;
        if (const auto input = candidate_inputs.find(*line.target_address);
            input != candidate_inputs.end())
            input->second.expected_call_sites.insert(line.address);
    }
    for (const auto& edge : function_edges) {
        if (edge.kind != ResolvedControlFlowKind::Call) continue;
        const auto input = candidate_inputs.find(edge.target_address);
        if (input == candidate_inputs.end()) continue;
        input->second.expected_call_sites.insert(edge.instruction_address);
    }
    for (auto& [address, input] : candidate_inputs) {
        input.state.stack_offsets[15u] = 0;
        if (input.expected_call_sites.empty() ||
            std::find(image.entry_points().begin(), image.entry_points().end(), address) !=
                image.entry_points().end())
            input.unknown_ingress = true;
    }
    std::unordered_map<std::uint32_t, std::size_t>
        fixpoint_function_index;
    fixpoint_function_index.reserve(functions.size());
    std::vector<const FunctionInfo*> fixpoint_functions;
    fixpoint_functions.reserve(functions.size());
    const auto key_plan_work = functions.size() * 2u;
    std::size_t key_plan_processed = 0u;
    report_subphase_progress(
        "cache-key-plan-start",
        "cache-key-plan",
        key_plan_work,
        0u,
        key_plan_work,
        0u);
    for (const auto& function : functions) {
        fixpoint_function_index.emplace(
            function.entry_address, fixpoint_functions.size());
        fixpoint_functions.push_back(&function);
        ++key_plan_processed;
        if (key_plan_processed <= 16u ||
            key_plan_processed % 128u == 0u ||
            key_plan_processed == key_plan_work) {
            report_subphase_progress(
                "cache-key-plan-progress",
                "cache-key-plan",
                key_plan_work,
                key_plan_processed,
                key_plan_work - key_plan_processed,
                key_plan_processed);
        }
    }
    std::vector<std::vector<std::size_t>>
        fixpoint_summary_dependencies(functions.size());
    for (std::size_t index = 0u; index < functions.size(); ++index) {
        const auto& function = functions[index];
        auto dependencies = function.direct_callees;
        // Keep this dependency contract aligned with evaluate_function rather
        // than assuming FunctionInfo will forever be the only call source.
        // Version validation is allowed to over-approximate, never to miss a
        // summary which a speculative evaluation can read.
        for (const auto block_address : function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end()) continue;
            for (const auto& line : block->second->lines) {
                const bool call =
                    line.instruction.control_flow ==
                        katana::sh4::ControlFlowKind::Call ||
                    line.instruction.control_flow ==
                        katana::sh4::ControlFlowKind::IndirectCall;
                if (!call) continue;
                if (line.target_address.has_value()) {
                    dependencies.push_back(*line.target_address);
                    continue;
                }
                const auto candidates =
                    summary_indirect_callees.find(line.address);
                if (candidates == summary_indirect_callees.end()) continue;
                dependencies.insert(
                    dependencies.end(),
                    candidates->second.targets.begin(),
                    candidates->second.targets.end());
            }
        }
        normalize(dependencies);
        auto& dependency_indices =
            fixpoint_summary_dependencies[index];
        dependency_indices.reserve(dependencies.size());
        for (const auto dependency : dependencies) {
            const auto found =
                fixpoint_function_index.find(dependency);
            if (found != fixpoint_function_index.end())
                dependency_indices.push_back(found->second);
        }
        ++key_plan_processed;
        if (key_plan_processed <= 16u ||
            key_plan_processed % 128u == 0u ||
            key_plan_processed == key_plan_work) {
            report_subphase_progress(
                "cache-key-plan-progress",
                "cache-key-plan",
                key_plan_work,
                key_plan_processed,
                key_plan_work - key_plan_processed,
                key_plan_processed);
        }
    }
    report_subphase_progress(
        "cache-key-plan-complete",
        "cache-key-plan",
        key_plan_work,
        key_plan_processed,
        0u,
        key_plan_processed);
    std::vector<std::uint64_t> fixpoint_summary_versions(
        functions.size(), 0u);
    std::vector<std::uint64_t> fixpoint_input_versions(
        functions.size(), 0u);
    struct GlobalFixpointBatchItem {
        std::uint32_t address = 0u;
        std::size_t function_index = 0u;
        AbstractState input;
        std::uint64_t input_version = 0u;
        std::vector<std::pair<std::size_t, std::uint64_t>>
            summary_versions;
        std::optional<FunctionEvaluation> evaluation;
        GuardedCodeInventoryWalkDiagnostics diagnostics;
        std::exception_ptr error;
    };
    const auto merge_fixpoint_diagnostics =
        [](GuardedCodeInventoryWalkDiagnostics& destination,
           const GuardedCodeInventoryWalkDiagnostics& source) {
            destination.maximum_local_fixpoint_iterations =
                std::max(
                    destination.maximum_local_fixpoint_iterations,
                    source.maximum_local_fixpoint_iterations);
            destination.local_fixpoint_limited_evaluations +=
                source.local_fixpoint_limited_evaluations;
            destination
                .abi_stack_argument_projection_truncated_functions =
                std::max(
                    destination
                        .abi_stack_argument_projection_truncated_functions,
                    source
                        .abi_stack_argument_projection_truncated_functions);
            destination.inventory_candidate_values_truncated =
                destination.inventory_candidate_values_truncated ||
                source.inventory_candidate_values_truncated;
            destination.abi_stack_base_unresolved =
                destination.abi_stack_base_unresolved ||
                source.abi_stack_base_unresolved;
        };
    struct PhysicalEvaluationScope {
        EvaluationActivityScope activity;

        PhysicalEvaluationScope(
            EvaluationActivityTelemetry* const telemetry,
            std::atomic_size_t& physical_evaluations)
            : activity(
                  telemetry,
                  EvaluationActivityKind::PhysicalInterpreter) {
            physical_evaluations.fetch_add(
                1u, std::memory_order_relaxed);
        }
    };
    const auto cached_evaluate_function =
        [&](const FunctionInfo& function,
            const std::unordered_map<
                std::uint32_t,
                IndirectCalleeCandidates>& indirect_callees,
            const TailIngressMap& evaluation_tail_ingresses,
            const std::map<
                std::uint32_t,
                FunctionValueSummary>& evaluation_summaries,
            const AbstractState& initial_state,
            const ResolutionCollectionMode resolution_mode,
            const bool may_merge_stack_inventory,
            GuardedCodeInventoryCollector* const
                guarded_inventory_collector,
            const std::set<std::uint32_t>* const
                isolated_inventory_call_sites,
            const std::map<
                std::uint32_t,
                FunctionValueSummary>* const contextual_summaries,
            const TailIngressMap* const local_tail_ingresses,
            GuardedCodeInventoryWalkDiagnostics* const
                walk_diagnostics,
            const AbiStackArgumentReadMap* const
                evaluation_abi_stack_argument_reads,
            const std::uint8_t inventory_sink_sources,
            const bool replay_outputs = true,
            const bool account_request = true) {
            const EvaluationActivityScope request_activity{
                account_request
                    ? evaluation_activity_if_observed
                    : nullptr,
                EvaluationActivityKind::Request};
            if (account_request)
                logical_evaluations.fetch_add(
                    1u, std::memory_order_relaxed);
            const auto requested_lens = select_evaluation_lens(
                resolution_mode,
                guarded_inventory_collector != nullptr,
                isolated_inventory_call_sites,
                contextual_summaries);
            const auto projection = make_function_evaluation_projection(
                initial_state,
                requested_lens,
                function.entry_address,
                forwarded_register_reads,
                abi_stack_argument_reads,
                !result.budget_exhausted);
            // Detailed stack diagnostics include deliberately repeated
            // per-evaluation trace side effects. Do not let memoization erase
            // those observations; this mode is already forced to one worker
            // and is not the product performance path.
            if (analyzer_stack_diagnostics_enabled()) {
                cache_diagnostic_bypass_evaluations.fetch_add(
                    1u, std::memory_order_relaxed);
                const PhysicalEvaluationScope physical{
                    evaluation_activity_if_observed,
                    physical_evaluations};
                auto artifact =
                    std::make_shared<CachedFunctionEvaluation>();
                artifact->walk_diagnostics
                    .local_fixpoint_iteration_budget =
                    maximum_local_fixpoint_iterations;
                auto* const inventory_target =
                    guarded_inventory_collector == nullptr
                        ? nullptr
                        : replay_outputs
                              ? guarded_inventory_collector
                              : &artifact->inventory;
                artifact->evaluation = evaluate_function(
                    image,
                    function,
                    block_index,
                    indirect_callees,
                    evaluation_tail_ingresses,
                    evaluation_summaries,
                    initial_state,
                    resolution_mode,
                    may_merge_stack_inventory,
                    inventory_target,
                    isolated_inventory_call_sites,
                    contextual_summaries,
                    local_tail_ingresses,
                    walk_diagnostics != nullptr
                        ? &artifact->walk_diagnostics
                        : nullptr,
                    evaluation_abi_stack_argument_reads,
                    inventory_sink_sources);
                canonicalize_evaluation_outputs(
                    artifact->evaluation,
                    forwarded_register_reads,
                    abi_stack_argument_reads);
                if (replay_outputs &&
                    walk_diagnostics != nullptr) {
                    merge_fixpoint_diagnostics(
                        *walk_diagnostics,
                        artifact->walk_diagnostics);
                }
                return std::pair{
                    std::shared_ptr<
                        const CachedFunctionEvaluation>{
                        std::move(artifact)},
                    false};
            }
            FunctionEvaluationCacheKey key;
            {
                const EvaluationActivityScope key_activity{
                    evaluation_activity_if_observed,
                    EvaluationActivityKind::KeyBuild};
                key = make_function_evaluation_cache_key(
                    image,
                    function,
                    block_index,
                    indirect_callees,
                    evaluation_tail_ingresses,
                    evaluation_summaries,
                    projection,
                    resolution_mode,
                    may_merge_stack_inventory,
                    guarded_inventory_collector != nullptr,
                    isolated_inventory_call_sites,
                    contextual_summaries,
                    local_tail_ingresses,
                    walk_diagnostics != nullptr,
                    evaluation_abi_stack_argument_reads,
                    inventory_sink_sources,
                    session.impl_->evaluations
                        .detailed_telemetry_enabled());
            }
            auto cached =
                session.impl_->evaluations.get_or_compute(
                    std::move(key),
                    projection.requested_lens,
                    projection.full_state_fallback,
                    [&]() {
                        const PhysicalEvaluationScope physical{
                            evaluation_activity_if_observed,
                            physical_evaluations};
                        auto artifact =
                            std::make_shared<
                                CachedFunctionEvaluation>();
                        artifact->walk_diagnostics
                            .local_fixpoint_iteration_budget =
                            maximum_local_fixpoint_iterations;
                        if (guarded_inventory_collector != nullptr) {
                            artifact->inventory
                                .begin_exact_replay_capture(
                                    *guarded_inventory_collector);
                        }
                        try {
                            artifact->evaluation =
                                evaluate_function(
                                    image,
                                    function,
                                    block_index,
                                    indirect_callees,
                                    evaluation_tail_ingresses,
                                    evaluation_summaries,
                                    projection.ingress,
                                    resolution_mode,
                                    may_merge_stack_inventory,
                                    guarded_inventory_collector !=
                                            nullptr
                                        ? &artifact->inventory
                                        : nullptr,
                                    isolated_inventory_call_sites,
                                    contextual_summaries,
                                    local_tail_ingresses,
                                    walk_diagnostics != nullptr
                                        ? &artifact
                                               ->walk_diagnostics
                                        : nullptr,
                                    evaluation_abi_stack_argument_reads,
                                    inventory_sink_sources);
                            canonicalize_evaluation_outputs(
                                artifact->evaluation,
                                forwarded_register_reads,
                                abi_stack_argument_reads);
                            if (projection.effective_lens !=
                                EvaluationLens::FullState)
                                session.impl_->evaluations
                                    .record_reconstructed_result();
                        } catch (...) {
                            artifact->inventory
                                .finish_exact_replay_capture();
                            throw;
                        }
                        artifact->inventory
                            .finish_exact_replay_capture();
                        artifact->inventory
                            .discard_transient_caches();
                        return std::shared_ptr<
                            const CachedFunctionEvaluation>{
                            std::move(artifact)};
                    },
                    evaluation_activity_if_observed);
            const auto& artifact = *cached.first;
            if (cached.second && replay_outputs &&
                guarded_inventory_collector != nullptr &&
                !artifact.inventory
                     .exact_replay_available()) {
                cache_replay_fallback_recomputes.fetch_add(
                    1u, std::memory_order_relaxed);
                auto fallback =
                    std::make_shared<CachedFunctionEvaluation>();
                fallback->walk_diagnostics
                    .local_fixpoint_iteration_budget =
                    maximum_local_fixpoint_iterations;
                const PhysicalEvaluationScope physical{
                    evaluation_activity_if_observed,
                    physical_evaluations};
                fallback->evaluation = evaluate_function(
                    image,
                    function,
                    block_index,
                    indirect_callees,
                    evaluation_tail_ingresses,
                    evaluation_summaries,
                    projection.ingress,
                    resolution_mode,
                    may_merge_stack_inventory,
                    guarded_inventory_collector,
                    isolated_inventory_call_sites,
                    contextual_summaries,
                    local_tail_ingresses,
                    walk_diagnostics != nullptr
                        ? &fallback->walk_diagnostics
                        : nullptr,
                    evaluation_abi_stack_argument_reads,
                    inventory_sink_sources);
                canonicalize_evaluation_outputs(
                    fallback->evaluation,
                    forwarded_register_reads,
                    abi_stack_argument_reads);
                if (projection.effective_lens !=
                    EvaluationLens::FullState)
                    session.impl_->evaluations
                        .record_reconstructed_result();
                if (walk_diagnostics != nullptr) {
                    merge_fixpoint_diagnostics(
                        *walk_diagnostics,
                        fallback->walk_diagnostics);
                }
                return std::pair{
                    std::shared_ptr<
                        const CachedFunctionEvaluation>{
                        std::move(fallback)},
                    true};
            }
            if (cached.second && replay_outputs &&
                guarded_inventory_collector != nullptr) {
                const EvaluationActivityScope replay_activity{
                    evaluation_activity_if_observed,
                    EvaluationActivityKind::ExactReplay};
                artifact.inventory.replay_deferred_copy_into(
                    *guarded_inventory_collector);
            }
            if (replay_outputs && walk_diagnostics != nullptr) {
                merge_fixpoint_diagnostics(
                    *walk_diagnostics,
                    artifact.walk_diagnostics);
            }
            return cached;
        };
    auto& fixpoint_executor = global_analysis_executor();
    const auto parallel_fixpoint_jobs =
        analyzer_stack_diagnostics_enabled()
            ? std::size_t{1u}
            : fixpoint_executor.maximum_jobs();
    result.fixpoint_worker_count = parallel_fixpoint_jobs;
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> queued;
    queued.reserve(functions.size());
    for (const auto& component : components) {
        for (const auto address : component) {
            pending.push_back(address);
            queued.insert(address);
        }
    }
    pending_count = pending.size();
    report_progress("fixpoint-start");
    while (!pending.empty() && !result.budget_exhausted) {
        if (result.fixpoint_iterations >= maximum_fixpoint_iterations) {
            result.budget_exhausted = true;
            break;
        }
        const auto remaining_budget =
            maximum_fixpoint_iterations -
            result.fixpoint_iterations;
        const auto batch_size =
            std::min(
                {pending.size(),
                 parallel_fixpoint_jobs,
                 remaining_budget});
        std::vector<GlobalFixpointBatchItem> batch(batch_size);
        auto pending_address = pending.begin();
        for (std::size_t index = 0u;
             index < batch.size();
             ++index, ++pending_address) {
            auto& item = batch[index];
            item.address = *pending_address;
            item.function_index =
                fixpoint_function_index.at(item.address);
            item.input =
                candidate_inputs.at(item.address).state;
            item.input_version =
                fixpoint_input_versions[item.function_index];
            item.summary_versions.reserve(
                fixpoint_summary_dependencies[item.function_index]
                    .size());
            for (const auto dependency :
                 fixpoint_summary_dependencies[item.function_index]) {
                item.summary_versions.emplace_back(
                    dependency,
                    fixpoint_summary_versions[dependency]);
            }
            item.diagnostics.local_fixpoint_iteration_budget =
                maximum_local_fixpoint_iterations;
        }
        pending_count = pending.size();
        if (batch.size() > 1u) {
            ++result.fixpoint_parallel_batches;
            report_progress("fixpoint-batch-start");
        }
        result.fixpoint_speculative_evaluations += batch.size();
        result.maximum_fixpoint_batch_size =
            std::max(
                result.maximum_fixpoint_batch_size,
                batch.size());
        const auto evaluate_batch_item =
            [&](GlobalFixpointBatchItem& item) noexcept {
                try {
                    item.evaluation.emplace(
                        cached_evaluate_function(
                            *fixpoint_functions[item.function_index],
                            summary_indirect_callees,
                            no_tail_ingresses,
                            summaries,
                            item.input,
                            ResolutionCollectionMode::None,
                            false,
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr,
                            &item.diagnostics,
                            &abi_stack_argument_reads,
                            target_abi_inventory_sink_sources(
                                item.address))
                            .first->evaluation);
                } catch (...) {
                    item.error = std::current_exception();
                }
            };
        if (batch.size() == 1u) {
            evaluate_batch_item(batch.front());
        } else {
            parallel_analysis_for(
                fixpoint_executor,
                batch.size(),
                parallel_fixpoint_jobs,
                function_value_parallel_activity_if_observed,
                [&](const std::size_t index) {
                    evaluate_batch_item(batch[index]);
                });
        }
        for (auto& item : batch) {
            bool stale =
                fixpoint_input_versions[item.function_index] !=
                item.input_version;
            for (const auto& [dependency, version] :
                 item.summary_versions) {
                stale =
                    stale ||
                    fixpoint_summary_versions[dependency] !=
                        version;
            }
            if (stale) {
                ++result.fixpoint_stale_repairs;
                item.input =
                    candidate_inputs.at(item.address).state;
                item.evaluation.reset();
                item.diagnostics = {};
                item.diagnostics.local_fixpoint_iteration_budget =
                    maximum_local_fixpoint_iterations;
                item.error = {};
                evaluate_batch_item(item);
            }
            if (pending.empty() ||
                pending.front() != item.address)
                throw std::logic_error(
                    "Paralleler Function-Value-Fixpunkt verlor die FIFO-Reihenfolge.");
            pending.pop_front();
            queued.erase(item.address);
            ++result.fixpoint_iterations;
            pending_count = pending.size();
            const bool sampled_iteration =
                result.fixpoint_iterations <= 16u ||
                (result.fixpoint_iterations &
                 (result.fixpoint_iterations - 1u)) == 0u ||
                result.fixpoint_iterations % 128u == 0u;
            if (sampled_iteration)
                report_progress("fixpoint-evaluate-start");
            emit_analyzer_fixpoint_trace(
                "global-start",
                result.fixpoint_iterations,
                item.address,
                item.address,
                pending.size());
            merge_fixpoint_diagnostics(
                inventory_walk_diagnostics,
                item.diagnostics);
            if (item.error)
                std::rethrow_exception(item.error);
            auto evaluation =
                std::move(*item.evaluation);
            if (evaluation.local_fixpoint_budget_exhausted) {
                result.budget_exhausted = true;
                break;
            }
            emit_analyzer_fixpoint_trace(
                "global-complete",
                result.fixpoint_iterations,
                item.address,
                item.address,
                pending.size());
            if (sampled_iteration)
                report_progress("fixpoint-evaluate-complete");
            auto& previous = summaries[item.address];
            if (previous != evaluation.summary) {
                previous = std::move(evaluation.summary);
                ++fixpoint_summary_versions[item.function_index];
                const auto callers =
                    callers_by_callee.find(item.address);
                if (callers != callers_by_callee.end()) {
                    for (const auto caller : callers->second) {
                        if (queued.insert(caller).second)
                            pending.push_back(caller);
                    }
                }
            }
            for (const auto& observation :
                 evaluation.call_arguments) {
                if (unresolved_stack_callback_loss_reaches_inventory_sink(
                        observation.state,
                        observation.callee)) {
                    inventory_walk_diagnostics
                        .abi_stack_base_unresolved = true;
                    emit_analyzer_stack_diagnostic(
                        "fixpoint-call",
                        item.address,
                        observation.call_site,
                        observation.callee);
                }
                const auto input =
                    candidate_inputs.find(observation.callee);
                if (input == candidate_inputs.end()) continue;
                if (merge_candidate_input(
                        input->second,
                        observation,
                        &inventory_walk_diagnostics)) {
                    const auto callee_index =
                        fixpoint_function_index.find(
                            observation.callee);
                    if (callee_index !=
                        fixpoint_function_index.end())
                        ++fixpoint_input_versions
                              [callee_index->second];
                    if (queued.insert(observation.callee).second)
                        pending.push_back(observation.callee);
                } else {
                    ++result.unchanged_ingress_skips;
                }
            }
            pending_count = pending.size();
        }
        if (result.budget_exhausted) break;
        if (batch.size() > 1u)
            report_progress("fixpoint-batch-complete");
    }
    // A summary is committed only once the global fixpoint has converged.
    // Counting speculative or later-invalidated evaluations as completed
    // functions would make this progress domain lie during workset growth.
    if (!result.budget_exhausted)
        summarized_functions = summaries.size();
    report_progress("fixpoint-complete");

    if (result.budget_exhausted) {
        for (auto& [address, summary] : summaries) {
            static_cast<void>(address);
            summary.memory_complete = false;
            summary.memory_values.clear();
            for (auto& value : summary.registers) {
                value.complete = false;
                value.guarded = true;
                value.values.clear();
                value.reason = "analysis-budget-exhausted";
            }
        }
        for (auto& [address, input] : candidate_inputs) {
            static_cast<void>(address);
            input.state = {};
            input.state.stack_offsets[15u] = 0;
        }
    }

    for (const auto& [address, summary] : summaries)
        result.summaries.push_back(summary);
    report_progress("resolution-start");
    GuardedCodeInventoryCollector guarded_inventory_collector{
        false, &guarded_native_entry_shapes};
    if (result.budget_exhausted) {
        // A failed global fixpoint has already invalidated every summary and
        // candidate input above. Re-running every function in the resolution
        // phase cannot recover proof, can repeat the same 65,536-step local
        // failure, and all of its output would be discarded. Preserve the
        // precise diagnostics and return immediately.
        result.guarded_code_inventory =
            guarded_inventory_collector.finish();
        result.guarded_code_inventory.walk_diagnostics =
            inventory_walk_diagnostics;
        stop_progress_pulse();
        report_progress("resolution-skipped-budget-exhausted");
        result.progress_callback_failed =
            progress_callback_failed.load(
                std::memory_order_relaxed);
        return result;
    }
    struct ForwardedStoreContext {
        const FunctionInfo* function = nullptr;
        std::uint32_t target = 0u;
        bool tail = false;
        bool isolated = false;
        AbstractState input;
        std::set<std::uint32_t> root_call_sites;
        bool evaluated = false;
        bool evaluation_dirty = true;
        std::size_t evaluation_count = 0u;
        std::uint64_t version = 0u;
        std::vector<InterproceduralTargetResolution> resolutions;
    };
    struct ResolutionFunctionResult {
        FunctionEvaluation evaluation;
        GuardedCodeInventoryCollector inventory{true};
        GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
        std::vector<ForwardedStoreContext> forwarded_store_contexts;
        std::deque<std::size_t> pending_forwarded_store_contexts;
        std::vector<bool> forwarded_store_context_queued;
        std::unordered_map<std::uint64_t, std::vector<std::size_t>>
            forwarded_store_context_indices;
        // No output from this resolution function may be published after any
        // of its base, contextual, isolated, or forwarded CFG walks hit the
        // local cap. The diagnostic alone blocks product export, but semantic
        // resolutions are consumed earlier by the outer decode fixpoint.
        bool local_fixpoint_budget_exhausted = false;
    };
    const auto& final_candidate_inputs = std::as_const(candidate_inputs);
    const auto& final_function_by_address = std::as_const(function_by_address);
    const auto& final_direct_local_persistent_store_sites =
        std::as_const(direct_local_persistent_store_sites);
    const auto& final_inventory_region_by_address =
        std::as_const(inventory_region_by_address);
    const auto& final_inventory_region_tail_ingresses_by_entry =
        std::as_const(inventory_region_tail_ingresses_by_entry);
    const auto candidate_call_pair_key =
        [](const std::uint32_t call_site, const std::uint32_t callee) {
            return (static_cast<std::uint64_t>(call_site) << 32u) |
                   callee;
        };
    std::unordered_set<std::uint64_t> semantic_call_pairs;
    semantic_call_pairs.reserve(function_edges.size() + blocks.size());
    for (const auto& edge : function_edges) {
        if (edge.kind != ResolvedControlFlowKind::Call) continue;
        semantic_call_pairs.insert(
            candidate_call_pair_key(edge.instruction_address,
                                    edge.target_address));
    }
    for (const auto& block : blocks) {
        if (block.lines.empty()) continue;
        const auto& control = controlling_line(block);
        if (control.instruction.control_flow !=
                katana::sh4::ControlFlowKind::Call ||
            !control.target_address.has_value())
            continue;
        semantic_call_pairs.insert(
            candidate_call_pair_key(control.address,
                                    *control.target_address));
    }
    std::vector<std::vector<std::size_t>>
        contextual_summary_dependencies(functions.size());
    for (std::size_t index = 0u;
         index < fixpoint_functions.size();
         ++index) {
        const auto& function = *fixpoint_functions[index];
        auto dependencies = function.direct_callees;
        for (const auto block_address :
             function.block_addresses) {
            const auto block = block_index.find(block_address);
            if (block == block_index.end()) continue;
            for (const auto& line : block->second->lines) {
                const bool call =
                    line.instruction.control_flow ==
                        katana::sh4::ControlFlowKind::Call ||
                    line.instruction.control_flow ==
                        katana::sh4::ControlFlowKind::
                            IndirectCall;
                if (!call) continue;
                if (line.target_address.has_value()) {
                    dependencies.push_back(
                        *line.target_address);
                    continue;
                }
                const auto candidates =
                    inventory_indirect_callees.find(
                        line.address);
                if (candidates ==
                    inventory_indirect_callees.end())
                    continue;
                dependencies.insert(
                    dependencies.end(),
                    candidates->second.targets.begin(),
                    candidates->second.targets.end());
            }
        }
        normalize(dependencies);
        auto& dependency_indices =
            contextual_summary_dependencies[index];
        dependency_indices.reserve(dependencies.size());
        for (const auto dependency : dependencies) {
            const auto found =
                fixpoint_function_index.find(dependency);
            if (found != fixpoint_function_index.end())
                dependency_indices.push_back(found->second);
        }
    }
    const auto requires_contextual_return =
        [&summaries](const std::uint32_t address) {
            const auto global_summary = summaries.find(address);
            const auto* global_return =
                global_summary == summaries.end()
                    ? nullptr
                    : register_summary(global_summary->second, 0u);
            return global_return == nullptr || !global_return->complete ||
                   global_return->values.empty();
        };
    std::unordered_set<std::uint64_t> candidate_call_pairs;
    std::unordered_set<std::uint32_t> candidate_call_owner_functions;
    candidate_call_pairs.reserve(candidate_call_carriers.size());
    for (const auto& carrier : candidate_call_carriers) {
        const auto global_summary = summaries.find(carrier.target);
        const auto* global_return =
            global_summary == summaries.end()
                ? nullptr
                : register_summary(global_summary->second, 0u);
        if (global_return != nullptr && global_return->complete &&
            !global_return->values.empty())
            continue;
        candidate_call_pairs.insert(
            candidate_call_pair_key(carrier.call_site, carrier.target));
        const auto owners =
            function_owners_by_control.find(carrier.call_site);
        if (owners == function_owners_by_control.end()) continue;
        candidate_call_owner_functions.insert(owners->second.begin(),
                                              owners->second.end());
    }
    std::vector<std::uint32_t> contextual_candidate_return_owners{
        candidate_call_owner_functions.begin(),
        candidate_call_owner_functions.end()};
    std::sort(contextual_candidate_return_owners.begin(),
              contextual_candidate_return_owners.end());
    const auto global_contextual_return_coordinator =
        contextual_candidate_return_owners.empty()
            ? std::optional<std::uint32_t>{}
            : std::optional<std::uint32_t>{
                  contextual_candidate_return_owners.front()};
    const auto contains_resolution_or_inventory_instruction =
        [&](const FunctionInfo& function) {
            for (const auto block_address :
                 function.block_addresses) {
                const auto block = block_index.find(block_address);
                if (block == block_index.end()) continue;
                for (const auto& line : block->second->lines) {
                    const auto kind = line.instruction.kind;
                    if (kind == katana::sh4::InstructionKind::Jmp ||
                        kind == katana::sh4::InstructionKind::Jsr ||
                        kind == katana::sh4::InstructionKind::Braf ||
                        kind == katana::sh4::InstructionKind::Bsrf ||
                        kind == katana::sh4::InstructionKind::MovLongStore ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongStorePreDecrement ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongStoreDisplacement ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongStoreR0Indexed ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongStoreGbrDisplacement ||
                        kind == katana::sh4::InstructionKind::MovLongLoad ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongLoadPostIncrement ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongLoadDisplacement ||
                        kind == katana::sh4::InstructionKind::
                                    MovLongLoadR0Indexed)
                        return true;
                }
            }
            return false;
        };
    std::vector<const FunctionInfo*> resolution_functions;
    resolution_functions.reserve(functions.size());
    for (const auto& candidate : functions) {
        const auto address = candidate.entry_address;
        if (!contains_resolution_or_inventory_instruction(candidate) &&
            !functions_reaching_guarded_inventory_sink.contains(
                address) &&
            (!global_contextual_return_coordinator.has_value() ||
             address != *global_contextual_return_coordinator))
            continue;
        resolution_functions.push_back(&candidate);
    }
    std::sort(resolution_functions.begin(),
              resolution_functions.end(),
              [](const auto* left, const auto* right) {
                  return left->entry_address < right->entry_address;
              });
    resolution_functions_total =
        resolution_functions.size();
    report_progress("resolution-dispatch-start");
    struct ForwardedContextEvaluation final {
        std::shared_ptr<const CachedFunctionEvaluation> artifact;
        // A producer miss forwards every inventory observation live even if
        // the bounded exact-replay stream overflows. Preserve that completed
        // collector instead of running the abstract interpreter twice.
        std::optional<GuardedCodeInventoryCollector> live_inventory;
        bool cache_hit = false;
    };
    const auto evaluate_forwarded_context =
        [&](const ForwardedStoreContext& context,
            const std::set<std::uint32_t>& root_call_sites,
            const TailIngressMap* const local_tail_ingresses) {
            const EvaluationActivityScope request_activity{
                evaluation_activity_if_observed,
                EvaluationActivityKind::Request};
            logical_evaluations.fetch_add(
                1u, std::memory_order_relaxed);
            GuardedCodeInventoryCollector replay_target{true};
            GuardedCodeInventoryWalkDiagnostics diagnostics;
            auto cached = cached_evaluate_function(
                *context.function,
                inventory_indirect_callees,
                tail_ingresses,
                summaries,
                context.input,
                ResolutionCollectionMode::GuardedInventory,
                true,
                &replay_target,
                context.isolated ? &root_call_sites : nullptr,
                nullptr,
                local_tail_ingresses,
                &diagnostics,
                &abi_stack_argument_reads,
                target_abi_inventory_sink_sources(
                    context.target),
                false,
                false);
            if (cached.first->inventory
                    .exact_replay_available()) {
                return ForwardedContextEvaluation{
                    std::move(cached.first),
                    std::nullopt,
                    cached.second};
            }

            if (!cached.second) {
                // The miss producer already forwarded the full successful
                // evaluation into replay_target. Replay overflow discarded
                // only the cacheable event stream, never this live result.
                return ForwardedContextEvaluation{
                    std::move(cached.first),
                    std::move(replay_target),
                    false};
            }

            // A non-retainable artifact can only be observed by a waiter
            // which coalesced while the producer was still in flight. Its
            // own target stayed empty, so this one consumer must recompute.
            cache_replay_fallback_recomputes.fetch_add(
                1u, std::memory_order_relaxed);
            auto fallback =
                std::make_shared<CachedFunctionEvaluation>();
            fallback->walk_diagnostics
                .local_fixpoint_iteration_budget =
                maximum_local_fixpoint_iterations;
            const PhysicalEvaluationScope physical{
                evaluation_activity_if_observed,
                physical_evaluations};
            const auto fallback_projection =
                make_function_evaluation_projection(
                    context.input,
                    context.isolated
                        ? EvaluationLens::IsolatedObservation
                        : EvaluationLens::GuardedInventory,
                    context.function->entry_address,
                    forwarded_register_reads,
                    abi_stack_argument_reads,
                    !result.budget_exhausted);
            fallback->evaluation = evaluate_function(
                image,
                *context.function,
                block_index,
                inventory_indirect_callees,
                tail_ingresses,
                summaries,
                fallback_projection.ingress,
                ResolutionCollectionMode::GuardedInventory,
                true,
                &fallback->inventory,
                context.isolated ? &root_call_sites : nullptr,
                nullptr,
                local_tail_ingresses,
                &fallback->walk_diagnostics,
                &abi_stack_argument_reads,
                target_abi_inventory_sink_sources(
                    context.target));
            canonicalize_evaluation_outputs(
                fallback->evaluation,
                forwarded_register_reads,
                abi_stack_argument_reads);
            if (fallback_projection.effective_lens !=
                EvaluationLens::FullState)
                session.impl_->evaluations
                    .record_reconstructed_result();
            return ForwardedContextEvaluation{
                std::shared_ptr<
                    const CachedFunctionEvaluation>{
                    std::move(fallback)},
                std::nullopt,
                true};
        };
    const auto evaluate_resolution_function = [&](const std::size_t function_index) {
        progress_resolution_functions_started.fetch_add(
            1u, std::memory_order_relaxed);
        ResolutionFunctionResult function_result;
        std::size_t next_root_resolution_compaction =
            std::size_t{1'024u};
        const auto compact_root_resolutions =
            [&](const bool force = false) {
                if (!force &&
                    function_result.evaluation.resolutions.size() <
                        next_root_resolution_compaction)
                    return;
                coalesce_resolutions(
                    function_result.evaluation.resolutions);
                next_root_resolution_compaction =
                    std::max(
                        std::size_t{1'024u},
                        function_result.evaluation
                                .resolutions.size() *
                            2u);
            };
        function_result.walk_diagnostics.forwarded_store_context_budget =
            maximum_forwarded_store_contexts;
        function_result.walk_diagnostics.contextual_return_context_budget =
            final_function_by_address.size();
        function_result.walk_diagnostics.contextual_return_evaluation_budget =
            maximum_contextual_return_evaluations;
        function_result.walk_diagnostics.abi_stack_argument_slot_budget =
            maximum_abi_stack_argument_slots;
        function_result.walk_diagnostics.local_fixpoint_iteration_budget =
            maximum_local_fixpoint_iterations;
        const auto record_local_fixpoint_limit = [&] {
            function_result.local_fixpoint_budget_exhausted = true;
        };
        const auto* function = resolution_functions[function_index];
        const auto& input = final_candidate_inputs.at(function->entry_address);
        const auto finalize_root_result = [&] {
            // Every exit, including fail-closed cap exits, must release the
            // large AbstractState transport graph before this root crosses
            // the ordered parallel-result barrier.
            function_result.evaluation.summary = {};
            function_result.evaluation.call_arguments = {};
            function_result.evaluation.inventory_transfers = {};
            function_result.forwarded_store_contexts = {};
            function_result.pending_forwarded_store_contexts = {};
            function_result.forwarded_store_context_queued = {};
            function_result.forwarded_store_context_indices = {};
            if (function_result.local_fixpoint_budget_exhausted) {
                function_result.evaluation.resolutions = {};
                function_result.inventory =
                    GuardedCodeInventoryCollector{true};
            } else {
                compact_root_resolutions(true);
            }
            if (function_result.walk_diagnostics
                    .abi_stack_argument_projection_truncated_functions != 0u)
                emit_abi_stack_projection_root_diagnostic(
                    function->entry_address);
            return std::move(function_result);
        };
        const auto record_forwarded_store_limit =
            [&](const ForwardedStoreContextLimitReason reason,
                const std::uint32_t target,
                const bool tail,
                const bool isolated,
                const std::set<std::uint32_t>& root_call_sites,
                const std::size_t context_count,
                const std::size_t root_call_site_count,
                const std::size_t evaluation_count,
                const AbstractState* const shape_existing,
                const AbstractState* const shape_incoming) {
                auto& diagnostics = function_result.walk_diagnostics
                                        .forwarded_store_context_limit_diagnostics;
                if (!diagnostics.empty()) return;
                ForwardedStoreContextLimitDiagnostic diagnostic;
                diagnostic.owner_entry = function->entry_address;
                diagnostic.target = target;
                diagnostic.exemplar_root_call_site = root_call_sites.empty()
                                                        ? 0u
                                                        : *root_call_sites.begin();
                diagnostic.context_count = context_count;
                diagnostic.root_call_site_count = root_call_site_count;
                diagnostic.evaluation_count = evaluation_count;
                diagnostic.tail = tail;
                diagnostic.isolated = isolated;
                diagnostic.reason = reason;
                diagnostics.push_back(diagnostic);
                const auto same_target_context_count =
                    static_cast<std::size_t>(std::count_if(
                        function_result.forwarded_store_contexts.begin(),
                        function_result.forwarded_store_contexts.end(),
                        [&](const auto& context) {
                            return context.target == target &&
                                   context.tail == tail &&
                                   context.isolated == isolated;
                        }));
                auto live_mask =
                    std::numeric_limits<std::uint16_t>::max();
                auto live_mask_known = false;
                if (const auto reads =
                        forwarded_register_reads.find(target);
                    reads != forwarded_register_reads.end()) {
                    live_mask = reads->second;
                    live_mask_known = true;
                }
                const AbiStackArgumentReadSet* required_stack_reads =
                    nullptr;
                if (const auto reads =
                        abi_stack_argument_reads.find(target);
                    reads != abi_stack_argument_reads.end())
                    required_stack_reads = &reads->second;
                emit_forwarded_cap_diagnostic(
                    reason,
                    function->entry_address,
                    target,
                    diagnostic.exemplar_root_call_site,
                    tail,
                    isolated,
                    context_count,
                    same_target_context_count,
                    root_call_site_count,
                    evaluation_count,
                    live_mask_known,
                    live_mask,
                    required_stack_reads,
                    shape_existing,
                    shape_incoming);
            };
        function_result.evaluation =
            cached_evaluate_function(
                *function,
                inventory_indirect_callees,
                tail_ingresses,
                summaries,
                input.state,
                ResolutionCollectionMode::Semantic,
                false,
                &function_result.inventory,
                nullptr,
                nullptr,
                nullptr,
                &function_result.walk_diagnostics,
                &abi_stack_argument_reads,
                target_abi_inventory_sink_sources(
                    function->entry_address))
                .first->evaluation;
        if (function_result.evaluation.local_fixpoint_budget_exhausted) {
            record_local_fixpoint_limit();
            return finalize_root_result();
        }
        const auto enqueue_forwarded_context =
            [&](const FunctionInfo* target_function,
                const std::uint32_t target,
                const bool tail,
                const bool isolated,
                AbstractState forwarded_input,
                const std::set<std::uint32_t>& root_call_sites) {
                // Non-isolated contexts carry no root correlation and use one
                // monotone abstract join bucket per target/transfer kind.
                // Isolated variants may also join when they belong to exactly
                // the same non-empty root partition. Different roots retain
                // the exact-shape path below, so their evidence is never
                // cross-correlated by widening.
                const auto context_key =
                    (static_cast<std::uint64_t>(target) << 2u) |
                    (tail ? 2u : 0u) |
                    (isolated ? 1u : 0u);
                auto& context_indices =
                    function_result
                        .forwarded_store_context_indices[
                        context_key];
                std::optional<std::size_t> existing_index;
                for (const auto index : context_indices) {
                    const auto& context =
                        function_result.forwarded_store_contexts[
                            index];
                    if (!isolated ||
                        (!root_call_sites.empty() &&
                         context.root_call_sites ==
                             root_call_sites)) {
                        existing_index = index;
                        break;
                    }
                }
                const auto widen_partition =
                    existing_index.has_value();
                if (!existing_index.has_value()) {
                    for (const auto index : context_indices) {
                        const auto& context =
                            function_result
                                .forwarded_store_contexts[index];
                        if (same_forwarded_store_shape(
                                context.input,
                                forwarded_input)) {
                            existing_index = index;
                            break;
                        }
                    }
                }
                if (existing_index.has_value()) {
                    const auto index = *existing_index;
                    auto& context = function_result.forwarded_store_contexts[index];
                    auto additional_roots = std::size_t{0u};
                    if (isolated) {
                        for (const auto root : root_call_sites) {
                            if (!context.root_call_sites.contains(root))
                                ++additional_roots;
                        }
                        if (context.root_call_sites.size() + additional_roots >
                            maximum_forwarded_store_context_root_call_sites) {
                            function_result.walk_diagnostics
                                .forwarded_store_context_limited_functions = 1u;
                            record_forwarded_store_limit(
                                ForwardedStoreContextLimitReason::RootCallSites,
                                context.target,
                                context.tail,
                                context.isolated,
                                root_call_sites,
                                function_result.forwarded_store_contexts.size(),
                                context.root_call_sites.size() + additional_roots,
                                context.evaluation_count,
                                &context.input,
                                &forwarded_input);
                            return;
                        }
                    }
                    const auto provenance_changed =
                        widen_partition
                            ? merge_state(
                                  context.input,
                                  forwarded_input,
                                  true)
                            : merge_forwarded_inventory_payload(
                                  context.input, forwarded_input);
                    bool roots_changed = false;
                    if (isolated) {
                        const auto root_count = context.root_call_sites.size();
                        context.root_call_sites.insert(root_call_sites.begin(),
                                                       root_call_sites.end());
                        roots_changed = context.root_call_sites.size() != root_count;
                    }
                    if (provenance_changed || roots_changed)
                        ++context.version;
                    if (provenance_changed || roots_changed)
                        context.evaluation_dirty = true;
                    if (context.evaluation_dirty &&
                        !function_result.forwarded_store_context_queued[index]) {
                        function_result.pending_forwarded_store_contexts.push_back(index);
                        function_result.forwarded_store_context_queued[index] = true;
                    }
                    return;
                }
                if (function_result.forwarded_store_contexts.size() >=
                    maximum_forwarded_store_contexts) {
                    const auto* const exemplar =
                        context_indices.empty()
                            ? nullptr
                            : &function_result
                                   .forwarded_store_contexts[
                                   context_indices.front()];
                    function_result.walk_diagnostics
                        .forwarded_store_context_limited_functions = 1u;
                    record_forwarded_store_limit(
                        ForwardedStoreContextLimitReason::ContextCount,
                        target,
                        tail,
                        isolated,
                        root_call_sites,
                        function_result.forwarded_store_contexts.size(),
                        root_call_sites.size(),
                        0u,
                        exemplar == nullptr
                            ? nullptr
                            : &exemplar->input,
                        &forwarded_input);
                    return;
                }
                if (isolated && root_call_sites.size() >
                                    maximum_forwarded_store_context_root_call_sites) {
                    function_result.walk_diagnostics
                        .forwarded_store_context_limited_functions = 1u;
                    record_forwarded_store_limit(
                        ForwardedStoreContextLimitReason::RootCallSites,
                        target,
                        tail,
                        isolated,
                        root_call_sites,
                        function_result.forwarded_store_contexts.size(),
                        root_call_sites.size(),
                        0u,
                        nullptr,
                        &forwarded_input);
                    return;
                }
                ForwardedStoreContext context;
                context.function = target_function;
                context.target = target;
                context.tail = tail;
                context.isolated = isolated;
                context.input = std::move(forwarded_input);
                if (isolated)
                    context.root_call_sites = root_call_sites;
                function_result.forwarded_store_contexts.push_back(std::move(context));
                context_indices.push_back(
                    function_result.forwarded_store_contexts.size() -
                    1u);
                function_result.pending_forwarded_store_contexts.push_back(
                    function_result.forwarded_store_contexts.size() - 1u);
                function_result.forwarded_store_context_queued.push_back(true);
            };
        // A direct call into a function with an exact local ABI store site can
        // inventory the already proven code-pointer argument at that site
        // without materializing a whole callee context.  This deliberately
        // excludes stack sources, tables, guarded indirect calls, helper
        // propagation and tails; those retain the full conservative walk.
        enum class DirectLocalPersistentStoreHarvest {
            NotApplicable,
            Handled,
            NeedsExact,
        };
        const auto harvest_direct_local_persistent_store =
            [&](const FunctionEvaluation::CallArguments& forwarded)
                -> DirectLocalPersistentStoreHarvest {
                if (!semantic_call_pairs.contains(
                        candidate_call_pair_key(forwarded.call_site,
                                                forwarded.callee)))
                    return DirectLocalPersistentStoreHarvest::NotApplicable;
                const auto sites = final_direct_local_persistent_store_sites.find(
                    forwarded.callee);
                if (sites == final_direct_local_persistent_store_sites.end() ||
                    sites->second.empty())
                    return DirectLocalPersistentStoreHarvest::NotApplicable;
                // The signature is context-free and therefore cannot know that
                // an incoming object-pointer register aliases this caller's
                // stack. At a concrete callsite, refuse the shortcut whenever
                // a syntactic destination may alias the caller stack; the
                // exact evaluator will then honor path-sensitive stack/object
                // joins and intervening stack clobbers.
                for (const auto& site : sites->second) {
                    for (std::uint8_t index = 0u;
                         index < forwarded.state.size();
                         ++index) {
                        if ((site.destination_registers & register_bit(index)) != 0u &&
                            forwarded.state.inventory_stack_may_alias[index])
                            return DirectLocalPersistentStoreHarvest::NeedsExact;
                    }
                }
                auto source_mask = std::uint8_t{0u};
                for (const auto& site : sites->second)
                    source_mask = static_cast<std::uint8_t>(
                        source_mask | site.value_sources);
                // The context-free signature has no individual stack-slot
                // identity. A stack-fed local store therefore always needs
                // the exact callsite evaluation, even when no register
                // argument happens to carry a code pointer.
                if ((source_mask & abi_stack_argument_taint) != 0u)
                    return DirectLocalPersistentStoreHarvest::NeedsExact;
                bool has_relevant_code_pointer = false;
                for (std::uint8_t index = 4u; index <= 7u; ++index) {
                    if ((source_mask & abi_entry_register_bit(index)) == 0u)
                        continue;
                    if (!forwarded.state[index].inventory_code_pointer_values.empty()) {
                        has_relevant_code_pointer = true;
                        break;
                    }
                }
                // A code pointer in an argument that cannot reach any local
                // persistent store does not require an isolated store walk.
                if (!has_relevant_code_pointer)
                    return DirectLocalPersistentStoreHarvest::Handled;

                std::vector<StoredCodeAddressCandidate> candidates;
                for (const auto& site : sites->second) {
                    // The summary carries no individual stack-slot identity;
                    // keep every such site on the old exact-context path.
                    if ((site.value_sources & abi_stack_argument_taint) != 0u)
                        return DirectLocalPersistentStoreHarvest::NeedsExact;
                    for (std::uint8_t index = 4u; index <= 7u; ++index) {
                        if ((site.value_sources & abi_entry_register_bit(index)) == 0u)
                            continue;
                        const auto& value = forwarded.state[index];
                        for (const auto candidate : value.inventory_code_pointer_values) {
                            const auto validation = validate_decode_candidate(image, candidate);
                            if (!validation.valid())
                                return DirectLocalPersistentStoreHarvest::NeedsExact;
                            // A concrete code address can also be a pointer-table
                            // base. Preserve the existing table observer for that
                            // case rather than treating arbitrary code bytes as a
                            // direct callback store.
                            if (function_result.inventory
                                    .stored_snapshot_table(image,
                                                          forwarded.call_site,
                                                          candidate)
                                    .has_value())
                                return DirectLocalPersistentStoreHarvest::NeedsExact;
                            StoredCodeAddressCandidate observation;
                            observation.target_address = validation.resolved_address;
                            observation.complete = false;
                            observation.guarded = true;
                            observation.store_instruction_addresses = {
                                site.store_instruction_address};
                            observation.evidence_call_sites.assign(
                                value.call_sites.begin(), value.call_sites.end());
                            observation.evidence_call_sites.push_back(
                                forwarded.call_site);
                            observation.evidence_callees.assign(
                                value.callees.begin(), value.callees.end());
                            observation.evidence_callees.push_back(forwarded.callee);
                            candidates.push_back(std::move(observation));
                        }
                    }
                }
                function_result.inventory.collect(std::move(candidates));
                return DirectLocalPersistentStoreHarvest::Handled;
            };
        const auto enqueue_forwarded_call =
            [&](const FunctionEvaluation::CallArguments& forwarded,
                const bool isolated,
                const std::set<std::uint32_t>& root_call_sites) {
                auto inventory_sink_sources =
                    abi_argument_taint_mask;
                auto persistent_store_sources = std::uint8_t{0u};
                auto indirect_dispatch_sources = std::uint8_t{0u};
                const bool has_memory_callback_payload =
                    has_stack_callback_memory_payload(
                        forwarded.state);
                if (unresolved_stack_callback_loss_reaches_inventory_sink(
                        forwarded.state, forwarded.callee)) {
                    function_result.walk_diagnostics.abi_stack_base_unresolved =
                        true;
                    emit_analyzer_stack_diagnostic(
                        "forwarded-call", function->entry_address,
                        forwarded.call_site, forwarded.callee);
                }
                const auto forwarded_function =
                    final_function_by_address.find(forwarded.callee);
                const auto forwarded_input =
                    final_candidate_inputs.find(forwarded.callee);
                if (forwarded_function == final_function_by_address.end() ||
                    forwarded_input == final_candidate_inputs.end() ||
                    (!functions_reaching_guarded_inventory_sink.contains(
                         forwarded.callee) &&
                     !has_memory_callback_payload))
                    return;
                // A zero source slice proves that this ordinary call cannot
                // forward an ABI argument into a callback store or dispatch.
                // Exact saved-stack memory and its loss marker are deliberately
                // independent of ABI arguments and still require the bounded
                // context walk.
                const auto guarded_tail_owner =
                    functions_with_guarded_abi_inventory_tail.contains(
                        forwarded.callee);
                auto direct_local_store_harvest =
                    DirectLocalPersistentStoreHarvest::NotApplicable;
                if (!guarded_tail_owner) {
                    const auto persistent_sources =
                        abi_persistent_store_sources.find(forwarded.callee);
                    if (persistent_sources !=
                        abi_persistent_store_sources.end())
                        persistent_store_sources = persistent_sources->second;
                    const auto dispatch_sources =
                        abi_indirect_dispatch_sources.find(forwarded.callee);
                    if (dispatch_sources !=
                        abi_indirect_dispatch_sources.end())
                        indirect_dispatch_sources = dispatch_sources->second;
                    inventory_sink_sources = static_cast<std::uint8_t>(
                        persistent_store_sources |
                        indirect_dispatch_sources);
                    if (inventory_sink_sources == 0u &&
                        !has_memory_callback_payload)
                        return;
                    // A local-store shortcut must not consume a context which
                    // also carries the value into an indirect dispatch.
                    if (indirect_dispatch_sources == 0u &&
                        !has_memory_callback_payload) {
                        direct_local_store_harvest =
                            harvest_direct_local_persistent_store(forwarded);
                        if (direct_local_store_harvest ==
                            DirectLocalPersistentStoreHarvest::Handled)
                            return;
                    }
                }
                if (indirect_dispatch_sources == 0u &&
                    direct_local_store_harvest !=
                        DirectLocalPersistentStoreHarvest::NeedsExact &&
                    !has_memory_callback_payload &&
                    !requires_forwarded_isolated_store_harvest(
                        image,
                        forwarded.state,
                        forwarded_input->second.state))
                    return;
                // Admission intentionally observes the complete caller state.
                // Only then is it reduced to the callee's interprocedural
                // general-register and stack live-in contract.
                auto entry_read_mask =
                    std::numeric_limits<std::uint16_t>::max();
                if (const auto mask =
                        forwarded_register_reads.find(forwarded.callee);
                    mask != forwarded_register_reads.end())
                    entry_read_mask = mask->second;
                const AbiStackArgumentReadSet* required_stack_reads = nullptr;
                if (const auto reads =
                        abi_stack_argument_reads.find(forwarded.callee);
                    reads != abi_stack_argument_reads.end())
                    required_stack_reads = &reads->second;
                enqueue_forwarded_context(
                    forwarded_function->second,
                    forwarded.callee,
                    false,
                    isolated,
                    isolated_store_input(forwarded.call_site,
                                         forwarded.state,
                                         entry_read_mask,
                                         (inventory_sink_sources &
                                          abi_stack_argument_taint) !=
                                             0u,
                                         required_stack_reads),
                    root_call_sites);
            };
        const auto enqueue_forwarded_tail =
            [&](const FunctionEvaluation::InventoryTransfer& forwarded,
                const bool isolated,
                const std::set<std::uint32_t>& root_call_sites) {
                if (unresolved_stack_callback_loss_reaches_inventory_sink(
                        forwarded.state, forwarded.target)) {
                    function_result.walk_diagnostics.abi_stack_base_unresolved =
                        true;
                    emit_analyzer_stack_diagnostic(
                        "forwarded-tail", function->entry_address,
                        forwarded.transfer_site, forwarded.target);
                }
                if (!has_forwarded_inventory_payload(forwarded.state))
                    return;
                const auto region =
                    final_inventory_region_by_address.find(forwarded.target);
                if (region == final_inventory_region_by_address.end()) return;
                const AbiStackArgumentReadSet* required_stack_reads = nullptr;
                if (const auto reads =
                        abi_stack_argument_reads.find(forwarded.target);
                    reads != abi_stack_argument_reads.end())
                    required_stack_reads = &reads->second;
                const std::uint16_t* required_register_reads = nullptr;
                if (const auto reads =
                        forwarded_register_reads.find(forwarded.target);
                    reads != forwarded_register_reads.end())
                    required_register_reads = &reads->second;
                enqueue_forwarded_context(region->second,
                                          forwarded.target,
                                          true,
                                          isolated,
                                          tail_store_input(
                                              forwarded.state,
                                              required_stack_reads,
                                              required_register_reads),
                                          root_call_sites);
            };
        const auto seed_forwarded_inventory =
            [&](const FunctionEvaluation& seed,
                const bool isolated,
                const std::set<std::uint32_t>& root_call_sites) {
                for (const auto& forwarded : seed.call_arguments)
                    enqueue_forwarded_call(forwarded, isolated, root_call_sites);
                for (const auto& forwarded : seed.inventory_transfers)
                    enqueue_forwarded_tail(forwarded, isolated, root_call_sites);
            };
        const auto drain_forwarded_inventory = [&] {
            struct ForwardedBatchItem {
                std::size_t context_index = 0u;
                std::uint64_t version = 0u;
                std::size_t evaluation_count = 0u;
                bool limit_reached = false;
                std::optional<ForwardedContextEvaluation>
                    evaluation;
                std::exception_ptr error;
            };
            auto& forwarded_executor = global_analysis_executor();
            while (!function_result.pending_forwarded_store_contexts.empty()) {
                while (!function_result
                            .pending_forwarded_store_contexts.empty()) {
                    const auto index =
                        function_result
                            .pending_forwarded_store_contexts.front();
                    const auto& context =
                        function_result.forwarded_store_contexts[
                            index];
                    if (!context.evaluated ||
                        context.evaluation_dirty)
                        break;
                    function_result
                        .pending_forwarded_store_contexts.pop_front();
                    function_result
                        .forwarded_store_context_queued[index] =
                        false;
                }
                if (function_result
                        .pending_forwarded_store_contexts.empty())
                    break;
                const auto batch_size =
                    std::min(
                        function_result
                            .pending_forwarded_store_contexts.size(),
                        forwarded_executor.maximum_jobs());
                std::vector<ForwardedBatchItem> batch(batch_size);
                auto pending_context =
                    function_result
                        .pending_forwarded_store_contexts.begin();
                for (std::size_t batch_index = 0u;
                     batch_index < batch.size();
                     ++batch_index, ++pending_context) {
                    auto& item = batch[batch_index];
                    item.context_index = *pending_context;
                    const auto& context =
                        function_result.forwarded_store_contexts[
                            item.context_index];
                    item.version = context.version;
                    item.evaluation_count =
                        context.evaluation_count;
                    item.limit_reached =
                        context.evaluation_count >=
                        maximum_forwarded_store_context_evaluations;
                }
                const auto evaluate_forwarded_item =
                    [&](ForwardedBatchItem& item) noexcept {
                        if (item.limit_reached) return;
                        try {
                            const auto& context =
                                function_result
                                    .forwarded_store_contexts[
                                    item.context_index];
                            const TailIngressMap*
                                local_tail_ingresses = nullptr;
                            if (context.tail) {
                                const auto local =
                                    final_inventory_region_tail_ingresses_by_entry
                                        .find(context.target);
                                if (local !=
                                    final_inventory_region_tail_ingresses_by_entry
                                        .end())
                                    local_tail_ingresses =
                                        &local->second;
                            }
                            item.evaluation.emplace(
                                evaluate_forwarded_context(
                                    context,
                                    context.root_call_sites,
                                    local_tail_ingresses));
                        } catch (...) {
                            item.error =
                                std::current_exception();
                        }
                    };
                if (batch.size() == 1u) {
                    evaluate_forwarded_item(batch.front());
                } else {
                    parallel_analysis_for(
                        forwarded_executor,
                        batch.size(),
                        maximum_parallel_resolution_jobs,
                        function_value_parallel_activity_if_observed,
                        [&](const std::size_t index) {
                            evaluate_forwarded_item(batch[index]);
                        });
                }
                for (auto& item : batch) {
                    if (function_result
                            .pending_forwarded_store_contexts
                            .empty() ||
                        function_result
                                .pending_forwarded_store_contexts
                                .front() !=
                            item.context_index)
                        throw std::logic_error(
                            "Paralleler Forwarded-Inventory-Fixpunkt "
                            "verlor die FIFO-Reihenfolge.");
                    function_result
                        .pending_forwarded_store_contexts.pop_front();
                    function_result
                        .forwarded_store_context_queued[
                            item.context_index] = false;
                    auto& context =
                        function_result.forwarded_store_contexts[
                            item.context_index];
                    if (context.evaluated &&
                        !context.evaluation_dirty)
                        continue;
                    if (context.evaluation_count >=
                        maximum_forwarded_store_context_evaluations) {
                        function_result.walk_diagnostics
                            .forwarded_store_context_limited_functions =
                            1u;
                        record_forwarded_store_limit(
                            ForwardedStoreContextLimitReason::
                                ReevaluationCount,
                            context.target,
                            context.tail,
                            context.isolated,
                            context.root_call_sites,
                            function_result
                                .forwarded_store_contexts.size(),
                            context.root_call_sites.size(),
                            context.evaluation_count,
                            &context.input,
                            nullptr);
                        continue;
                    }
                    const bool stale =
                        context.version != item.version ||
                        context.evaluation_count !=
                            item.evaluation_count;
                    if (stale) {
                        item.evaluation.reset();
                        item.error = {};
                        const TailIngressMap*
                            local_tail_ingresses = nullptr;
                        if (context.tail) {
                            const auto local =
                                final_inventory_region_tail_ingresses_by_entry
                                    .find(context.target);
                            if (local !=
                                final_inventory_region_tail_ingresses_by_entry
                                    .end())
                                local_tail_ingresses =
                                    &local->second;
                        }
                        try {
                            item.evaluation.emplace(
                                evaluate_forwarded_context(
                                    context,
                                    context.root_call_sites,
                                    local_tail_ingresses));
                        } catch (...) {
                            item.error =
                                std::current_exception();
                        }
                    }
                    ++context.evaluation_count;
                    if (item.error)
                        std::rethrow_exception(item.error);
                    if (!item.evaluation ||
                        !item.evaluation->artifact)
                        throw std::logic_error(
                            "Forwarded-Inventory-Auswertung fehlt.");
                    if (item.evaluation->cache_hit) {
                        ++function_result.walk_diagnostics
                              .forwarded_store_evaluation_cache_hits;
                    } else {
                        ++function_result.walk_diagnostics
                              .forwarded_store_evaluation_cache_misses;
                    }
                    merge_fixpoint_diagnostics(
                        function_result.walk_diagnostics,
                        item.evaluation->artifact->walk_diagnostics);
                    if (item.evaluation->artifact->evaluation
                            .local_fixpoint_budget_exhausted) {
                        record_local_fixpoint_limit();
                        return;
                    }
                    if (item.evaluation->live_inventory) {
                        std::move(*item.evaluation->live_inventory)
                            .replay_deferred_into(
                                function_result.inventory);
                    } else {
                        const EvaluationActivityScope replay_activity{
                            evaluation_activity_if_observed,
                            EvaluationActivityKind::ExactReplay};
                        item.evaluation->artifact->inventory
                            .replay_deferred_copy_into(
                                function_result.inventory);
                    }
                    context.evaluated = true;
                    context.evaluation_dirty = false;
                    context.resolutions =
                        item.evaluation->artifact->evaluation.resolutions;
                    const auto propagated_root_call_sites =
                        context.root_call_sites;
                    const auto context_isolated =
                        context.isolated;
                    for (const auto& forwarded :
                         item.evaluation->artifact->evaluation
                             .call_arguments)
                        enqueue_forwarded_call(
                            forwarded,
                            context_isolated,
                            propagated_root_call_sites);
                    for (const auto& forwarded :
                         item.evaluation->artifact->evaluation
                             .inventory_transfers)
                        enqueue_forwarded_tail(
                            forwarded,
                            context_isolated,
                            propagated_root_call_sites);
                    if (function_result
                            .local_fixpoint_budget_exhausted)
                        return;
                }
            }
        };
        const auto harvest_contextual_candidate_returns = [&] {
            if (!global_contextual_return_coordinator.has_value() ||
                function->entry_address !=
                    *global_contextual_return_coordinator)
                return;
            const bool merged_owner_context =
                contextual_candidate_return_owners.size() > 1u;
            std::map<std::uint32_t, AbstractState> context_inputs;
            std::map<std::uint32_t, FunctionValueSummary>
                contextual_summaries;
            std::map<std::uint32_t, std::set<std::uint32_t>>
                context_callers;
            std::deque<std::uint32_t> pending_contexts;
            std::unordered_set<std::uint32_t> queued_contexts;
            std::unordered_set<std::uint32_t>
                candidate_context_functions;
            std::vector<std::uint64_t> contextual_input_versions(
                functions.size(), 0u);
            std::vector<std::uint64_t> contextual_summary_versions(
                functions.size(), 0u);
            std::vector<std::uint64_t> contextual_candidate_versions(
                functions.size(), 0u);
            const auto enqueue_context =
                [&](const std::uint32_t address) {
                    if (queued_contexts.insert(address).second)
                        pending_contexts.push_back(address);
                };
            // Candidate-return contexts are a product-wide guarded inventory
            // domain. Seed every owner once and merge only where their helper
            // graphs meet. Rebuilding this closure independently for every
            // owner is equivalent semantically but multiplies the dominant
            // analysis cost by the number of owners.
            for (const auto owner :
                 contextual_candidate_return_owners) {
                context_inputs.emplace(
                    owner,
                    final_candidate_inputs.at(owner).state);
                enqueue_context(owner);
            }
            std::size_t contextual_evaluations = 0u;
            bool contextual_context_budget_exhausted = false;
            const auto contextual_entry_register_reads =
                [&](const std::uint32_t address) {
                    const auto found =
                        forwarded_register_reads.find(address);
                    return found == forwarded_register_reads.end()
                               ? std::numeric_limits<std::uint16_t>::max()
                               : found->second;
                };
            const auto contextual_entry_stack_reads =
                [&](const std::uint32_t address)
                    -> const AbiStackArgumentReadSet* {
                    const auto found =
                        abi_stack_argument_reads.find(address);
                    return found == abi_stack_argument_reads.end()
                               ? nullptr
                               : &found->second;
                };
            struct ContextualBatchItem {
                std::uint32_t address = 0u;
                std::size_t function_index = 0u;
                AbstractState input;
                std::uint64_t input_version = 0u;
                std::uint64_t candidate_version = 0u;
                std::vector<std::pair<std::size_t, std::uint64_t>>
                    summary_versions;
                std::optional<FunctionEvaluation> evaluation;
                GuardedCodeInventoryWalkDiagnostics diagnostics;
                std::exception_ptr error;
            };
            auto& contextual_executor = global_analysis_executor();
            while (!pending_contexts.empty() &&
                   !contextual_context_budget_exhausted &&
                   contextual_evaluations <
                       maximum_contextual_return_evaluations) {
                const auto remaining_budget =
                    maximum_contextual_return_evaluations -
                    contextual_evaluations;
                const auto batch_size =
                    std::min(
                        {pending_contexts.size(),
                         contextual_executor.maximum_jobs(),
                         remaining_budget});
                std::vector<ContextualBatchItem> batch(batch_size);
                auto pending_address = pending_contexts.begin();
                for (std::size_t index = 0u;
                     index < batch.size();
                     ++index, ++pending_address) {
                    auto& item = batch[index];
                    item.address = *pending_address;
                    item.function_index =
                        fixpoint_function_index.at(item.address);
                    item.input = context_inputs.at(item.address);
                    item.input_version =
                        contextual_input_versions[item.function_index];
                    item.candidate_version =
                        contextual_candidate_versions[item.function_index];
                    item.summary_versions.reserve(
                        contextual_summary_dependencies[item.function_index]
                            .size());
                    for (const auto dependency :
                         contextual_summary_dependencies[
                             item.function_index]) {
                        item.summary_versions.emplace_back(
                            dependency,
                            contextual_summary_versions[dependency]);
                    }
                    item.diagnostics.local_fixpoint_iteration_budget =
                        maximum_local_fixpoint_iterations;
                }
                const auto evaluate_contextual_item =
                    [&](ContextualBatchItem& item) noexcept {
                        try {
                            item.evaluation.emplace(
                                cached_evaluate_function(
                                    *fixpoint_functions[
                                        item.function_index],
                                    inventory_indirect_callees,
                                    tail_ingresses,
                                    summaries,
                                    item.input,
                                    ResolutionCollectionMode::None,
                                    true,
                                    nullptr,
                                    nullptr,
                                    &contextual_summaries,
                                    nullptr,
                                    &item.diagnostics,
                                    &abi_stack_argument_reads,
                                    0u)
                                    .first->evaluation);
                        } catch (...) {
                            item.error = std::current_exception();
                        }
                    };
                if (batch.size() == 1u) {
                    evaluate_contextual_item(batch.front());
                } else {
                    parallel_analysis_for(
                        contextual_executor,
                        batch.size(),
                        maximum_parallel_resolution_jobs,
                        function_value_parallel_activity_if_observed,
                        [&](const std::size_t index) {
                            evaluate_contextual_item(batch[index]);
                        });
                }
                for (auto& item : batch) {
                    bool stale =
                        contextual_input_versions[
                            item.function_index] !=
                            item.input_version ||
                        contextual_candidate_versions[
                            item.function_index] !=
                            item.candidate_version;
                    for (const auto& [dependency, version] :
                         item.summary_versions) {
                        stale =
                            stale ||
                            contextual_summary_versions[dependency] !=
                                version;
                    }
                    if (stale) {
                        item.input =
                            context_inputs.at(item.address);
                        item.evaluation.reset();
                        item.diagnostics = {};
                        item.diagnostics
                            .local_fixpoint_iteration_budget =
                            maximum_local_fixpoint_iterations;
                        item.error = {};
                        evaluate_contextual_item(item);
                    }
                    if (pending_contexts.empty() ||
                        pending_contexts.front() != item.address)
                        throw std::logic_error(
                            "Paralleler Contextual-Return-Fixpunkt "
                            "verlor die FIFO-Reihenfolge.");
                    pending_contexts.pop_front();
                    queued_contexts.erase(item.address);
                    ++contextual_evaluations;
                    if (item.error)
                        std::rethrow_exception(item.error);
                    auto context_evaluation =
                        std::move(*item.evaluation);
                    merge_fixpoint_diagnostics(
                        function_result.walk_diagnostics,
                        item.diagnostics);
                    if (context_evaluation
                            .local_fixpoint_budget_exhausted) {
                        record_local_fixpoint_limit();
                        return;
                    }
                    const auto previous =
                        contextual_summaries.find(item.address);
                    const bool summary_changed =
                        previous == contextual_summaries.end() ||
                        previous->second !=
                            context_evaluation.summary;
                    contextual_summaries[item.address] =
                        std::move(context_evaluation.summary);
                    if (summary_changed) {
                        ++contextual_summary_versions[
                            item.function_index];
                        const auto callers =
                            context_callers.find(item.address);
                        if (callers != context_callers.end()) {
                            for (const auto caller :
                                 callers->second)
                                enqueue_context(caller);
                        }
                    }
                    for (auto& observation :
                         context_evaluation.call_arguments) {
                        const auto pair = candidate_call_pair_key(
                            observation.call_site,
                            observation.callee);
                        const bool candidate_call =
                            candidate_call_pairs.contains(pair);
                        const bool contextual_helper_call =
                            candidate_context_functions.contains(
                                item.address) &&
                            semantic_call_pairs.contains(pair) &&
                            has_contextual_candidate_abi_argument(
                                observation.state,
                                contextual_entry_register_reads(
                                    observation.callee),
                                contextual_entry_stack_reads(
                                    observation.callee)) &&
                            requires_contextual_return(
                                observation.callee);
                        if (!candidate_call &&
                            !contextual_helper_call)
                            continue;
                        if (!final_function_by_address.contains(
                                observation.callee))
                            continue;
                        if (!context_inputs.contains(
                                observation.callee) &&
                            context_inputs.size() >=
                                final_function_by_address.size()) {
                            function_result.walk_diagnostics
                                .contextual_return_context_limited_functions =
                                1u;
                            emit_contextual_return_limit_diagnostic(
                                "contexts",
                                0u,
                                function->entry_address,
                                item.address,
                                observation.callee,
                                context_inputs.size(),
                                contextual_evaluations,
                                pending_contexts.size());
                            contextual_context_budget_exhausted =
                                true;
                            break;
                        }
                        auto callee_context_input =
                            observation.state;
                        if (candidate_call) {
                            mark_contextual_candidate_abi_arguments(
                                callee_context_input,
                                contextual_entry_register_reads(
                                    observation.callee),
                                contextual_entry_stack_reads(
                                    observation.callee));
                        }
                        context_callers[observation.callee].insert(
                            item.address);
                        const auto callee_index =
                            fixpoint_function_index.at(
                                observation.callee);
                        const bool candidate_membership_changed =
                            candidate_context_functions.insert(
                                observation.callee)
                                .second;
                        if (candidate_membership_changed)
                            ++contextual_candidate_versions[
                                callee_index];
                        const auto [stored, inserted] =
                            context_inputs.try_emplace(
                                observation.callee,
                                callee_context_input);
                        const bool input_changed =
                            inserted ||
                            merge_state(stored->second,
                                        callee_context_input);
                        if (input_changed) {
                            ++contextual_input_versions[
                                callee_index];
                        }
                        if (input_changed ||
                            candidate_membership_changed) {
                            enqueue_context(
                                observation.callee);
                        }
                    }
                    if (contextual_context_budget_exhausted)
                        break;
                }
            }
            if (contextual_context_budget_exhausted)
                return;
            if (!pending_contexts.empty()) {
                function_result.walk_diagnostics.contextual_return_evaluation_limited_functions = 1u;
                emit_contextual_return_limit_diagnostic(
                    "evaluations",
                    1u,
                    function->entry_address,
                    pending_contexts.front(),
                    pending_contexts.front(),
                    context_inputs.size(),
                    contextual_evaluations,
                    pending_contexts.size());
                return;
            }
            // The fixpoint above is intentionally side-effect free. Running
            // GuardedInventory while summaries are still changing would leak
            // transient targets and collector diagnostics into the final
            // result. Converged contexts are independent and can be harvested
            // in parallel into private deferred collectors, then merged in
            // canonical address order.
            struct StableContextResult {
                FunctionEvaluation evaluation;
                GuardedCodeInventoryCollector inventory{true};
                GuardedCodeInventoryWalkDiagnostics diagnostics;
                std::exception_ptr error;
            };
            std::vector<std::uint32_t> stable_addresses;
            stable_addresses.reserve(context_inputs.size());
            for (const auto& [address, context_input] :
                 context_inputs) {
                static_cast<void>(context_input);
                if (final_function_by_address.contains(address))
                    stable_addresses.push_back(address);
            }
            std::vector<StableContextResult> stable_results(
                stable_addresses.size());
            const auto harvest_stable_context =
                [&](const std::size_t index) noexcept {
                    auto& stable = stable_results[index];
                    const auto address = stable_addresses[index];
                    try {
                        stable.diagnostics
                            .local_fixpoint_iteration_budget =
                            maximum_local_fixpoint_iterations;
                        stable.evaluation =
                            cached_evaluate_function(
                            *final_function_by_address.at(address),
                            inventory_indirect_callees,
                            tail_ingresses,
                            summaries,
                            context_inputs.at(address),
                            ResolutionCollectionMode::GuardedInventory,
                            true,
                            &stable.inventory,
                            nullptr,
                            &contextual_summaries,
                            nullptr,
                            &stable.diagnostics,
                            &abi_stack_argument_reads,
                            target_abi_inventory_sink_sources(
                                address))
                                .first->evaluation;
                    } catch (...) {
                        stable.error = std::current_exception();
                    }
                };
            if (stable_results.size() < 2u) {
                if (!stable_results.empty())
                    harvest_stable_context(0u);
            } else {
                parallel_analysis_for(
                    contextual_executor,
                    stable_results.size(),
                    maximum_parallel_resolution_jobs,
                    function_value_parallel_activity_if_observed,
                    harvest_stable_context);
            }
            for (auto& stable : stable_results) {
                if (stable.error)
                    std::rethrow_exception(stable.error);
                merge_fixpoint_diagnostics(
                    function_result.walk_diagnostics,
                    stable.diagnostics);
                if (stable.evaluation
                        .local_fixpoint_budget_exhausted) {
                    record_local_fixpoint_limit();
                    return;
                }
                if (merged_owner_context) {
                    // Joining distinct roots deliberately forgets their
                    // correlation. The resulting finite targets are useful
                    // guarded AOT inventory, but can never become an
                    // authoritative complete control-flow proof.
                    stable.inventory
                        .mark_stored_candidates_incomplete();
                    for (auto& resolution :
                         stable.evaluation.resolutions) {
                        resolution.guarded = true;
                        resolution.complete = false;
                        resolution.evidence =
                            resolution.targets.empty()
                                ? ControlFlowEvidence::Unresolved
                                : ControlFlowEvidence::GuardedPartial;
                        resolution.reason =
                            resolution.targets.empty()
                                ? "global-contexts-unknown"
                                : "global-contexts-partial";
                    }
                    for (auto& transfer :
                         stable.evaluation.inventory_transfers) {
                        transfer.guarded = true;
                        transfer.complete = false;
                    }
                }
                std::move(stable.inventory)
                    .replay_deferred_into(
                        function_result.inventory);
                function_result.evaluation.resolutions.insert(
                    function_result.evaluation.resolutions.end(),
                    std::make_move_iterator(
                        stable.evaluation.resolutions.begin()),
                    std::make_move_iterator(
                        stable.evaluation.resolutions.end()));
                compact_root_resolutions();
                function_result.evaluation.call_arguments.insert(
                    function_result.evaluation.call_arguments.end(),
                    std::make_move_iterator(
                        stable.evaluation.call_arguments.begin()),
                    std::make_move_iterator(
                        stable.evaluation.call_arguments.end()));
                function_result.evaluation.inventory_transfers.insert(
                    function_result.evaluation.inventory_transfers.end(),
                    std::make_move_iterator(
                        stable.evaluation
                            .inventory_transfers.begin()),
                    std::make_move_iterator(
                        stable.evaluation
                            .inventory_transfers.end()));
            }
        };
        if (!result.budget_exhausted)
            harvest_contextual_candidate_returns();
        if (function_result.local_fixpoint_budget_exhausted)
            return finalize_root_result();
        coalesce_call_arguments(
            function_result.evaluation.call_arguments);
        coalesce_inventory_transfers(
            function_result.evaluation.inventory_transfers);
        const std::set<std::uint32_t> no_forwarded_root_call_sites;
        if (!result.budget_exhausted)
            seed_forwarded_inventory(function_result.evaluation,
                                     false,
                                     no_forwarded_root_call_sites);
        if (!result.budget_exhausted &&
            functions_reaching_guarded_inventory_sink.contains(
                function->entry_address)) {
            const auto dispatch_sources =
                abi_indirect_dispatch_sources.find(function->entry_address);
            const bool reaches_indirect_dispatch =
                dispatch_sources != abi_indirect_dispatch_sources.end() &&
                dispatch_sources->second != 0u;
            std::vector<std::pair<std::uint32_t, const AbstractState*>>
                isolated_observations;
            isolated_observations.reserve(input.observations.size());
            for (const auto& [call_site, observation] :
                 input.observations) {
                if (!functions_with_guarded_abi_inventory_tail.contains(function->entry_address) &&
                    !reaches_indirect_dispatch &&
                    !requires_isolated_store_harvest(input, call_site, observation))
                    continue;
                isolated_observations.emplace_back(
                    call_site, &observation);
            }
            const AbiStackArgumentReadSet* required_stack_reads =
                nullptr;
            if (const auto reads =
                    abi_stack_argument_reads.find(
                        function->entry_address);
                reads != abi_stack_argument_reads.end())
                required_stack_reads = &reads->second;
            auto required_register_reads =
                std::numeric_limits<std::uint16_t>::max();
            if (const auto reads =
                    forwarded_register_reads.find(
                        function->entry_address);
                reads != forwarded_register_reads.end())
                required_register_reads = reads->second;
            struct IsolatedEvaluationResult {
                FunctionEvaluation evaluation;
                GuardedCodeInventoryCollector inventory{true};
                GuardedCodeInventoryWalkDiagnostics diagnostics;
                std::exception_ptr error;
            };
            std::vector<IsolatedEvaluationResult> isolated_results(
                isolated_observations.size());
            const auto evaluate_isolated =
                [&](const std::size_t index) noexcept {
                    auto& isolated = isolated_results[index];
                    const auto& [call_site, observation] =
                        isolated_observations[index];
                    const std::set<std::uint32_t>
                        root_call_sites{call_site};
                    try {
                        isolated.diagnostics
                            .local_fixpoint_iteration_budget =
                            maximum_local_fixpoint_iterations;
                        isolated.evaluation =
                            cached_evaluate_function(
                            *function,
                            inventory_indirect_callees,
                            tail_ingresses,
                            summaries,
                            isolated_store_input(
                                call_site,
                                *observation,
                                required_register_reads,
                                true,
                                required_stack_reads),
                            ResolutionCollectionMode::
                                GuardedInventory,
                            true,
                            &isolated.inventory,
                            &root_call_sites,
                            nullptr,
                            nullptr,
                            &isolated.diagnostics,
                            &abi_stack_argument_reads,
                            target_abi_inventory_sink_sources(
                                function->entry_address))
                                .first->evaluation;
                    } catch (...) {
                        isolated.error =
                            std::current_exception();
                    }
                };
            if (isolated_results.size() < 2u) {
                if (!isolated_results.empty())
                    evaluate_isolated(0u);
            } else {
                parallel_analysis_for(
                    global_analysis_executor(),
                    isolated_results.size(),
                    maximum_parallel_resolution_jobs,
                    function_value_parallel_activity_if_observed,
                    evaluate_isolated);
            }
            for (std::size_t index = 0u;
                 index < isolated_results.size();
                 ++index) {
                auto& isolated = isolated_results[index];
                if (isolated.error)
                    std::rethrow_exception(isolated.error);
                merge_fixpoint_diagnostics(
                    function_result.walk_diagnostics,
                    isolated.diagnostics);
                if (isolated.evaluation
                        .local_fixpoint_budget_exhausted) {
                    record_local_fixpoint_limit();
                    return finalize_root_result();
                }
                std::move(isolated.inventory)
                    .replay_deferred_into(
                        function_result.inventory);
                function_result.evaluation.resolutions.insert(
                    function_result.evaluation.resolutions.end(),
                    std::make_move_iterator(
                        isolated.evaluation.resolutions.begin()),
                    std::make_move_iterator(
                        isolated.evaluation.resolutions.end()));
                compact_root_resolutions();
                const std::set<std::uint32_t> root_call_sites{
                    isolated_observations[index].first};
                seed_forwarded_inventory(
                    isolated.evaluation,
                    true,
                    root_call_sites);
                if (function_result.walk_diagnostics
                        .forwarded_store_context_limited_functions != 0u)
                    break;
            }
        }
        if (!result.budget_exhausted)
            drain_forwarded_inventory();
        if (function_result.local_fixpoint_budget_exhausted)
            return finalize_root_result();
        for (auto& context : function_result.forwarded_store_contexts) {
            function_result.evaluation.resolutions.insert(
                function_result.evaluation.resolutions.end(),
                std::make_move_iterator(context.resolutions.begin()),
                std::make_move_iterator(context.resolutions.end()));
            compact_root_resolutions();
        }
        return finalize_root_result();
    };

    // Resolution roots are pure and may finish in any order, but inventory
    // admission is deliberately order-sensitive: the bounded raw-candidate
    // collector keeps deterministic top-K evidence and supports later
    // complete-evidence promotion.  Fold roots in canonical function order,
    // never worker-completion order.  A private copy makes the fold
    // transactional: if any root reaches its local cap, none of the partial
    // resolution inventory is published.
    std::optional<GuardedCodeInventoryCollector>
        resolution_inventory_candidate;
    if (!result.budget_exhausted)
        resolution_inventory_candidate.emplace(
            guarded_inventory_collector);
    bool resolution_local_fixpoint_budget_exhausted = false;
    const auto commit_resolution_result =
        [&](ResolutionFunctionResult resolved) {
        inventory_walk_diagnostics.forwarded_store_context_limited_functions +=
            resolved.walk_diagnostics.forwarded_store_context_limited_functions;
        inventory_walk_diagnostics.forwarded_store_evaluation_cache_hits +=
            resolved.walk_diagnostics
                .forwarded_store_evaluation_cache_hits;
        inventory_walk_diagnostics.forwarded_store_evaluation_cache_misses +=
            resolved.walk_diagnostics
                .forwarded_store_evaluation_cache_misses;
        inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.insert(
            inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.end(),
            std::make_move_iterator(
                resolved.walk_diagnostics.forwarded_store_context_limit_diagnostics.begin()),
            std::make_move_iterator(
                resolved.walk_diagnostics.forwarded_store_context_limit_diagnostics.end()));
        inventory_walk_diagnostics.contextual_return_context_limited_functions +=
            resolved.walk_diagnostics.contextual_return_context_limited_functions;
        inventory_walk_diagnostics.contextual_return_evaluation_limited_functions +=
            resolved.walk_diagnostics.contextual_return_evaluation_limited_functions;
        inventory_walk_diagnostics.abi_stack_argument_projection_truncated_functions +=
            resolved.walk_diagnostics.abi_stack_argument_projection_truncated_functions;
        inventory_walk_diagnostics.local_fixpoint_limited_evaluations +=
            resolved.walk_diagnostics.local_fixpoint_limited_evaluations;
        inventory_walk_diagnostics.maximum_local_fixpoint_iterations =
            std::max(
                inventory_walk_diagnostics
                    .maximum_local_fixpoint_iterations,
                resolved.walk_diagnostics
                    .maximum_local_fixpoint_iterations);
        inventory_walk_diagnostics.inventory_candidate_values_truncated =
            inventory_walk_diagnostics.inventory_candidate_values_truncated ||
            resolved.walk_diagnostics.inventory_candidate_values_truncated;
        inventory_walk_diagnostics.abi_stack_base_unresolved =
            inventory_walk_diagnostics.abi_stack_base_unresolved ||
            resolved.walk_diagnostics.abi_stack_base_unresolved;
        if (resolved.local_fixpoint_budget_exhausted) {
            resolution_local_fixpoint_budget_exhausted = true;
            resolution_inventory_candidate.reset();
            result.resolutions.clear();
            resolution_count = 0u;
        }
        const bool publish_resolution_outputs =
            !result.budget_exhausted &&
            !resolution_local_fixpoint_budget_exhausted;
        if (publish_resolution_outputs) {
            std::move(resolved.inventory).replay_into(
                *resolution_inventory_candidate);
            resolution_count +=
                resolved.evaluation.resolutions.size();
            result.resolutions.insert(
                result.resolutions.end(),
                std::make_move_iterator(
                    resolved.evaluation.resolutions.begin()),
                std::make_move_iterator(
                    resolved.evaluation.resolutions.end()));
        }
        ++resolution_functions_committed;
        if (resolution_functions_committed <= 16u ||
            resolution_functions_committed % 128u == 0u ||
            resolution_functions_committed == resolution_functions_total)
            report_progress("resolution-progress");
    };

    if (resolution_functions.size() <
        minimum_parallel_resolution_functions) {
        for (std::size_t index = 0u;
             index < resolution_functions.size();
             ++index) {
            progress_resolution_head_of_line_index.store(
                index, std::memory_order_relaxed);
            progress_resolution_head_started_nanoseconds.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            commit_resolution_result(
                evaluate_resolution_function(index));
        }
        progress_resolution_head_started_nanoseconds.store(
            0, std::memory_order_relaxed);
    } else {
        struct ResolutionResultSlot {
            std::optional<ResolutionFunctionResult> result;
            std::exception_ptr error;
            bool ready = false;
        };
        std::vector<ResolutionResultSlot> slots(
            resolution_functions.size());
        std::mutex slots_mutex;
        std::condition_variable slots_ready;
        bool producer_done = false;
        std::exception_ptr producer_error;
        auto& resolution_executor = global_analysis_executor();
        std::jthread producer([&]() noexcept {
            try {
                parallel_analysis_for(
                    resolution_executor,
                    resolution_functions.size(),
                    maximum_parallel_resolution_jobs,
                    function_value_parallel_activity_if_observed,
                    [&](const std::size_t index) noexcept {
                        std::optional<ResolutionFunctionResult> resolved;
                        std::exception_ptr error;
                        try {
                            resolved.emplace(
                                evaluate_resolution_function(index));
                        } catch (...) {
                            error = std::current_exception();
                        }
                        {
                            const std::lock_guard lock(slots_mutex);
                            slots[index].result =
                                std::move(resolved);
                            slots[index].error =
                                std::move(error);
                            slots[index].ready = true;
                            progress_resolution_functions_ready.fetch_add(
                                1u, std::memory_order_relaxed);
                        }
                        slots_ready.notify_all();
                        resolution_executor.notify_waiters();
                    });
            } catch (...) {
                const std::lock_guard lock(slots_mutex);
                producer_error = std::current_exception();
            }
            {
                const std::lock_guard lock(slots_mutex);
                producer_done = true;
            }
            slots_ready.notify_all();
            resolution_executor.notify_waiters();
        });

        std::exception_ptr first_resolution_error;
        bool missing_resolution_result = false;
        for (std::size_t index = 0u;
             index < slots.size();
             ++index) {
            progress_resolution_head_of_line_index.store(
                index, std::memory_order_relaxed);
            progress_resolution_head_started_nanoseconds.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            const auto ready_or_done = [&] {
                const std::lock_guard lock(slots_mutex);
                return slots[index].ready || producer_done;
            };
            if (resolution_executor.current_thread_is_worker()) {
                resolution_executor.help_until(ready_or_done);
            } else {
                std::unique_lock lock(slots_mutex);
                slots_ready.wait(
                    lock,
                    [&] {
                        return slots[index].ready ||
                               producer_done;
                    });
            }

            std::optional<ResolutionFunctionResult> resolved;
            {
                const std::lock_guard lock(slots_mutex);
                if (!slots[index].ready) {
                    missing_resolution_result = true;
                    break;
                }
                first_resolution_error =
                    slots[index].error;
                resolved =
                    std::move(slots[index].result);
                progress_resolution_functions_ready.fetch_sub(
                    1u, std::memory_order_relaxed);
            }
            if (first_resolution_error) break;
            if (!resolved) {
                missing_resolution_result = true;
                break;
            }
            commit_resolution_result(
                std::move(*resolved));
        }
        progress_resolution_head_started_nanoseconds.store(
            0, std::memory_order_relaxed);
        producer.join();
        if (producer_error)
            std::rethrow_exception(producer_error);
        if (first_resolution_error)
            std::rethrow_exception(first_resolution_error);
        if (missing_resolution_result)
            throw std::logic_error(
                "Parallele Function-Resolution lieferte kein Ergebnis.");
    }
    if (resolution_local_fixpoint_budget_exhausted)
        result.budget_exhausted = true;
    if (resolution_inventory_candidate)
        guarded_inventory_collector =
            std::move(*resolution_inventory_candidate);
    std::sort(
        inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.begin(),
        inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.owner_entry,
                            left.target,
                            left.reason,
                            left.tail,
                            left.isolated,
                            left.exemplar_root_call_site) <
                   std::tie(right.owner_entry,
                            right.target,
                            right.reason,
                            right.tail,
                            right.isolated,
                            right.exemplar_root_call_site);
        });
    inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.erase(
        std::unique(
            inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.begin(),
            inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.end()),
        inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.end());
    constexpr std::size_t maximum_forwarded_store_limit_diagnostics = 16u;
    if (inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.size() >
        maximum_forwarded_store_limit_diagnostics) {
        inventory_walk_diagnostics.forwarded_store_context_limit_diagnostics.resize(
            maximum_forwarded_store_limit_diagnostics);
    }
    result.guarded_code_inventory = guarded_inventory_collector.finish();
    result.guarded_code_inventory.walk_diagnostics = inventory_walk_diagnostics;
    coalesce_resolutions(result.resolutions);
    resolution_count = result.resolutions.size();
    stop_progress_pulse();
    report_progress("complete");
    result.progress_callback_failed =
        progress_callback_failed.load(
            std::memory_order_relaxed);
    return result;
}

} // namespace katana::analysis
