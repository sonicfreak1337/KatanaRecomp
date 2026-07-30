#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"
#include "guarded_native_entry_shape.hpp"
#include "snapshot_pointer_candidates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis {
namespace {

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
// This cache is a performance aid, not an analysis budget. Completed entries
// are evicted least-recently-used; if every slot is still in flight, the
// context is evaluated normally so no evidence or fail-closed diagnostic is
// lost.
constexpr std::size_t maximum_pass_forwarded_evaluation_cache_entries =
    256u;
constexpr std::size_t maximum_contextual_return_evaluations =
    maximum_fixpoint_iterations;
constexpr std::size_t maximum_inventory_stack_coordinates = 64u;
constexpr std::size_t maximum_inventory_regions = maximum_guarded_code_inventory;
constexpr std::size_t maximum_inventory_region_blocks = 256u;
constexpr std::size_t maximum_memory_values = 256u;
constexpr std::size_t maximum_parallel_resolution_jobs = 12u;
constexpr std::size_t minimum_parallel_resolution_functions = 64u;
constexpr std::size_t maximum_abi_stack_read_top_chain = 16u;

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
void add_unresolved_saved_stack_alias(
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
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& left, const auto& right) {
                      if (left.target_address != right.target_address)
                          return left.target_address < right.target_address;
                      return left.store_instruction_addresses <
                             right.store_instruction_addresses;
        });
        for (auto& candidate : candidates) {
            const auto complete_evidence = candidate.complete;
            collect_stored_candidate(std::move(candidate),
                                     complete_evidence);
        }
    }

    void collect(std::vector<ReturnedCodeAddressTableCandidate> candidates) {
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& left, const auto& right) {
                      if (left.table_address != right.table_address)
                          return left.table_address < right.table_address;
                      return left.load_instruction_addresses <
                             right.load_instruction_addresses;
                  });
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
        const auto target = candidate.target_address;
        const auto existing = stored_candidates_.find(target);
        if (existing == stored_candidates_.end() &&
            stored_candidates_.size() >=
                maximum_raw_stored_code_candidates) {
            raw_stored_candidates_truncated_ = true;
            candidate_inventory_truncated_ = true;
            auto worst = stored_candidates_.end();
            for (auto candidate_it = stored_candidates_.rbegin();
                 candidate_it != stored_candidates_.rend();
                 ++candidate_it) {
                if (!complete_stored_targets_.contains(
                        candidate_it->first)) {
                    worst = std::prev(candidate_it.base());
                    break;
                }
            }
            if (complete_evidence) {
                if (worst == stored_candidates_.end()) {
                    worst = std::prev(stored_candidates_.end());
                    if (target >= worst->first) return;
                }
            } else {
                if (worst == stored_candidates_.end() ||
                    target >= worst->first)
                    return;
            }
            complete_stored_targets_.erase(worst->first);
            stored_candidates_.erase(worst);
        }
        const auto [stored, inserted] =
            stored_candidates_.try_emplace(target, std::move(candidate));
        if (complete_evidence)
            complete_stored_targets_.insert(target);
        if (inserted) return;
        auto& destination = stored->second;
        destination.complete = destination.complete && candidate.complete;
        destination.guarded = true;
        destination.store_instruction_addresses.insert(
            destination.store_instruction_addresses.end(),
            candidate.store_instruction_addresses.begin(),
            candidate.store_instruction_addresses.end());
        destination.evidence_call_sites.insert(destination.evidence_call_sites.end(),
                                               candidate.evidence_call_sites.begin(),
                                               candidate.evidence_call_sites.end());
        destination.evidence_callees.insert(destination.evidence_callees.end(),
                                            candidate.evidence_callees.begin(),
                                            candidate.evidence_callees.end());
    }

    void collect_returned_candidate(ReturnedCodeAddressTableCandidate candidate) {
        table_scan_truncated_ = table_scan_truncated_ || candidate.scan_truncated;
        normalize(candidate.target_addresses);
        if (candidate.target_addresses.empty()) return;
        const auto [stored, inserted] =
            returned_tables_.try_emplace(candidate.table_address, std::move(candidate));
        if (inserted) return;
        auto& destination = stored->second;
        destination.target_addresses.insert(destination.target_addresses.end(),
                                             candidate.target_addresses.begin(),
                                             candidate.target_addresses.end());
        destination.load_instruction_addresses.insert(
            destination.load_instruction_addresses.end(),
            candidate.load_instruction_addresses.begin(),
            candidate.load_instruction_addresses.end());
        destination.evidence_call_sites.insert(destination.evidence_call_sites.end(),
                                               candidate.evidence_call_sites.begin(),
                                               candidate.evidence_call_sites.end());
        destination.evidence_callees.insert(destination.evidence_callees.end(),
                                            candidate.evidence_callees.begin(),
                                            candidate.evidence_callees.end());
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
    std::map<std::uint32_t, ReturnedCodeAddressTableCandidate> returned_tables_;
    std::unordered_map<std::uint32_t, std::optional<JumpTableAnalysis>>
        stored_snapshot_tables_;
    detail::GuardedNativeEntryShapeCache* shape_cache_ = nullptr;
    bool defer_stored_admission_ = false;
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
                add_unresolved_saved_stack_alias(
                    destination,
                    unresolved_saved_stack_alias_source_stack,
                    candidate.inventory_saved_stack_epoch
                        .tracks_current_epoch);
                candidate.inventory_saved_stack_epoch = {};
                changed = true;
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
            add_unresolved_saved_stack_alias(
                destination,
                unresolved_saved_stack_alias_source_stack,
                candidate.inventory_saved_stack_epoch
                    .tracks_current_epoch);
            candidate.inventory_saved_stack_epoch = {};
            changed = true;
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
            add_unresolved_saved_stack_alias(
                destination,
                unresolved_saved_stack_alias_source_memory,
                candidate.inventory_saved_stack_epoch
                    .tracks_current_epoch);
            candidate.inventory_saved_stack_epoch = {};
            changed = true;
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

void add_unresolved_saved_stack_alias(
    AbstractState& state,
    const std::uint8_t sources,
    const bool tracks_current_epoch) {
    if (sources == 0u) return;
    state.inventory_unresolved_saved_stack_alias_sources =
        static_cast<std::uint8_t>(
            state.inventory_unresolved_saved_stack_alias_sources |
            sources);
    state.inventory_unresolved_saved_stack_alias_tracks_current_epoch =
        state
            .inventory_unresolved_saved_stack_alias_tracks_current_epoch ||
        tracks_current_epoch;
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
    auto input = caller;
    input.stack_values.clear();
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
    std::vector<std::pair<std::uint32_t, AbstractState>> returns;
    std::size_t local_fixpoint_iterations = 0u;
    while (!pending.empty()) {
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
        if (controlling_line(*block->second).instruction.kind ==
            katana::sh4::InstructionKind::Rts) {
            returns.emplace_back(controlling_line(*block->second).address, state);
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
    }

    // A return block can be revisited while its input converges. Return-site
    // identity is semantic; visit history is not. Coalesce each physical RTS
    // before publishing summaries so recursive callers cannot be requeued by
    // an ever-growing duplicate return_sites vector.
    std::sort(returns.begin(),
              returns.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    std::vector<std::pair<std::uint32_t, AbstractState>>
        coalesced_returns;
    coalesced_returns.reserve(returns.size());
    for (auto& returned : returns) {
        if (coalesced_returns.empty() ||
            coalesced_returns.back().first != returned.first) {
            coalesced_returns.push_back(std::move(returned));
            continue;
        }
        static_cast<void>(merge_state(
            coalesced_returns.back().second,
            returned.second,
            may_merge_stack_inventory));
    }
    returns = std::move(coalesced_returns);

    // A block can be revisited while its local input converges.  Publish one
    // conservative observation per physical callsite/callee pair; exposing
    // transient visits to the interprocedural worklist lets the same callsite
    // alternately replace its callee input and can keep a recursive graph
    // alive long after the local state has stabilized.
    coalesce_call_arguments(evaluation.call_arguments);

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
        auto returned_memory = returns.front().second.memory_values;
        for (auto return_state = returns.begin() + 1; return_state != returns.end();
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

} // namespace

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
    return detail::analyze_function_values_with_guarded_entry_cache(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        progress_callback,
        guarded_native_entry_shapes);
}

FunctionValueAnalysisResult
detail::analyze_function_values_with_abi_contract_observer_for_testing(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const AbiContractObserver& observer) {
    detail::GuardedNativeEntryShapeCache guarded_native_entry_shapes(image);
    return detail::analyze_function_values_with_guarded_entry_cache(
        image,
        lines,
        function_boundaries,
        resolved_edges,
        {},
        guarded_native_entry_shapes,
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
    begin_detailed_analyzer_diagnostic_epoch();
    FunctionValueAnalysisResult result;
    result.iteration_budget = maximum_fixpoint_iterations;
    std::size_t completed_functions = 0u;
    std::size_t resolution_count = 0u;
    std::size_t block_count = 0u;
    std::size_t function_count = 0u;
    std::size_t pending_count = 0u;
    const auto report_progress = [&](const std::string_view phase) {
        if (!progress_callback) return;
        progress_callback({phase,
                           function_count,
                           block_count,
                           result.fixpoint_iterations,
                           completed_functions,
                           pending_count,
                           resolution_count});
    };
    if (lines.empty() || function_boundaries.empty() ||
        image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC)
        return result;
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
    while (!pending_inventory_regions.empty() &&
           inventory_regions.size() < maximum_inventory_regions) {
        const auto target = pending_inventory_regions.front();
        pending_inventory_regions.pop_front();
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
    while (!pending_abi_signatures.empty()) {
        if (abi_signature_iterations >= maximum_fixpoint_iterations) {
            result.budget_exhausted = true;
            break;
        }
        ++abi_signature_iterations;
        const auto address = pending_abi_signatures.front();
        pending_abi_signatures.pop_front();
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
        while (!pending_abi_stack_reads.empty()) {
            if (abi_stack_read_iterations >= maximum_fixpoint_iterations) {
                result.budget_exhausted = true;
                break;
            }
            ++abi_stack_read_iterations;
            const auto address = pending_abi_stack_reads.front();
            pending_abi_stack_reads.pop_front();
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
        while (!pending_abi_store_signatures.empty()) {
            if (abi_store_signature_iterations >= maximum_fixpoint_iterations) {
                result.budget_exhausted = true;
                break;
            }
            ++abi_store_signature_iterations;
            const auto address = pending_abi_store_signatures.front();
            pending_abi_store_signatures.pop_front();
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
    while (!pending_inventory_reachability.empty()) {
        const auto callee = pending_inventory_reachability.front();
        pending_inventory_reachability.pop_front();
        const auto callers = inventory_callers_by_callee.find(callee);
        if (callers == inventory_callers_by_callee.end()) continue;
        for (const auto caller : callers->second) {
            add_inventory_sink(caller);
        }
    }
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
    while (!pending.empty()) {
        if (result.fixpoint_iterations >= maximum_fixpoint_iterations) {
            result.budget_exhausted = true;
            break;
        }
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        const auto function = function_by_address.find(address);
        if (function == function_by_address.end()) continue;
        ++result.fixpoint_iterations;
        pending_count = pending.size();
        const bool sampled_iteration = result.fixpoint_iterations <= 16u ||
                                       (result.fixpoint_iterations &
                                        (result.fixpoint_iterations - 1u)) == 0u ||
                                       result.fixpoint_iterations % 128u == 0u;
        if (sampled_iteration) report_progress("fixpoint-evaluate-start");
        emit_analyzer_fixpoint_trace("global-start",
                                     result.fixpoint_iterations,
                                     address,
                                     address,
                                     pending.size());
        auto evaluation = evaluate_function(image,
                                            *function->second,
                                            block_index,
                                            summary_indirect_callees,
                                            no_tail_ingresses,
                                            summaries,
                                            candidate_inputs[address].state,
                                            ResolutionCollectionMode::None,
                                            false,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            &inventory_walk_diagnostics,
                                            &abi_stack_argument_reads,
                                            target_abi_inventory_sink_sources(
                                                address));
        emit_analyzer_fixpoint_trace("global-complete",
                                     result.fixpoint_iterations,
                                     address,
                                     address,
                                     pending.size());
        if (sampled_iteration) report_progress("fixpoint-evaluate-complete");
        auto& previous = summaries[address];
        if (previous != evaluation.summary) {
            previous = std::move(evaluation.summary);
            const auto callers = callers_by_callee.find(address);
            if (callers != callers_by_callee.end()) {
                for (const auto caller : callers->second) {
                    if (queued.insert(caller).second) pending.push_back(caller);
                }
            }
        }
        for (const auto& observation : evaluation.call_arguments) {
            if (unresolved_stack_callback_loss_reaches_inventory_sink(
                    observation.state, observation.callee)) {
                inventory_walk_diagnostics.abi_stack_base_unresolved =
                    true;
                emit_analyzer_stack_diagnostic(
                    "fixpoint-call", address, observation.call_site, observation.callee);
            }
            const auto input = candidate_inputs.find(observation.callee);
            if (input == candidate_inputs.end()) continue;
            if (merge_candidate_input(
                    input->second,
                    observation,
                    &inventory_walk_diagnostics)) {
                if (queued.insert(observation.callee).second) {
                    pending.push_back(observation.callee);
                }
            } else {
                ++result.unchanged_ingress_skips;
            }
        }
        pending_count = pending.size();
    }
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
    std::vector<const FunctionInfo*> resolution_functions;
    resolution_functions.reserve(functions.size());
    for (const auto& function : functions)
        resolution_functions.push_back(&function);
    std::sort(resolution_functions.begin(),
              resolution_functions.end(),
              [](const auto* left, const auto* right) {
                  return left->entry_address < right->entry_address;
              });

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
        std::vector<InterproceduralTargetResolution> resolutions;
        std::vector<FunctionEvaluation::CallArguments> call_arguments;
        std::vector<FunctionEvaluation::InventoryTransfer> inventory_transfers;
    };
    struct ResolutionFunctionResult {
        FunctionEvaluation evaluation;
        GuardedCodeInventoryCollector inventory{true};
        GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
        std::vector<ForwardedStoreContext> forwarded_store_contexts;
        std::deque<std::size_t> pending_forwarded_store_contexts;
        std::vector<bool> forwarded_store_context_queued;
    };
    struct CachedForwardedEvaluation {
        FunctionEvaluation evaluation;
        GuardedCodeInventoryCollector inventory{true};
        GuardedCodeInventoryWalkDiagnostics walk_diagnostics;
    };
    using ForwardedEvaluationCacheBucketKey =
        std::tuple<std::uint32_t,
                   std::uint32_t,
                   bool,
                   bool,
                   bool,
                   std::set<std::uint32_t>>;
    struct ForwardedEvaluationCacheEntry {
        AbstractState input;
        std::shared_future<
            std::shared_ptr<const CachedForwardedEvaluation>>
            result;
        std::uint64_t last_use = 0u;
    };
    std::map<ForwardedEvaluationCacheBucketKey,
             std::vector<ForwardedEvaluationCacheEntry>>
        forwarded_evaluation_cache;
    std::mutex forwarded_evaluation_cache_mutex;
    std::size_t forwarded_evaluation_cache_entries = 0u;
    std::uint64_t forwarded_evaluation_cache_clock = 0u;
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
    const auto evaluate_forwarded_context =
        [&](const ForwardedStoreContext& context,
            const std::set<std::uint32_t>& root_call_sites,
            const TailIngressMap* const local_tail_ingresses) {
            using CachedResult =
                std::shared_ptr<const CachedForwardedEvaluation>;
            const auto compute = [&]() -> CachedResult {
                auto result =
                    std::make_shared<CachedForwardedEvaluation>();
                result->evaluation = evaluate_function(
                    image,
                    *context.function,
                    block_index,
                    inventory_indirect_callees,
                    tail_ingresses,
                    summaries,
                    context.input,
                    ResolutionCollectionMode::GuardedInventory,
                    true,
                    &result->inventory,
                    context.isolated ? &root_call_sites : nullptr,
                    nullptr,
                    local_tail_ingresses,
                    &result->walk_diagnostics,
                    &abi_stack_argument_reads,
                    target_abi_inventory_sink_sources(
                        context.target));
                return result;
            };

            const ForwardedEvaluationCacheBucketKey key{
                context.function->entry_address,
                context.target,
                context.tail,
                context.isolated,
                local_tail_ingresses != nullptr,
                root_call_sites};
            std::shared_future<CachedResult> future;
            std::shared_ptr<std::promise<CachedResult>> producer;
            {
                const std::lock_guard lock(
                    forwarded_evaluation_cache_mutex);
                auto bucket =
                    forwarded_evaluation_cache.find(key);
                if (bucket != forwarded_evaluation_cache.end()) {
                    auto found = std::find_if(
                        bucket->second.begin(),
                        bucket->second.end(),
                        [&](const auto& entry) {
                            return entry.input == context.input;
                        });
                    if (found != bucket->second.end()) {
                        future = found->result;
                        found->last_use =
                            ++forwarded_evaluation_cache_clock;
                    }
                }
                if (!future.valid() &&
                    forwarded_evaluation_cache_entries >=
                        maximum_pass_forwarded_evaluation_cache_entries) {
                    auto oldest_bucket =
                        forwarded_evaluation_cache.end();
                    std::size_t oldest_index = 0u;
                    auto oldest_use =
                        std::numeric_limits<std::uint64_t>::max();
                    for (auto candidate_bucket =
                             forwarded_evaluation_cache.begin();
                         candidate_bucket !=
                         forwarded_evaluation_cache.end();
                         ++candidate_bucket) {
                        for (std::size_t index = 0u;
                             index < candidate_bucket->second.size();
                             ++index) {
                            const auto& candidate =
                                candidate_bucket->second[index];
                            if (candidate.last_use >= oldest_use ||
                                candidate.result.wait_for(
                                    std::chrono::seconds{0}) !=
                                    std::future_status::ready)
                                continue;
                            oldest_bucket = candidate_bucket;
                            oldest_index = index;
                            oldest_use = candidate.last_use;
                        }
                    }
                    if (oldest_bucket !=
                        forwarded_evaluation_cache.end()) {
                        oldest_bucket->second.erase(
                            oldest_bucket->second.begin() +
                            static_cast<std::ptrdiff_t>(
                                oldest_index));
                        --forwarded_evaluation_cache_entries;
                        if (oldest_bucket->second.empty())
                            forwarded_evaluation_cache.erase(
                                oldest_bucket);
                    }
                }
                if (!future.valid() &&
                    forwarded_evaluation_cache_entries <
                        maximum_pass_forwarded_evaluation_cache_entries) {
                    producer =
                        std::make_shared<std::promise<CachedResult>>();
                    future = producer->get_future().share();
                    forwarded_evaluation_cache[key].push_back(
                        {context.input,
                         future,
                         ++forwarded_evaluation_cache_clock});
                    ++forwarded_evaluation_cache_entries;
                }
            }

            if (!future.valid())
                return std::pair{compute(), false};
            if (!producer)
                return std::pair{future.get(), true};
            try {
                auto result = compute();
                producer->set_value(result);
                return std::pair{std::move(result), false};
            } catch (...) {
                producer->set_exception(
                    std::current_exception());
                throw;
            }
        };
    const auto merge_cached_forwarded_diagnostics =
        [](GuardedCodeInventoryWalkDiagnostics& destination,
           const GuardedCodeInventoryWalkDiagnostics& source) {
            destination.maximum_local_fixpoint_iterations =
                std::max(
                    destination.maximum_local_fixpoint_iterations,
                    source.maximum_local_fixpoint_iterations);
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
    const auto evaluate_resolution_function = [&](const std::size_t function_index) {
        ResolutionFunctionResult function_result;
        function_result.walk_diagnostics.forwarded_store_context_budget =
            maximum_forwarded_store_contexts;
        function_result.walk_diagnostics.contextual_return_context_budget =
            final_function_by_address.size();
        function_result.walk_diagnostics.contextual_return_evaluation_budget =
            maximum_contextual_return_evaluations;
        function_result.walk_diagnostics.abi_stack_argument_slot_budget =
            maximum_abi_stack_argument_slots;
        const auto* function = resolution_functions[function_index];
        const auto& input = final_candidate_inputs.at(function->entry_address);
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
        function_result.evaluation = evaluate_function(image,
                                                       *function,
                                                       block_index,
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
                                                            function->entry_address));
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
                const auto same_target_kind =
                    [&](const auto& context) {
                        return context.target == target &&
                               context.tail == tail &&
                               context.isolated == isolated;
                    };
                auto existing = std::find_if(
                    function_result.forwarded_store_contexts.begin(),
                    function_result.forwarded_store_contexts.end(),
                    [&](const auto& context) {
                        return same_target_kind(context) &&
                               (!isolated ||
                                (!root_call_sites.empty() &&
                                 context.root_call_sites ==
                                     root_call_sites));
                    });
                const auto widen_partition =
                    existing !=
                    function_result.forwarded_store_contexts.end();
                if (!widen_partition) {
                    existing = std::find_if(
                        function_result.forwarded_store_contexts.begin(),
                        function_result.forwarded_store_contexts.end(),
                        [&](const auto& context) {
                            return same_target_kind(context) &&
                                   same_forwarded_store_shape(
                                       context.input, forwarded_input);
                        });
                }
                if (existing != function_result.forwarded_store_contexts.end()) {
                    const auto index = static_cast<std::size_t>(
                        std::distance(function_result.forwarded_store_contexts.begin(),
                                      existing));
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
                    const auto exemplar = std::find_if(
                        function_result.forwarded_store_contexts.begin(),
                        function_result.forwarded_store_contexts.end(),
                        [&](const auto& context) {
                            return context.target == target &&
                                   context.tail == tail &&
                                   context.isolated == isolated;
                        });
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
                        exemplar ==
                                function_result.forwarded_store_contexts.end()
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
            while (!function_result.pending_forwarded_store_contexts.empty()) {
                const auto index =
                    function_result.pending_forwarded_store_contexts.front();
                function_result.pending_forwarded_store_contexts.pop_front();
                function_result.forwarded_store_context_queued[index] = false;
                auto& context = function_result.forwarded_store_contexts[index];
                if (context.evaluated && !context.evaluation_dirty) continue;
                if (context.evaluation_count >=
                    maximum_forwarded_store_context_evaluations) {
                    function_result.walk_diagnostics
                        .forwarded_store_context_limited_functions = 1u;
                    record_forwarded_store_limit(
                        ForwardedStoreContextLimitReason::ReevaluationCount,
                        context.target,
                        context.tail,
                        context.isolated,
                        context.root_call_sites,
                        function_result.forwarded_store_contexts.size(),
                        context.root_call_sites.size(),
                        context.evaluation_count,
                        &context.input,
                        nullptr);
                    continue;
                }
                ++context.evaluation_count;
                const auto root_call_sites = context.root_call_sites;
                const TailIngressMap* local_tail_ingresses = nullptr;
                if (context.tail) {
                    const auto local =
                        final_inventory_region_tail_ingresses_by_entry.find(
                            context.target);
                    if (local !=
                        final_inventory_region_tail_ingresses_by_entry.end())
                        local_tail_ingresses = &local->second;
                }
                const auto [forwarded_evaluation, cache_hit] =
                    evaluate_forwarded_context(
                        context,
                        root_call_sites,
                        local_tail_ingresses);
                if (cache_hit) {
                    ++function_result.walk_diagnostics
                          .forwarded_store_evaluation_cache_hits;
                } else {
                    ++function_result.walk_diagnostics
                          .forwarded_store_evaluation_cache_misses;
                }
                forwarded_evaluation->inventory
                    .replay_deferred_copy_into(
                        function_result.inventory);
                merge_cached_forwarded_diagnostics(
                    function_result.walk_diagnostics,
                    forwarded_evaluation->walk_diagnostics);
                context.evaluated = true;
                context.evaluation_dirty = false;
                context.resolutions =
                    forwarded_evaluation->evaluation.resolutions;
                context.call_arguments =
                    forwarded_evaluation->evaluation.call_arguments;
                context.inventory_transfers =
                    forwarded_evaluation->evaluation
                        .inventory_transfers;
                const auto propagated_root_call_sites = context.root_call_sites;
                const auto context_isolated = context.isolated;
                const auto call_arguments = context.call_arguments;
                const auto inventory_transfers = context.inventory_transfers;
                for (const auto& forwarded : call_arguments)
                    enqueue_forwarded_call(forwarded,
                                           context_isolated,
                                           propagated_root_call_sites);
                for (const auto& forwarded : inventory_transfers)
                    enqueue_forwarded_tail(forwarded,
                                           context_isolated,
                                           propagated_root_call_sites);
            }
        };
        const auto harvest_contextual_candidate_returns = [&] {
            if (!candidate_call_owner_functions.contains(
                    function->entry_address))
                return;
            std::map<std::uint32_t, AbstractState> context_inputs;
            std::map<std::uint32_t, FunctionValueSummary>
                contextual_summaries;
            std::map<std::uint32_t, std::set<std::uint32_t>>
                context_callers;
            std::deque<std::uint32_t> pending_contexts;
            std::unordered_set<std::uint32_t> queued_contexts;
            std::unordered_set<std::uint32_t>
                candidate_context_functions;
            const auto enqueue_context =
                [&](const std::uint32_t address) {
                    if (queued_contexts.insert(address).second)
                        pending_contexts.push_back(address);
                };
            context_inputs.emplace(function->entry_address, input.state);
            enqueue_context(function->entry_address);
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
            while (!pending_contexts.empty() &&
                   !contextual_context_budget_exhausted &&
                   contextual_evaluations <
                       maximum_contextual_return_evaluations) {
                const auto address = pending_contexts.front();
                pending_contexts.pop_front();
                queued_contexts.erase(address);
                const auto context_function =
                    final_function_by_address.find(address);
                const auto context_input = context_inputs.find(address);
                if (context_function == final_function_by_address.end() ||
                    context_input == context_inputs.end())
                    continue;
                ++contextual_evaluations;
                auto context_evaluation = evaluate_function(
                    image,
                    *context_function->second,
                    block_index,
                    inventory_indirect_callees,
                    tail_ingresses,
                    summaries,
                    context_input->second,
                    ResolutionCollectionMode::None,
                    true,
                    nullptr,
                    nullptr,
                    &contextual_summaries,
                    nullptr,
                    nullptr,
                    &abi_stack_argument_reads);
                const auto previous =
                    contextual_summaries.find(address);
                const bool summary_changed =
                    previous == contextual_summaries.end() ||
                    previous->second != context_evaluation.summary;
                contextual_summaries[address] =
                    std::move(context_evaluation.summary);
                if (summary_changed) {
                    const auto callers = context_callers.find(address);
                    if (callers != context_callers.end()) {
                        for (const auto caller : callers->second)
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
                        candidate_context_functions.contains(address) &&
                        semantic_call_pairs.contains(pair) &&
                        has_contextual_candidate_abi_argument(
                            observation.state,
                            contextual_entry_register_reads(
                                observation.callee),
                            contextual_entry_stack_reads(
                                observation.callee)) &&
                        requires_contextual_return(observation.callee);
                    if (!candidate_call && !contextual_helper_call)
                        continue;
                    if (!final_function_by_address.contains(
                            observation.callee))
                        continue;
                    if (!context_inputs.contains(observation.callee) &&
                        context_inputs.size() >=
                            final_function_by_address.size()) {
                        function_result.walk_diagnostics.contextual_return_context_limited_functions = 1u;
                        emit_contextual_return_limit_diagnostic(
                            "contexts",
                            0u,
                            function->entry_address,
                            address,
                            observation.callee,
                            context_inputs.size(),
                            contextual_evaluations,
                            pending_contexts.size());
                        contextual_context_budget_exhausted = true;
                        break;
                    }
                    auto callee_context_input = observation.state;
                    if (candidate_call) {
                        mark_contextual_candidate_abi_arguments(
                            callee_context_input,
                            contextual_entry_register_reads(
                                observation.callee),
                            contextual_entry_stack_reads(
                                observation.callee));
                    }
                    context_callers[observation.callee].insert(address);
                    candidate_context_functions.insert(
                        observation.callee);
                    const auto [stored, inserted] =
                        context_inputs.try_emplace(
                            observation.callee,
                            callee_context_input);
                    if (inserted ||
                        merge_state(stored->second, callee_context_input))
                        enqueue_context(observation.callee);
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
            // result. Harvest every converged context exactly once instead.
            for (const auto& [address, context_input] :
                 context_inputs) {
                const auto context_function =
                    final_function_by_address.find(address);
                if (context_function == final_function_by_address.end())
                    continue;
                auto stable_evaluation = evaluate_function(
                    image,
                    *context_function->second,
                    block_index,
                    inventory_indirect_callees,
                    tail_ingresses,
                    summaries,
                    context_input,
                    ResolutionCollectionMode::GuardedInventory,
                    true,
                    &function_result.inventory,
                    nullptr,
                    &contextual_summaries,
                    nullptr,
                    &function_result.walk_diagnostics,
                    &abi_stack_argument_reads,
                    target_abi_inventory_sink_sources(
                        address));
                function_result.evaluation.resolutions.insert(
                    function_result.evaluation.resolutions.end(),
                    std::make_move_iterator(
                        stable_evaluation.resolutions.begin()),
                    std::make_move_iterator(
                        stable_evaluation.resolutions.end()));
                function_result.evaluation.call_arguments.insert(
                    function_result.evaluation.call_arguments.end(),
                    std::make_move_iterator(
                        stable_evaluation.call_arguments.begin()),
                    std::make_move_iterator(
                        stable_evaluation.call_arguments.end()));
                function_result.evaluation.inventory_transfers.insert(
                    function_result.evaluation.inventory_transfers.end(),
                    std::make_move_iterator(
                        stable_evaluation.inventory_transfers.begin()),
                    std::make_move_iterator(
                        stable_evaluation.inventory_transfers.end()));
            }
        };
        if (!result.budget_exhausted)
            harvest_contextual_candidate_returns();
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
            for (const auto& [call_site, observation] : input.observations) {
                if (!functions_with_guarded_abi_inventory_tail.contains(function->entry_address) &&
                    !reaches_indirect_dispatch &&
                    !requires_isolated_store_harvest(input, call_site, observation))
                    continue;
                const std::set<std::uint32_t> root_call_sites{call_site};
                const AbiStackArgumentReadSet* required_stack_reads = nullptr;
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
                auto isolated_evaluation =
                    evaluate_function(image,
                                      *function,
                                      block_index,
                                      inventory_indirect_callees,
                                      tail_ingresses,
                                      summaries,
                                      isolated_store_input(call_site,
                                                           observation,
                                                           required_register_reads,
                                                           true,
                                                           required_stack_reads),
                                      ResolutionCollectionMode::GuardedInventory,
                                      true,
                                      &function_result.inventory,
                                      &root_call_sites,
                                      nullptr,
                                      nullptr,
                                      &function_result.walk_diagnostics,
                                      &abi_stack_argument_reads,
                                      target_abi_inventory_sink_sources(
                                          function->entry_address));
                function_result.evaluation.resolutions.insert(
                    function_result.evaluation.resolutions.end(),
                    std::make_move_iterator(
                        isolated_evaluation.resolutions.begin()),
                    std::make_move_iterator(
                        isolated_evaluation.resolutions.end()));
                seed_forwarded_inventory(isolated_evaluation,
                                         true,
                                         root_call_sites);
                if (function_result.walk_diagnostics
                        .forwarded_store_context_limited_functions != 0u)
                    break;
            }
        }
        if (!result.budget_exhausted)
            drain_forwarded_inventory();
        for (auto& context : function_result.forwarded_store_contexts) {
            function_result.evaluation.resolutions.insert(
                function_result.evaluation.resolutions.end(),
                std::make_move_iterator(context.resolutions.begin()),
                std::make_move_iterator(context.resolutions.end()));
        }
        if (function_result.walk_diagnostics
                .abi_stack_argument_projection_truncated_functions != 0u)
            emit_abi_stack_projection_root_diagnostic(
                function->entry_address);
        return function_result;
    };

    std::vector<std::optional<ResolutionFunctionResult>> function_results(
        resolution_functions.size());
    if (resolution_functions.size() <
        minimum_parallel_resolution_functions) {
        for (std::size_t index = 0u; index < resolution_functions.size(); ++index)
            function_results[index].emplace(evaluate_resolution_function(index));
    } else {
        parallel_analysis_for(
            resolution_functions.size(),
            maximum_parallel_resolution_jobs,
            [&](const std::size_t index) {
                function_results[index].emplace(
                    evaluate_resolution_function(index));
            });
    }

    for (auto& function_result : function_results) {
        auto resolved = std::move(*function_result);

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
        resolution_count += resolved.evaluation.resolutions.size();
        result.resolutions.insert(
            result.resolutions.end(),
            std::make_move_iterator(resolved.evaluation.resolutions.begin()),
            std::make_move_iterator(resolved.evaluation.resolutions.end()));
        std::move(resolved.inventory).replay_into(guarded_inventory_collector);
        ++completed_functions;
        if (completed_functions <= 16u || completed_functions % 128u == 0u ||
            completed_functions == functions.size())
            report_progress("resolution-progress");
    }
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
    std::sort(result.resolutions.begin(),
              result.resolutions.end(),
              [](const auto& left, const auto& right) {
                  if (left.instruction_address != right.instruction_address)
                      return left.instruction_address < right.instruction_address;
                   if (left.call != right.call) return left.call < right.call;
                   return left.targets < right.targets;
               });

    std::vector<InterproceduralTargetResolution> merged;
    std::unordered_set<std::uint32_t> merged_context_sites;
    for (auto& resolution : result.resolutions) {
        if (merged.empty() || merged.back().instruction_address != resolution.instruction_address) {
            merged.push_back(std::move(resolution));
            continue;
        }
        auto& site = merged.back();
        merged_context_sites.insert(site.instruction_address);
        site.targets.insert(
            site.targets.end(), resolution.targets.begin(), resolution.targets.end());
        normalize(site.targets);
        site.call_sites.insert(
            site.call_sites.end(), resolution.call_sites.begin(), resolution.call_sites.end());
        normalize(site.call_sites);
        site.callees.insert(
            site.callees.end(), resolution.callees.begin(), resolution.callees.end());
        normalize(site.callees);
        site.complete = site.complete && resolution.complete;
        site.guarded = site.guarded || resolution.guarded || !resolution.complete;
    }
    for (auto& site : merged) {
        if (!merged_context_sites.contains(site.instruction_address)) continue;
        site.evidence = site.targets.empty() ? ControlFlowEvidence::Unresolved
                        : site.complete      ? (site.guarded ? ControlFlowEvidence::GuardedComplete
                                                             : ControlFlowEvidence::ProvenComplete)
                                             : ControlFlowEvidence::GuardedPartial;
        site.reason = site.targets.empty() ? "all-contexts-unknown"
                      : site.complete      ? "all-contexts-complete"
                                           : "merged-contexts-partial";
    }
    result.resolutions = std::move(merged);
    resolution_count = result.resolutions.size();
    report_progress("complete");
    return result;
}

} // namespace katana::analysis
