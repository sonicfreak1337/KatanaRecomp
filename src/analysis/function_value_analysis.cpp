#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"
#include "guarded_native_entry_shape.hpp"
#include "snapshot_pointer_candidates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
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

constexpr std::size_t maximum_summary_values = 8u;
constexpr std::size_t maximum_guarded_code_inventory = 1'024u;
constexpr std::size_t maximum_raw_stored_code_candidates =
    maximum_guarded_code_inventory * 4u;
constexpr std::size_t reserved_returned_table_targets = 256u;
constexpr std::size_t maximum_forwarded_store_contexts = 64u;
constexpr std::size_t maximum_inventory_regions = maximum_guarded_code_inventory;
constexpr std::size_t maximum_inventory_region_blocks = 256u;
constexpr std::size_t maximum_memory_values = 256u;
constexpr std::size_t maximum_fixpoint_iterations = 65'536u;
constexpr std::size_t maximum_parallel_resolution_jobs = 12u;
constexpr std::size_t minimum_parallel_resolution_functions = 64u;
constexpr std::int32_t maximum_stack_distance = 4'096;

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

struct AbstractValue {
    bool known = false;
    bool guarded = false;
    bool complete = false;
    // Inventory-only provenance: the value was derived from the architectural
    // stack pointer.  This is narrower than "may alias stack" and lets guarded
    // code-pointer inventory distinguish an actual stack spill from a store
    // through an otherwise unknown object or heap pointer.
    bool inventory_stack_derived = false;
    // Inventory-only evidence that the value crossed a native-code argument
    // boundary as a finite, decode-valid address.  Generic call-site
    // provenance is deliberately not strong enough for this purpose.
    bool inventory_code_pointer = false;
    std::vector<std::uint32_t> values;
    std::set<std::uint32_t> call_sites;
    std::set<std::uint32_t> callees;

    bool operator==(const AbstractValue&) const = default;
};

struct AbstractState {
    std::array<AbstractValue, 16u> registers;
    std::array<std::optional<std::int32_t>, 16u> stack_offsets;
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
    std::map<std::int32_t, AbstractValue> stack_values;
    std::map<std::uint32_t, AbstractValue> memory_values;

    AbstractState() {
        registers[15u].inventory_stack_derived = true;
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
};

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
        destination.raw_stored_candidates_truncated_ =
            destination.raw_stored_candidates_truncated_ ||
            raw_stored_candidates_truncated_;
        destination.table_scan_truncated_ =
            destination.table_scan_truncated_ || table_scan_truncated_;
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
    bool candidate_inventory_truncated_ = false;
    bool table_scan_truncated_ = false;
};

void make_unknown(AbstractValue& value) {
    value.known = false;
    value.guarded = false;
    value.complete = false;
    value.inventory_stack_derived = false;
    value.inventory_code_pointer = false;
    value.values.clear();
    value.call_sites.clear();
    value.callees.clear();
}

void make_unknown_preserving_provenance(AbstractValue& value) {
    const auto inventory_stack_derived = value.inventory_stack_derived;
    const auto inventory_code_pointer = value.inventory_code_pointer;
    auto call_sites = std::move(value.call_sites);
    auto callees = std::move(value.callees);
    make_unknown(value);
    value.inventory_stack_derived = inventory_stack_derived;
    value.inventory_code_pointer = inventory_code_pointer;
    value.call_sites = std::move(call_sites);
    value.callees = std::move(callees);
}

void set_value(AbstractValue& value, const std::uint32_t constant) {
    value.known = true;
    value.guarded = false;
    value.complete = true;
    value.inventory_stack_derived = false;
    value.inventory_code_pointer = false;
    value.values = {constant};
    value.call_sites.clear();
    value.callees.clear();
}

bool merge_value(AbstractValue& destination, const AbstractValue& source) {
    const bool inventory_stack_derived =
        destination.inventory_stack_derived || source.inventory_stack_derived;
    const bool inventory_code_pointer =
        destination.inventory_code_pointer || source.inventory_code_pointer;
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        const bool changed = destination.known || destination.guarded || destination.complete ||
                             destination.inventory_stack_derived !=
                                 inventory_stack_derived ||
                             destination.inventory_code_pointer !=
                                 inventory_code_pointer ||
                             !destination.values.empty() ||
                             call_sites != destination.call_sites ||
                             callees != destination.callees;
        destination.known = false;
        destination.guarded = false;
        destination.complete = false;
        destination.inventory_stack_derived = inventory_stack_derived;
        destination.inventory_code_pointer = inventory_code_pointer;
        destination.values.clear();
        destination.call_sites = std::move(call_sites);
        destination.callees = std::move(callees);
        return changed;
    }
    bool changed = false;
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
    if (destination.inventory_code_pointer != inventory_code_pointer) {
        destination.inventory_code_pointer = inventory_code_pointer;
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
    return changed;
}

bool merge_state(AbstractState& destination,
                 const AbstractState& source,
                 const bool may_merge_stack_values = false) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.size(); ++index)
        changed = merge_value(destination[index], source[index]) || changed;
    for (std::size_t index = 0u; index < destination.stack_offsets.size(); ++index) {
        const auto merged_may_alias =
            destination.stack_may_alias[index] || source.stack_may_alias[index];
        const auto merged_inventory_may_alias =
            destination.inventory_stack_may_alias[index] ||
            source.inventory_stack_may_alias[index];
        const auto merged_inventory_vbr_relative =
            destination.inventory_vbr_relative[index] &&
            source.inventory_vbr_relative[index];
        if (destination.stack_offsets[index] != source.stack_offsets[index]) {
            if (destination.stack_offsets[index].has_value()) changed = true;
            destination.stack_offsets[index].reset();
        }
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
    }
    for (auto slot = destination.stack_values.begin(); slot != destination.stack_values.end();) {
        const auto source_slot = source.stack_values.find(slot->first);
        if (source_slot == source.stack_values.end()) {
            if (may_merge_stack_values) {
                const auto guarded = true;
                const auto complete = false;
                changed = slot->second.guarded != guarded ||
                              slot->second.complete != complete ||
                          changed;
                slot->second.guarded = guarded;
                slot->second.complete = complete;
                ++slot;
            } else {
                slot = destination.stack_values.erase(slot);
                changed = true;
            }
            continue;
        }
        changed = merge_value(slot->second, source_slot->second) || changed;
        ++slot;
    }
    if (may_merge_stack_values) {
        for (const auto& [offset, value] : source.stack_values) {
            if (destination.stack_values.contains(offset)) continue;
            auto candidate = value;
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
            value = destination.memory_values.erase(value);
            changed = true;
            continue;
        }
        changed = merge_value(value->second, source_value->second) || changed;
        ++value;
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
        static_cast<void>(
            merge_state(merged.back().state, observation.state));
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
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
            state.inventory_vbr_relative[index] = false;
        }
    }
}

void apply_binary(AbstractValue& destination,
                  const AbstractValue& source,
                  const katana::sh4::InstructionKind kind) {
    const bool inventory_stack_derived =
        destination.inventory_stack_derived || source.inventory_stack_derived;
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        make_unknown(destination);
        destination.inventory_stack_derived = inventory_stack_derived;
        destination.call_sites = std::move(call_sites);
        destination.callees = std::move(callees);
        return;
    }
    destination.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
    destination.callees.insert(source.callees.begin(), source.callees.end());
    std::vector<std::uint32_t> values;
    for (const auto left : destination.values) {
        for (const auto right : source.values) {
            switch (kind) {
            case katana::sh4::InstructionKind::AddRegister:
                values.push_back(left + right);
                break;
            case katana::sh4::InstructionKind::SubRegister:
                values.push_back(left - right);
                break;
            case katana::sh4::InstructionKind::AndRegister:
                values.push_back(left & right);
                break;
            case katana::sh4::InstructionKind::OrRegister:
                values.push_back(left | right);
                break;
            case katana::sh4::InstructionKind::XorRegister:
                values.push_back(left ^ right);
                break;
            default:
                make_unknown(destination);
                destination.inventory_stack_derived = inventory_stack_derived;
                return;
            }
            if (values.size() > maximum_summary_values) {
                destination.inventory_stack_derived = inventory_stack_derived;
                make_unknown_preserving_provenance(destination);
                destination.inventory_code_pointer = false;
                return;
            }
        }
    }
    normalize(values);
    destination.values = std::move(values);
    destination.guarded = destination.guarded || source.guarded;
    destination.complete = destination.complete && source.complete;
    destination.inventory_stack_derived = inventory_stack_derived;
    destination.inventory_code_pointer = false;
}

template <typename Operation> void apply_unary(AbstractValue& value, Operation operation) {
    value.inventory_code_pointer = false;
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
    const auto* segment = image.find_segment(address, width);
    if (segment == nullptr || !segment->permissions.readable) return std::nullopt;
    const auto offset = segment->byte_offset(address);
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
        value = image.read_u32_le(address);
        break;
    default:
        return std::nullopt;
    }
    return ImageValue{value, segment->permissions.writable};
}

void load_memory_values(AbstractValue& destination,
                        const AbstractState& state,
                        const std::vector<std::uint32_t>& addresses,
                        const std::size_t width,
                        const katana::io::ExecutableImage& image,
                        const AbstractValue* address_evidence = nullptr) {
    if (addresses.empty()) {
        make_unknown(destination);
        return;
    }
    AbstractValue loaded;
    bool first = true;
    for (const auto address : addresses) {
        AbstractValue value;
        if (width == 4u) {
            const auto forwarded = state.memory_values.find(address);
            if (forwarded != state.memory_values.end()) {
                value = forwarded->second;
                value.guarded = true;
            }
        }
        if (!value.known) {
            const auto image_value = read_image_value(image, address, width);
            if (!image_value.has_value()) {
                make_unknown(destination);
                return;
            }
            set_value(value, image_value->value);
            value.guarded = image_value->guarded;
            value.complete = !image_value->guarded;
        }
        if (first) {
            loaded = std::move(value);
            first = false;
        } else if (merge_value(loaded, value) && !loaded.known) {
            make_unknown(destination);
            return;
        }
    }
    if (address_evidence != nullptr) {
        loaded.guarded = loaded.guarded || address_evidence->guarded;
        loaded.complete = loaded.complete && address_evidence->complete;
        // The address selects where a value is loaded from; it is not
        // provenance for the loaded contents.  Code-pointer argument
        // provenance is attached only when that value itself crosses a
        // proven ABI boundary.
    }
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

void store_memory_values(AbstractState& state,
                         const std::vector<std::uint32_t>& addresses,
                         const std::size_t width,
                         const AbstractValue& value,
                         const AbstractValue& address_evidence) {
    if (!address_evidence.known || !address_evidence.complete || addresses.empty()) {
        state.memory_values.clear();
        return;
    }
    if (std::any_of(addresses.begin(), addresses.end(), [width](const auto address) {
            return width == 0u ||
                   address > std::numeric_limits<std::uint32_t>::max() - (width - 1u);
        })) {
        state.memory_values.clear();
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
    if (width != 4u || !value.known) return;
    auto stored = value;
    stored.guarded = true;
    stored.complete = stored.complete && address_evidence.complete;
    // Destination-address provenance must not become provenance of the
    // stored contents.  The source value already carries any legitimate
    // code-pointer evidence.
    for (const auto address : addresses)
        state.memory_values[address] = stored;
    if (state.memory_values.size() > maximum_memory_values) state.memory_values.clear();
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
            state.stack_values.clear();
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

void store_stack_value(AbstractState& state,
                       const std::optional<std::int32_t> offset,
                       const bool may_alias_stack,
                       const bool preserve_guarded_inventory,
                       const std::size_t width,
                       const AbstractValue& value) {
    invalidate_stack_range(
        state, offset, may_alias_stack, preserve_guarded_inventory, width);
    if (offset.has_value() && width == 4u && value.known) state.stack_values[*offset] = value;
}

void load_stack_value(AbstractValue& destination,
                      const AbstractState& state,
                      const std::optional<std::int32_t> offset,
                      const std::size_t width) {
    if (!offset.has_value() || width != 4u) {
        make_unknown(destination);
        return;
    }
    const auto value = state.stack_values.find(*offset);
    if (value == state.stack_values.end()) {
        make_unknown(destination);
        return;
    }
    destination = value->second;
}

void adjust_stack_offset(AbstractState& state,
                         const std::uint8_t register_index,
                         const std::int32_t delta) {
    state[register_index].inventory_code_pointer = false;
    if (state.stack_offsets[register_index].has_value()) {
        const auto adjusted =
            static_cast<std::int64_t>(*state.stack_offsets[register_index]) + delta;
        if (adjusted < -maximum_stack_distance || adjusted > maximum_stack_distance)
            state.stack_offsets[register_index].reset();
        else
            state.stack_offsets[register_index] = static_cast<std::int32_t>(adjusted);
    }
    if (state[register_index].known) {
        for (auto& value : state[register_index].values)
            value += static_cast<std::uint32_t>(delta);
        normalize(state[register_index].values);
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
    const auto& instruction = line.instruction;
    const auto incoming_source_vbr_relative =
        state.inventory_vbr_relative[instruction.source_register];
    const auto incoming_destination_vbr_relative =
        state.inventory_vbr_relative[instruction.destination_register];
    const auto written_registers = general_register_write_mask(instruction);
    for (std::uint8_t index = 0u; index < state.size(); ++index) {
        if ((written_registers & register_bit(index)) != 0u)
            state.inventory_vbr_relative[index] = false;
    }
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
        state.stack_may_alias[instruction.destination_register] = false;
        state.inventory_stack_may_alias[instruction.destination_register] = false;
        return;
    case katana::sh4::InstructionKind::MovRegister:
        state[instruction.destination_register] = state[instruction.source_register];
        state.stack_offsets[instruction.destination_register] =
            state.stack_offsets[instruction.source_register];
        state.stack_may_alias[instruction.destination_register] =
            state.stack_may_alias[instruction.source_register];
        state.inventory_stack_may_alias[instruction.destination_register] =
            state.inventory_stack_may_alias[instruction.source_register];
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_source_vbr_relative;
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        adjust_stack_offset(state, instruction.destination_register, instruction.immediate);
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        return;
    case katana::sh4::InstructionKind::AddRegister:
    case katana::sh4::InstructionKind::SubRegister:
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister: {
        std::optional<std::int32_t> adjusted_stack_offset;
        const auto& source = state[instruction.source_register];
        if (preserve_guarded_stack_inventory &&
            instruction.kind == katana::sh4::InstructionKind::AddRegister &&
            state.stack_offsets[instruction.destination_register].has_value() &&
            !state.stack_offsets[instruction.source_register].has_value() &&
            !state.stack_may_alias[instruction.source_register] &&
            !state.inventory_stack_may_alias[instruction.source_register] &&
            source.known && source.values.size() == 1u) {
            // This branch is exclusive to the guarded inventory walk.  A
            // writable literal is runtime-authoritative and therefore
            // incomplete, but its captured finite delta is still useful for
            // following a guarded stack spill/reload.  Any resulting code
            // address remains a candidate; this never creates a fixed CFG
            // edge or a runtime fallback.
            auto delta = static_cast<std::int64_t>(source.values.front());
            if (delta > std::numeric_limits<std::int32_t>::max())
                delta -= (std::int64_t{1} << 32u);
            const auto adjusted =
                static_cast<std::int64_t>(
                    *state.stack_offsets[instruction.destination_register]) +
                delta;
            if (adjusted >= -maximum_stack_distance &&
                adjusted <= maximum_stack_distance)
                adjusted_stack_offset =
                    static_cast<std::int32_t>(adjusted);
        }
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
        return;
    }
    case katana::sh4::InstructionKind::AndImmediate:
    case katana::sh4::InstructionKind::OrImmediate:
    case katana::sh4::InstructionKind::XorImmediate: {
        const auto immediate = static_cast<std::uint32_t>(instruction.immediate);
        apply_unary(state[0u], [&](const std::uint32_t value) {
            return instruction.kind == katana::sh4::InstructionKind::AndImmediate
                       ? value & immediate
                   : instruction.kind == katana::sh4::InstructionKind::OrImmediate
                       ? value | immediate
                       : value ^ immediate;
        });
        state.stack_offsets[0u].reset();
        return;
    }
    case katana::sh4::InstructionKind::ShiftLogicalLeftOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 1u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftTwo:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 2u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftEight:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 8u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalLeftSixteen:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 16u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 1u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightTwo:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 2u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightEight:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 8u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftLogicalRightSixteen:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value >> 16u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftArithmeticLeftOne:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value << 1u; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ShiftArithmeticRightOne:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> 1);
        });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ExtendUnsignedByte:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value & 0xFFu; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ExtendUnsignedWord:
        apply_unary(state[instruction.destination_register],
                    [](const std::uint32_t value) { return value & 0xFFFFu; });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ExtendSignedByte:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
        });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::ExtendSignedWord:
        apply_unary(state[instruction.destination_register], [](const std::uint32_t value) {
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
        });
        state.stack_offsets[instruction.destination_register].reset();
        return;
    case katana::sh4::InstructionKind::MoveT:
        state[instruction.destination_register].known = true;
        state[instruction.destination_register].guarded = false;
        state[instruction.destination_register].complete = true;
        state[instruction.destination_register].inventory_stack_derived = false;
        state[instruction.destination_register].inventory_code_pointer = false;
        state[instruction.destination_register].values = {0u, 1u};
        state[instruction.destination_register].call_sites.clear();
        state[instruction.destination_register].callees.clear();
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] = false;
        state.inventory_stack_may_alias[instruction.destination_register] = false;
        return;
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_value(state[0u],
                  ((line.address + 4u) & ~3u) +
                      static_cast<std::uint32_t>(instruction.displacement));
        state.stack_offsets[0u].reset();
        state.stack_may_alias[0u] = false;
        state.inventory_stack_may_alias[0u] = false;
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
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] =
            !state[instruction.destination_register].known;
        state.inventory_stack_may_alias[instruction.destination_register] =
            !state[instruction.destination_register].known;
        return;
    }
    case katana::sh4::InstructionKind::MovByteStore:
    case katana::sh4::InstructionKind::MovWordStore:
    case katana::sh4::InstructionKind::MovLongStore: {
        const auto width = instruction.kind == katana::sh4::InstructionKind::MovByteStore   ? 1u
                           : instruction.kind == katana::sh4::InstructionKind::MovWordStore ? 2u
                                                                                            : 4u;
        const auto offset = stack_slot(state, instruction.destination_register);
        store_stack_value(state,
                          offset,
                          state.stack_may_alias[instruction.destination_register],
                          preserve_guarded_stack_inventory,
                          width,
                          state[instruction.source_register]);
        if (!offset.has_value()) {
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                width,
                                state[instruction.source_register],
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
        const auto offset = stack_slot(state, instruction.destination_register);
        store_stack_value(state,
                          offset,
                          state.stack_may_alias[instruction.destination_register],
                          preserve_guarded_stack_inventory,
                          width,
                          state[instruction.source_register]);
        if (!offset.has_value()) {
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                width,
                                state[instruction.source_register],
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
        store_stack_value(state,
                          offset,
                          state.stack_may_alias[instruction.destination_register],
                          preserve_guarded_stack_inventory,
                          width,
                          state[instruction.source_register]);
        if (!offset.has_value()) {
            store_memory_values(
                state,
                displaced_addresses(state[instruction.destination_register],
                                    static_cast<std::uint32_t>(instruction.displacement)),
                width,
                state[instruction.source_register],
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
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(state[instruction.destination_register],
                             state,
                             stack_slot(state, instruction.source_register),
                             width);
        } else {
            load_memory_values(state[instruction.destination_register],
                               state,
                               displaced_addresses(state[instruction.source_register], 0u),
                               width,
                               image,
                               &state[instruction.source_register]);
        }
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadPostIncrement:
    case katana::sh4::InstructionKind::MovWordLoadPostIncrement:
    case katana::sh4::InstructionKind::MovLongLoadPostIncrement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadPostIncrement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadPostIncrement ? 2u
                                                                                         : 4u;
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(state[instruction.destination_register],
                             state,
                             stack_slot(state, instruction.source_register),
                             width);
        } else {
            load_memory_values(state[instruction.destination_register],
                               state,
                               displaced_addresses(state[instruction.source_register], 0u),
                               width,
                               image,
                               &state[instruction.source_register]);
        }
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        if (instruction.source_register != instruction.destination_register) {
            adjust_stack_offset(
                state, instruction.source_register, static_cast<std::int32_t>(width));
            state.inventory_vbr_relative[instruction.source_register] =
                incoming_source_vbr_relative;
        }
        return;
    }
    case katana::sh4::InstructionKind::MovByteLoadDisplacement:
    case katana::sh4::InstructionKind::MovWordLoadDisplacement:
    case katana::sh4::InstructionKind::MovLongLoadDisplacement: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadDisplacement   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadDisplacement ? 2u
                                                                                        : 4u;
        if (state.stack_offsets[instruction.source_register].has_value()) {
            load_stack_value(
                state[instruction.destination_register],
                state,
                stack_slot(state, instruction.source_register, instruction.displacement),
                width);
        } else {
            load_memory_values(
                state[instruction.destination_register],
                state,
                displaced_addresses(state[instruction.source_register],
                                    static_cast<std::uint32_t>(instruction.displacement)),
                width,
                image,
                &state[instruction.source_register]);
        }
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreR0Indexed:
    case katana::sh4::InstructionKind::MovWordStoreR0Indexed:
    case katana::sh4::InstructionKind::MovLongStoreR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteStoreR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordStoreR0Indexed ? 2u
                                                                                      : 4u;
        if (state.stack_may_alias[0u] ||
            state.stack_may_alias[instruction.destination_register]) {
            if (preserve_guarded_stack_inventory) {
                for (auto& [stack_offset, value] : state.stack_values) {
                    static_cast<void>(stack_offset);
                    value.guarded = true;
                    value.complete = false;
                }
            } else {
                state.stack_values.clear();
            }
        }
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
        store_memory_values(state,
                            indexed_addresses(state[0u],
                                              state[instruction.destination_register],
                                              instruction.destination_register == 0u),
                            width,
                            state[instruction.source_register],
                            evidence);
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreGbrDisplacement:
    case katana::sh4::InstructionKind::MovWordStoreGbrDisplacement:
    case katana::sh4::InstructionKind::MovLongStoreGbrDisplacement:
    case katana::sh4::InstructionKind::AndByteImmediate:
    case katana::sh4::InstructionKind::XorByteImmediate:
    case katana::sh4::InstructionKind::OrByteImmediate:
    case katana::sh4::InstructionKind::TestAndSetByte:
    case katana::sh4::InstructionKind::FmovStore:
    case katana::sh4::InstructionKind::FmovStorePreDecrement:
    case katana::sh4::InstructionKind::FmovStoreR0Indexed:
    case katana::sh4::InstructionKind::Prefetch:
    case katana::sh4::InstructionKind::TrapAlways:
        state.stack_values.clear();
        state.memory_values.clear();
        clear_written(state, instruction);
        return;
    case katana::sh4::InstructionKind::StoreSpecialRegisterPreDecrement: {
        adjust_stack_offset(state, instruction.destination_register, -4);
        state.inventory_vbr_relative[instruction.destination_register] =
            incoming_destination_vbr_relative;
        const auto offset = stack_slot(state, instruction.destination_register);
        invalidate_stack_range(state,
                               offset,
                               state.stack_may_alias[instruction.destination_register],
                               preserve_guarded_stack_inventory,
                               4u);
        if (!offset.has_value()) {
            AbstractValue unknown;
            store_memory_values(state,
                                displaced_addresses(state[instruction.destination_register], 0u),
                                4u,
                                unknown,
                                state[instruction.destination_register]);
        }
        return;
    }
    case katana::sh4::InstructionKind::LoadSpecialRegisterPostIncrement:
        adjust_stack_offset(state, instruction.source_register, 4);
        state.inventory_vbr_relative[instruction.source_register] =
            incoming_source_vbr_relative;
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
        return;
    case katana::sh4::InstructionKind::MovByteLoadR0Indexed:
    case katana::sh4::InstructionKind::MovWordLoadR0Indexed:
    case katana::sh4::InstructionKind::MovLongLoadR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteLoadR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordLoadR0Indexed ? 2u
                                                                                     : 4u;
        auto evidence = state[0u];
        evidence.guarded = evidence.guarded || state[instruction.source_register].guarded;
        evidence.complete = evidence.complete && state[instruction.source_register].complete;
        evidence.call_sites.insert(state[instruction.source_register].call_sites.begin(),
                                   state[instruction.source_register].call_sites.end());
        evidence.callees.insert(state[instruction.source_register].callees.begin(),
                                state[instruction.source_register].callees.end());
        load_memory_values(state[instruction.destination_register],
                           state,
                           indexed_addresses(state[0u],
                                             state[instruction.source_register],
                                             instruction.source_register == 0u),
                           width,
                           image,
                           &evidence);
        state.stack_offsets[instruction.destination_register].reset();
        state.stack_may_alias[instruction.destination_register] = true;
        state.inventory_stack_may_alias[instruction.destination_register] = true;
        return;
    }
    default:
        if (instruction.kind == katana::sh4::InstructionKind::Unknown) {
            state.stack_values.clear();
            state.memory_values.clear();
        }
        clear_written(state, instruction);
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

void observe_callee_arguments(
    const katana::io::ExecutableImage& image,
    const AbstractState& state,
    const std::uint32_t call_site,
    const std::span<const std::uint32_t> candidate_callees,
    const bool candidate_callees_guarded,
    std::vector<FunctionEvaluation::CallArguments>* const call_arguments) {
    if (call_arguments == nullptr || candidate_callees.empty()) return;
    AbstractState observation;
    observation.registers = state.registers;
    observation.stack_offsets = state.stack_offsets;
    observation.stack_may_alias = state.stack_may_alias;
    observation.inventory_stack_may_alias = state.inventory_stack_may_alias;
    observation.inventory_vbr_relative = state.inventory_vbr_relative;
    observation.memory_values = state.memory_values;
    // This provenance belongs to the value passed in an ABI argument register,
    // not to an address used to load that value.  Loads deliberately do not
    // inherit it from their address operand.
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        auto& value = observation[index];
        if (!value.known || value.values.empty() ||
            value.values.size() > maximum_summary_values ||
            !std::ranges::all_of(value.values, [&](const auto candidate) {
                return validate_decode_candidate(image, candidate).valid();
            }))
            continue;
        value.inventory_code_pointer = true;
    }
    // Caller-local stack slots are not part of callee ingress.  Keeping
    // them in observations only deep-copied irrelevant state and could
    // requeue a callee when the effective merged input was unchanged.
    if (candidate_callees_guarded) {
        for (auto& value : observation)
            value.guarded = true;
        for (auto& [address, value] : observation.memory_values) {
            static_cast<void>(address);
            value.guarded = true;
        }
    }
    for (std::size_t index = 0u; index < candidate_callees.size(); ++index) {
        const auto candidate = candidate_callees[index];
        if (index + 1u == candidate_callees.size())
            call_arguments->push_back({call_site, candidate, std::move(observation)});
        else
            call_arguments->push_back({call_site, candidate, observation});
    }
}

void observe_inventory_transfers(
    const katana::io::ExecutableImage& image,
    const AbstractState& state,
    const std::uint32_t transfer_site,
    const std::span<const std::uint32_t> candidate_callees,
    const bool guarded,
    const bool complete,
    const bool requires_code_pointer,
    std::vector<FunctionEvaluation::InventoryTransfer>* const transfers) {
    if (transfers == nullptr || candidate_callees.empty()) return;
    auto observation = state;
    bool found_code_pointer = false;
    const auto observe_code_pointer = [&](const AbstractValue& value) {
        if (!value.inventory_code_pointer || !value.known ||
            value.values.empty() ||
            value.values.size() > maximum_summary_values ||
            !std::ranges::all_of(value.values, [&](const auto candidate) {
                return validate_decode_candidate(image, candidate).valid();
            }))
            return;
        found_code_pointer = true;
    };
    for (const auto& value : observation)
        observe_code_pointer(value);
    for (const auto& [offset, value] : observation.stack_values) {
        static_cast<void>(offset);
        observe_code_pointer(value);
    }
    for (const auto& [address, value] : observation.memory_values) {
        static_cast<void>(address);
        observe_code_pointer(value);
    }
    if (requires_code_pointer && !found_code_pointer) return;
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
    for (std::size_t index = 0u; index < candidate_callees.size(); ++index) {
        const auto candidate = candidate_callees[index];
        if (index + 1u == candidate_callees.size())
            transfers->push_back({transfer_site,
                                  candidate,
                                  std::move(observation),
                                  guarded,
                                  complete});
        else
            transfers->push_back(
                {transfer_site, candidate, observation, guarded, complete});
    }
}

void apply_call(AbstractState& state,
                const katana::io::ExecutableImage& image,
                const std::uint32_t call_site,
                const std::optional<std::uint32_t> callee,
                const std::span<const std::uint32_t> candidate_callees,
                const bool candidate_callees_guarded,
                const bool candidate_callees_complete,
                const std::map<std::uint32_t, FunctionValueSummary>& summaries,
                std::vector<FunctionEvaluation::CallArguments>* call_arguments,
                const bool preserve_guarded_stack_inventory = false,
                const std::map<std::uint32_t, FunctionValueSummary>*
                    contextual_summaries = nullptr) {
    observe_callee_arguments(image,
                             state,
                             call_site,
                             candidate_callees,
                             candidate_callees_guarded,
                             call_arguments);
    if (image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC) {
        for (std::size_t index = 0u; index < state.size(); ++index) {
            make_unknown(state[index]);
            state.stack_offsets[index].reset();
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
            state.inventory_vbr_relative[index] = false;
        }
        state[15u].inventory_stack_derived = true;
        state.stack_values.clear();
        state.memory_values.clear();
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
            state.stack_values.clear();
        }
    }
    state.memory_values.clear();
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        make_unknown(state[index]);
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.stack_offsets[index].reset();
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.stack_may_alias[index] = true;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.inventory_stack_may_alias[index] = true;
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.inventory_vbr_relative[index] = false;
    make_unknown(state[15u]);
    state[15u].inventory_stack_derived = true;
    state.inventory_vbr_relative[15u] = false;
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
    bool returned_memory_complete = candidate_callees_complete;
    bool returned_memory_initialized = false;
    std::map<std::uint32_t, AbstractValue> returned_memory;
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
                value.call_sites = {call_site};
                value.callees = {candidate};
                if (value.known) candidate_memory.emplace(memory.address, std::move(value));
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
                    if (!value->second.known)
                        value = returned_memory.erase(value);
                    else
                        ++value;
                }
            }
        }
        const auto* returned = register_summary(*summary, 0u);
        if (returned == nullptr || returned->values.empty()) {
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
        evidence_callees.insert(candidate);
        evidence_callees.insert(returned->evidence_callees.begin(),
                                returned->evidence_callees.end());
    }
    if (returned_memory_complete && returned_memory_initialized)
        state.memory_values = std::move(returned_memory);
    normalize(returned_values);
    if (returned_values.empty() || returned_values.size() > maximum_summary_values) return;
    state[0u].known = true;
    state[0u].guarded = returned_guarded || !returned_complete;
    state[0u].complete = returned_complete;
    state[0u].values = std::move(returned_values);
    state[0u].call_sites = {call_site};
    state[0u].callees = std::move(evidence_callees);
    state.stack_may_alias[0u] = returned_may_alias_stack;
    state.inventory_stack_may_alias[0u] =
        returned_inventory_may_alias_stack;
    state.inventory_vbr_relative[0u] = false;
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

void observe_stored_code_addresses(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& line,
    const AbstractState& state,
    const bool allow_forwarded_unknown_object_store,
    GuardedCodeInventoryCollector& guarded_inventory_collector,
    std::vector<StoredCodeAddressCandidate>& candidates) {
    using K = katana::sh4::InstructionKind;
    const auto& instruction = line.instruction;
    bool supported = false;
    bool stack_based = false;
    bool stack_derived = false;
    bool vbr_relative_destination = false;
    bool destination_proven_non_stack = false;
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
        value.inventory_code_pointer && finite_value(value);
    if (stack_based && !forwarded_code_pointer_store) return;

    include_provenance(value);
    if (!finite_value(value))
        return;
    const bool finite_resolved_non_stack_destination =
        !vbr_relative_destination && destination_proven_non_stack &&
        finite_effective_address(effective_destinations) &&
        std::ranges::all_of(
            effective_destinations,
            [&](const auto address) {
                return image.resolve_segment_address(address, 4u).has_value();
            });
    const bool direct_code_pointer_provenance =
        !evidence_call_sites.empty() || vbr_relative_destination;
    if (!direct_code_pointer_provenance &&
        !finite_resolved_non_stack_destination)
        return;

    std::vector<std::uint32_t> validated_candidates;
    validated_candidates.reserve(value.values.size());
    bool all_candidates_valid = true;
    for (const auto candidate : value.values) {
        const auto validation = validate_decode_candidate(image, candidate);
        const bool scan_stored_table =
            finite_resolved_non_stack_destination &&
            (!direct_code_pointer_provenance || !validation.valid());
        if (scan_stored_table) {
            const auto& table =
                guarded_inventory_collector.stored_snapshot_table(
                    image, line.address, candidate);
            if (!table.has_value()) {
                if (!validation.valid()) all_candidates_valid = false;
                continue;
            }
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
            if (!direct_code_pointer_provenance) continue;
        }
        if (!validation.valid()) {
            all_candidates_valid = false;
            continue;
        }
        if (direct_code_pointer_provenance)
            validated_candidates.push_back(validation.resolved_address);
    }
    normalize(validated_candidates);
    const bool complete = value.complete && all_candidates_valid;
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
    AbstractValue effective_address;
    switch (line.instruction.kind) {
    case K::MovLongLoad:
    case K::MovLongLoadPostIncrement: {
        const auto base_register = line.instruction.source_register;
        if (state.inventory_stack_may_alias[base_register]) return;
        effective_address = state[base_register];
        break;
    }
    case K::MovLongLoadDisplacement: {
        const auto base_register = line.instruction.source_register;
        if (state.inventory_stack_may_alias[base_register]) return;
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
        if (state.inventory_stack_may_alias[0u] ||
            state.inventory_stack_may_alias[base_register])
            return;
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

FunctionEvaluation evaluate_function(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>& indirect_callees,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>& tail_ingresses,
    const std::map<std::uint32_t, FunctionValueSummary>& summaries,
    const AbstractState& initial_state,
    const bool collect_resolutions,
    const bool may_merge_stack_inventory = false,
    GuardedCodeInventoryCollector* const guarded_inventory_collector = nullptr,
    const std::optional<std::uint32_t> isolated_inventory_call_site =
        std::nullopt,
    const std::map<std::uint32_t, FunctionValueSummary>*
        contextual_summaries = nullptr) {
    FunctionEvaluation evaluation;
    evaluation.summary.function_address = function.entry_address;
    if (!collect_resolutions)
        evaluation.call_arguments.reserve(function.direct_callees.size() +
                                          function.tail_jump_targets.size());
    // Final resolution still needs inventory-only ABI observations when a
    // collector is active.  Without them, a locally computed code pointer
    // passed through a candidate-only call cannot enter the bounded forwarded
    // store walk, even though no semantic call edge is being asserted.
    auto* const call_arguments =
        collect_resolutions && guarded_inventory_collector == nullptr
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

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        const auto block = blocks.find(address);
        if (block == blocks.end()) continue;
        auto state = inputs.at(address);
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
        };
        std::optional<DelayedTailIngress> delayed_tail_ingress;
        for (const auto& line : block->second->lines) {
            const bool indirect = line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
            if (collect_resolutions && indirect) {
                const auto& value = state[line.instruction.branch_register];
                InterproceduralTargetResolution resolution;
                resolution.instruction_address = line.address;
                resolution.register_index = line.instruction.branch_register;
                resolution.call = line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
                if (value.known && !value.values.empty()) {
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
                if (resolution.targets.empty()) {
                    resolution.guarded = true;
                    resolution.complete = false;
                    resolution.evidence = ControlFlowEvidence::Unresolved;
                    resolution.reason = "context-target-unknown";
                }
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
                    stored_candidates);
                if (isolated_inventory_call_site.has_value()) {
                    for (auto& candidate : stored_candidates) {
                        candidate.complete = false;
                        candidate.guarded = true;
                        candidate.evidence_call_sites.push_back(
                            *isolated_inventory_call_site);
                    }
                }
                guarded_inventory_collector->collect(
                    std::move(stored_candidates));
                if (!isolated_inventory_call_site.has_value()) {
                    std::vector<ReturnedCodeAddressTableCandidate>
                        returned_tables;
                    observe_returned_code_address_tables(
                        image, line, state, returned_tables);
                    guarded_inventory_collector->collect(
                        std::move(returned_tables));
                }
            }
            if (!call)
                apply_transfer(
                    state, line, image, may_merge_stack_inventory);
            if (delayed_call.has_value()) {
                apply_call(state,
                           image,
                           delayed_call->call_site,
                           delayed_call->direct_callee,
                           delayed_call->candidate_callees,
                           delayed_call->candidate_callees_guarded,
                           delayed_call->candidate_callees_complete,
                           summaries,
                           call_arguments,
                           may_merge_stack_inventory,
                           contextual_summaries);
                delayed_call.reset();
            }
            if (delayed_tail_ingress.has_value()) {
                observe_inventory_transfers(
                    image,
                    state,
                    delayed_tail_ingress->transfer_site,
                    delayed_tail_ingress->candidate_callees,
                    delayed_tail_ingress->candidate_callees_guarded,
                    delayed_tail_ingress->candidate_callees_complete,
                    delayed_tail_ingress->requires_code_pointer,
                    inventory_transfers);
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
                               line.address,
                               callee,
                               candidate_callees,
                               candidate_callees_guarded,
                               candidate_callees_complete,
                               summaries,
                               call_arguments,
                               may_merge_stack_inventory,
                               contextual_summaries);
            }
            if (!call &&
                (line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::UnconditionalBranch ||
                 line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::IndirectBranch ||
                 line.instruction.control_flow ==
                     katana::sh4::ControlFlowKind::ConditionalBranch)) {
                const auto tail_ingress = tail_ingresses.find(line.address);
                if (tail_ingress == tail_ingresses.end()) continue;
                if (line.instruction.has_delay_slot) {
                    delayed_tail_ingress = DelayedTailIngress{
                        line.address,
                        tail_ingress->second.targets,
                        tail_ingress->second.guarded,
                        tail_ingress->second.complete,
                        tail_ingress->second.requires_code_pointer};
                } else {
                    observe_inventory_transfers(image,
                                                state,
                                                line.address,
                                                tail_ingress->second.targets,
                                                tail_ingress->second.guarded,
                                                tail_ingress->second.complete,
                                                tail_ingress->second.requires_code_pointer,
                                                inventory_transfers);
                }
            }
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

    // A block can be revisited while its local input converges.  Publish one
    // conservative observation per physical callsite/callee pair; exposing
    // transient visits to the interprocedural worklist lets the same callsite
    // alternately replace its callee input and can keep a recursive graph
    // alive long after the local state has stabilized.
    coalesce_call_arguments(evaluation.call_arguments);

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
        std::set<std::uint32_t> values;
        std::set<std::uint32_t> evidence;
        for (const auto& [return_site, state] : returns) {
            static_cast<void>(return_site);
            const auto& value = state[register_index];
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
        if (finite) summary.values.assign(values.begin(), values.end());
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
            memory.complete = value.complete;
            memory.guarded = value.guarded;
            memory.values = std::move(value.values);
            evaluation.summary.memory_values.push_back(std::move(memory));
        }
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

bool merge_candidate_input(CandidateInput& destination,
                           const FunctionEvaluation::CallArguments& observation) {
    const auto [stored, inserted] =
        destination.observations.try_emplace(observation.call_site, observation.state);
    if (!inserted) {
        if (stored->second == observation.state) return false;
        stored->second = observation.state;
    }
    AbstractState merged;
    merged.stack_offsets[15u] = 0;
    if (destination.unknown_ingress || destination.expected_call_sites.empty() ||
        !std::all_of(
            destination.expected_call_sites.begin(),
            destination.expected_call_sites.end(),
            [&](const auto call_site) { return destination.observations.contains(call_site); })) {
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
        bool may_alias_stack = false;
        bool inventory_may_alias_stack = false;
        bool inventory_stack_derived = false;
        bool inventory_code_pointer = false;
        bool inventory_vbr_relative = true;
        std::optional<std::int32_t> stack_offset;
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
                inventory_stack_derived || source.inventory_stack_derived;
            inventory_code_pointer =
                inventory_code_pointer || source.inventory_code_pointer;
            inventory_vbr_relative =
                inventory_vbr_relative &&
                call_observation.inventory_vbr_relative[index];
            const auto rebased_stack_offset =
                callee_relative_stack_offset(call_observation, index);
            if (first_stack_provenance) {
                stack_offset = rebased_stack_offset;
                first_stack_provenance = false;
            } else if (stack_offset != rebased_stack_offset) {
                exact_stack_provenance = false;
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
        target.inventory_code_pointer = inventory_code_pointer;
        target.call_sites = std::move(call_sites);
        target.callees = std::move(callees);
        merged.stack_may_alias[index] = may_alias_stack;
        merged.inventory_stack_may_alias[index] =
            inventory_may_alias_stack;
        merged.inventory_vbr_relative[index] =
            inventory_vbr_relative;
        if (may_alias_stack && exact_stack_provenance)
            merged.stack_offsets[index] = stack_offset;
        else
            merged.stack_offsets[index].reset();
    }
    const auto first_call_site = *destination.expected_call_sites.begin();
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
            if (!value->second.known) {
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
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        const auto& observed = observation[index];
        const auto& merged = input.state[index];
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

bool function_contains_guarded_inventory_store(
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
                        line.instruction.kind);
                }))
            return true;
    }
    return false;
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

AbstractState isolated_store_input(const std::uint32_t call_site,
                                   const AbstractState& observation) {
    AbstractState input;
    input.stack_offsets[15u] = 0;
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        input[index] = observation[index];
        input[index].complete = false;
        input[index].guarded = input[index].known;
        input[index].call_sites.insert(call_site);
        input.stack_offsets[index] =
            callee_relative_stack_offset(observation, index);
        input.stack_may_alias[index] = observation.stack_may_alias[index];
        input.inventory_stack_may_alias[index] =
            observation.inventory_stack_may_alias[index];
        input.inventory_vbr_relative[index] =
            observation.inventory_vbr_relative[index];
        if (input[index].values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(input[index]);
    }
    return input;
}

AbstractState tail_store_input(const AbstractState& observation) {
    auto input = observation;
    for (auto& value : input) {
        if (!value.known) continue;
        value.guarded = true;
        value.complete = false;
        if (value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    for (auto& [offset, value] : input.stack_values) {
        static_cast<void>(offset);
        value.guarded = true;
        value.complete = false;
        if (value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    for (auto& [address, value] : input.memory_values) {
        static_cast<void>(address);
        value.guarded = true;
        value.complete = false;
        if (value.values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(value);
    }
    return input;
}

bool requires_forwarded_isolated_store_harvest(const katana::io::ExecutableImage& image,
                                               const AbstractState& observation,
                                               const AbstractState& merged_input) {
    for (std::uint8_t index = 4u; index <= 7u; ++index) {
        const auto& observed = observation[index];
        if (!observed.known || observed.values.empty() ||
            observed.values.size() > maximum_summary_values ||
            !observed.inventory_code_pointer)
            continue;
        const auto& merged = merged_input[index];
        for (const auto value : observed.values) {
            if (!validate_decode_candidate(image, value).valid()) continue;
            if (!merged.known ||
                std::find(merged.values.begin(), merged.values.end(), value) ==
                    merged.values.end())
                return true;
        }
    }
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
detail::analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> function_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    detail::GuardedNativeEntryShapeCache& guarded_native_entry_shapes) {
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
    bool inventory_region_walk_truncated = false;
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
                        {control.address, successor, true, true, true});
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
                        {control.address, successor, false, true});
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
                     candidates->second.complete});
            }
        }
        if (region_budget_exhausted)
            inventory_region_walk_truncated = true;
        if (visited_blocks.empty() || region_budget_exhausted)
            continue;
        InventoryRegion region;
        region.function.entry_address = target;
        region.function.evidence = ControlFlowEvidence::GuardedPartial;
        region.function.block_addresses.assign(visited_blocks.begin(),
                                               visited_blocks.end());
        normalize(region.function.block_addresses);
        inventory_regions.push_back(std::move(region));
        for (const auto& ingress : local_tail_ingresses) {
            inventory_region_tail_ingresses.push_back(ingress);
            enqueue_inventory_region(ingress.target);
        }
    }
    if (!pending_inventory_regions.empty())
        inventory_region_walk_truncated = true;
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
                                       const bool requires_code_pointer = false) {
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
    };
    for (const auto& carrier : candidate_tail_carriers) {
        const std::array target{carrier.target};
        add_tail_ingress(carrier.transfer_site, target, true, false, true);
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
        add_tail_ingress(transfer_site,
                         candidates.targets,
                         candidates.guarded,
                         candidates.complete,
                         true);
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
                add_tail_ingress(control.address, target, false, true);
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
                             candidates->second.complete);
        }
    }
    for (const auto& ingress : inventory_region_tail_ingresses) {
        const std::array target{ingress.target};
        add_tail_ingress(ingress.transfer_site,
                         target,
                         ingress.guarded,
                         ingress.complete,
                         ingress.requires_code_pointer);
    }
    for (auto& [transfer_site, ingress] : tail_ingresses) {
        static_cast<void>(transfer_site);
        normalize(ingress.targets);
    }
    std::unordered_set<std::uint32_t> functions_with_inventory_tail;
    for (const auto& [transfer_site, ingress] : tail_ingresses) {
        const auto owners = function_owners_by_control.find(transfer_site);
        if (owners == function_owners_by_control.end())
            continue;
        functions_with_inventory_tail.insert(owners->second.begin(),
                                             owners->second.end());
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
        if (!function_contains_guarded_inventory_store(function, block_index)) continue;
        add_inventory_sink(function.entry_address);
    }
    for (const auto function : functions_with_inventory_tail)
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
        auto evaluation = evaluate_function(image,
                                            *function->second,
                                            block_index,
                                            summary_indirect_callees,
                                            no_tail_ingresses,
                                            summaries,
                                            candidate_inputs[address].state,
                                            false);
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
            const auto input = candidate_inputs.find(observation.callee);
            if (input == candidate_inputs.end()) continue;
            if (merge_candidate_input(input->second, observation)) {
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

    struct ResolutionFunctionResult {
        FunctionEvaluation evaluation;
        GuardedCodeInventoryCollector inventory{true};
        bool inventory_walk_truncated = false;
    };
    const auto& final_candidate_inputs = std::as_const(candidate_inputs);
    const auto& final_function_by_address = std::as_const(function_by_address);
    const auto& final_inventory_region_by_address =
        std::as_const(inventory_region_by_address);
    const auto candidate_call_pair_key =
        [](const std::uint32_t call_site, const std::uint32_t callee) {
            return (static_cast<std::uint64_t>(call_site) << 32u) |
                   callee;
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
    const auto evaluate_resolution_function = [&](const std::size_t function_index) {
        ResolutionFunctionResult function_result;
        const auto* function = resolution_functions[function_index];
        const auto& input = final_candidate_inputs.at(function->entry_address);
        function_result.evaluation = evaluate_function(image,
                                                       *function,
                                                       block_index,
                                                       inventory_indirect_callees,
                                                       tail_ingresses,
                                                       summaries,
                                                       input.state,
                                                       true,
                                                       false,
                                                       &function_result.inventory);
        std::size_t forwarded_context_count = 0u;
        const auto harvest_forwarded_inventory =
            [&](const FunctionEvaluation& seed,
                const std::optional<std::uint32_t> root_call_site) {
                struct ForwardedStoreContext {
                    const FunctionInfo* function = nullptr;
                    std::uint32_t target = 0u;
                    std::uint32_t ingress_site = 0u;
                    bool tail = false;
                    AbstractState input;
                };
                std::deque<ForwardedStoreContext> forwarded_contexts;
                std::vector<ForwardedStoreContext>
                    visited_forwarded_contexts;
                visited_forwarded_contexts.reserve(maximum_forwarded_store_contexts);
                const auto enqueue_context =
                    [&](const FunctionInfo* target_function,
                        const std::uint32_t target,
                        const std::uint32_t ingress_site,
                        const bool tail,
                        AbstractState forwarded_input) {
                        if (std::any_of(
                                visited_forwarded_contexts.begin(),
                                visited_forwarded_contexts.end(),
                                [&](const auto& visited) {
                                    return visited.target == target &&
                                           visited.ingress_site == ingress_site &&
                                           visited.tail == tail &&
                                           visited.input == forwarded_input;
                                }))
                            return;
                        if (forwarded_context_count + forwarded_contexts.size() >=
                            maximum_forwarded_store_contexts) {
                            function_result.inventory_walk_truncated = true;
                            return;
                        }
                        visited_forwarded_contexts.push_back(
                            {target_function,
                             target,
                             ingress_site,
                             tail,
                             forwarded_input});
                        forwarded_contexts.push_back(
                            {target_function,
                             target,
                             ingress_site,
                             tail,
                             std::move(forwarded_input)});
                    };
                const auto enqueue_call =
                    [&](const FunctionEvaluation::CallArguments& forwarded) {
                        const auto forwarded_function =
                            final_function_by_address.find(forwarded.callee);
                        const auto forwarded_input =
                            final_candidate_inputs.find(forwarded.callee);
                        if (forwarded_function == final_function_by_address.end() ||
                            forwarded_input == final_candidate_inputs.end() ||
                            !functions_reaching_guarded_inventory_sink.contains(
                                forwarded.callee) ||
                            !requires_forwarded_isolated_store_harvest(
                                image,
                                forwarded.state,
                                forwarded_input->second.state))
                            return;
                        enqueue_context(
                            forwarded_function->second,
                            forwarded.callee,
                            forwarded.call_site,
                            false,
                            isolated_store_input(forwarded.call_site,
                                                 forwarded.state));
                    };
                const auto enqueue_tail =
                    [&](const FunctionEvaluation::InventoryTransfer& forwarded) {
                        const auto region =
                            final_inventory_region_by_address.find(forwarded.target);
                        if (region == final_inventory_region_by_address.end())
                            return;
                        enqueue_context(region->second,
                                        forwarded.target,
                                        forwarded.transfer_site,
                                        true,
                                        tail_store_input(forwarded.state));
                    };
                for (const auto& forwarded : seed.call_arguments)
                    enqueue_call(forwarded);
                for (const auto& forwarded : seed.inventory_transfers)
                    enqueue_tail(forwarded);

                // This is an inventory-only walk.  Calls retain the existing
                // isolated ABI contract, while terminal jumps preserve the
                // complete post-delay state in an owner-bounded ephemeral
                // region.  Neither path contributes summaries or CFG edges.
                while (!forwarded_contexts.empty() &&
                       forwarded_context_count <
                           maximum_forwarded_store_contexts) {
                    auto forwarded = std::move(forwarded_contexts.front());
                    forwarded_contexts.pop_front();
                    ++forwarded_context_count;
                    auto forwarded_evaluation = evaluate_function(
                        image,
                        *forwarded.function,
                        block_index,
                        inventory_indirect_callees,
                        tail_ingresses,
                        summaries,
                        forwarded.input,
                        false,
                        true,
                        &function_result.inventory,
                        root_call_site);
                    for (const auto& nested : forwarded_evaluation.call_arguments)
                        enqueue_call(nested);
                    for (const auto& nested : forwarded_evaluation.inventory_transfers)
                        enqueue_tail(nested);
                }
                if (!forwarded_contexts.empty())
                    function_result.inventory_walk_truncated = true;
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
            const auto enqueue_context =
                [&](const std::uint32_t address) {
                    if (queued_contexts.insert(address).second)
                        pending_contexts.push_back(address);
                };
            context_inputs.emplace(function->entry_address, input.state);
            enqueue_context(function->entry_address);
            std::size_t contextual_evaluations = 0u;
            while (!pending_contexts.empty() &&
                   contextual_evaluations <
                       maximum_forwarded_store_contexts) {
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
                    false,
                    true,
                    &function_result.inventory,
                    std::nullopt,
                    &contextual_summaries);
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
                    if (!candidate_call_pairs.contains(
                            candidate_call_pair_key(
                                observation.call_site,
                                observation.callee)))
                        continue;
                    if (!final_function_by_address.contains(
                            observation.callee))
                        continue;
                    if (!context_inputs.contains(observation.callee) &&
                        context_inputs.size() >=
                            maximum_forwarded_store_contexts) {
                        function_result.inventory_walk_truncated = true;
                        continue;
                    }
                    context_callers[observation.callee].insert(address);
                    const auto [stored, inserted] =
                        context_inputs.try_emplace(
                            observation.callee,
                            std::move(observation.state));
                    if (inserted ||
                        merge_state(stored->second, observation.state))
                        enqueue_context(observation.callee);
                }
            }
            if (!pending_contexts.empty())
                function_result.inventory_walk_truncated = true;
        };
        if (!result.budget_exhausted)
            harvest_contextual_candidate_returns();
        if (!result.budget_exhausted)
            harvest_forwarded_inventory(function_result.evaluation,
                                        std::nullopt);
        if (!result.budget_exhausted &&
            functions_reaching_guarded_inventory_sink.contains(
                function->entry_address)) {
            for (const auto& [call_site, observation] : input.observations) {
                if (!functions_with_inventory_tail.contains(function->entry_address) &&
                    !requires_isolated_store_harvest(input, call_site, observation))
                    continue;
                auto isolated_evaluation =
                    evaluate_function(image,
                                      *function,
                                      block_index,
                                      inventory_indirect_callees,
                                      tail_ingresses,
                                      summaries,
                                      isolated_store_input(call_site,
                                                           observation),
                                      false,
                                      true,
                                      &function_result.inventory,
                                      call_site);
                harvest_forwarded_inventory(isolated_evaluation,
                                            call_site);
            }
        }
        return function_result;
    };

    std::vector<std::optional<ResolutionFunctionResult>> function_results(
        resolution_functions.size());
    auto resolution_jobs = std::size_t{1u};
    if (resolution_functions.size() >= minimum_parallel_resolution_functions) {
        resolution_jobs =
            std::min(resolution_functions.size(),
                     std::min(maximum_parallel_resolution_jobs,
                              static_cast<std::size_t>(
                                  std::max(1u, std::thread::hardware_concurrency()))));
    }
    if (resolution_jobs == 1u) {
        for (std::size_t index = 0u; index < resolution_functions.size(); ++index)
            function_results[index].emplace(evaluate_resolution_function(index));
    } else {
        std::atomic_size_t next_function = 0u;
        std::vector<std::exception_ptr> errors(resolution_functions.size());
        std::vector<std::future<void>> workers;
        workers.reserve(resolution_jobs);
        for (std::size_t worker = 0u; worker < resolution_jobs; ++worker) {
            workers.push_back(std::async(std::launch::async, [&] {
                for (;;) {
                    const auto index =
                        next_function.fetch_add(1u, std::memory_order_relaxed);
                    if (index >= resolution_functions.size()) return;
                    try {
                        function_results[index].emplace(
                            evaluate_resolution_function(index));
                    } catch (...) {
                        errors[index] = std::current_exception();
                    }
                }
            }));
        }
        for (auto& worker : workers)
            worker.get();
        for (const auto& error : errors) {
            if (error) std::rethrow_exception(error);
        }
    }

    bool inventory_walk_truncated = inventory_region_walk_truncated;
    for (auto& function_result : function_results) {
        auto resolved = std::move(*function_result);
        inventory_walk_truncated =
            inventory_walk_truncated || resolved.inventory_walk_truncated;
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
    result.guarded_code_inventory = guarded_inventory_collector.finish();
    result.guarded_code_inventory.candidate_inventory_truncated =
        result.guarded_code_inventory.candidate_inventory_truncated ||
        inventory_walk_truncated;
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
