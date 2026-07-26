#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"
#include "snapshot_pointer_candidates.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_summary_values = 8u;
constexpr std::size_t maximum_guarded_code_inventory = 1'024u;
constexpr std::size_t maximum_memory_values = 256u;
constexpr std::size_t maximum_fixpoint_iterations = 65'536u;
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
    std::map<std::int32_t, AbstractValue> stack_values;
    std::map<std::uint32_t, AbstractValue> memory_values;

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
};

struct IndirectCalleeCandidates {
    std::vector<std::uint32_t> targets;
    bool guarded = false;
    bool complete = true;
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

class GuardedCodeInventoryCollector {
  public:
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
            if (!admit(candidate.target_address)) continue;
            candidate.guarded = true;
            const auto [stored, inserted] =
                stored_candidates_.try_emplace(candidate.target_address,
                                               std::move(candidate));
            if (inserted) continue;
            auto& destination = stored->second;
            destination.complete =
                destination.complete && candidate.complete;
            destination.guarded = true;
            destination.store_instruction_addresses.insert(
                destination.store_instruction_addresses.end(),
                candidate.store_instruction_addresses.begin(),
                candidate.store_instruction_addresses.end());
            destination.evidence_call_sites.insert(
                destination.evidence_call_sites.end(),
                candidate.evidence_call_sites.begin(),
                candidate.evidence_call_sites.end());
            destination.evidence_callees.insert(
                destination.evidence_callees.end(),
                candidate.evidence_callees.begin(),
                candidate.evidence_callees.end());
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
        for (auto& candidate : candidates) {
            table_scan_truncated_ =
                table_scan_truncated_ || candidate.scan_truncated;
            normalize(candidate.target_addresses);
            if (candidate.target_addresses.empty()) continue;
            const auto [stored, inserted] =
                returned_tables_.try_emplace(candidate.table_address,
                                             std::move(candidate));
            if (inserted) continue;
            auto& destination = stored->second;
            destination.target_addresses.insert(
                destination.target_addresses.end(),
                candidate.target_addresses.begin(),
                candidate.target_addresses.end());
            destination.load_instruction_addresses.insert(
                destination.load_instruction_addresses.end(),
                candidate.load_instruction_addresses.begin(),
                candidate.load_instruction_addresses.end());
            destination.evidence_call_sites.insert(
                destination.evidence_call_sites.end(),
                candidate.evidence_call_sites.begin(),
                candidate.evidence_call_sites.end());
            destination.evidence_callees.insert(
                destination.evidence_callees.end(),
                candidate.evidence_callees.begin(),
                candidate.evidence_callees.end());
            destination.scan_truncated =
                destination.scan_truncated || candidate.scan_truncated;
        }
    }

    GuardedCodeInventory finish() {
        GuardedCodeInventory inventory;
        inventory.stored_code_addresses.reserve(stored_candidates_.size());
        for (auto& [target, candidate] : stored_candidates_) {
            static_cast<void>(target);
            normalize(candidate.store_instruction_addresses);
            normalize(candidate.evidence_call_sites);
            normalize(candidate.evidence_callees);
            inventory.stored_code_addresses.push_back(std::move(candidate));
        }
        inventory.returned_code_address_tables.reserve(returned_tables_.size());
        for (auto& [table_address, candidate] : returned_tables_) {
            static_cast<void>(table_address);
            normalize(candidate.target_addresses);
            // Stored callback addresses carry the strongest inventory
            // provenance, so admit all of them before broad returned-table
            // scans compete for the shared bounded candidate budget.
            std::erase_if(candidate.target_addresses,
                          [&](const auto target) { return !admit(target); });
            if (candidate.target_addresses.empty()) continue;
            normalize(candidate.load_instruction_addresses);
            normalize(candidate.evidence_call_sites);
            normalize(candidate.evidence_callees);
            inventory.returned_code_address_tables.push_back(
                std::move(candidate));
        }
        inventory.candidate_budget = maximum_guarded_code_inventory;
        inventory.candidate_count = admitted_targets_.size();
        inventory.candidate_inventory_truncated =
            candidate_inventory_truncated_;
        inventory.table_scan_truncated = table_scan_truncated_;
        return inventory;
    }

  private:
    bool admit(const std::uint32_t target) {
        if (admitted_targets_.contains(target)) return true;
        if (admitted_targets_.size() >= maximum_guarded_code_inventory) {
            candidate_inventory_truncated_ = true;
            return false;
        }
        admitted_targets_.insert(target);
        return true;
    }

    std::set<std::uint32_t> admitted_targets_;
    std::map<std::uint32_t, StoredCodeAddressCandidate> stored_candidates_;
    std::map<std::uint32_t, ReturnedCodeAddressTableCandidate> returned_tables_;
    bool candidate_inventory_truncated_ = false;
    bool table_scan_truncated_ = false;
};

void make_unknown(AbstractValue& value) {
    value.known = false;
    value.guarded = false;
    value.complete = false;
    value.values.clear();
    value.call_sites.clear();
    value.callees.clear();
}

void make_unknown_preserving_provenance(AbstractValue& value) {
    auto call_sites = std::move(value.call_sites);
    auto callees = std::move(value.callees);
    make_unknown(value);
    value.call_sites = std::move(call_sites);
    value.callees = std::move(callees);
}

void set_value(AbstractValue& value, const std::uint32_t constant) {
    value.known = true;
    value.guarded = false;
    value.complete = true;
    value.values = {constant};
    value.call_sites.clear();
    value.callees.clear();
}

bool merge_value(AbstractValue& destination, const AbstractValue& source) {
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        const bool changed = destination.known || destination.guarded || destination.complete ||
                             !destination.values.empty() ||
                             call_sites != destination.call_sites ||
                             callees != destination.callees;
        destination.known = false;
        destination.guarded = false;
        destination.complete = false;
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
        }
    }
}

void apply_binary(AbstractValue& destination,
                  const AbstractValue& source,
                  const katana::sh4::InstructionKind kind) {
    if (!destination.known || !source.known) {
        auto call_sites = destination.call_sites;
        call_sites.insert(source.call_sites.begin(), source.call_sites.end());
        auto callees = destination.callees;
        callees.insert(source.callees.begin(), source.callees.end());
        make_unknown(destination);
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
                return;
            }
            if (values.size() > maximum_summary_values) {
                make_unknown_preserving_provenance(destination);
                return;
            }
        }
    }
    normalize(values);
    destination.values = std::move(values);
    destination.guarded = destination.guarded || source.guarded;
    destination.complete = destination.complete && source.complete;
}

template <typename Operation> void apply_unary(AbstractValue& value, Operation operation) {
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
        loaded.call_sites.insert(address_evidence->call_sites.begin(),
                                 address_evidence->call_sites.end());
        loaded.callees.insert(address_evidence->callees.begin(), address_evidence->callees.end());
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
    stored.call_sites.insert(address_evidence.call_sites.begin(),
                             address_evidence.call_sites.end());
    stored.callees.insert(address_evidence.callees.begin(), address_evidence.callees.end());
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
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        adjust_stack_offset(state, instruction.destination_register, instruction.immediate);
        return;
    case katana::sh4::InstructionKind::AddRegister:
    case katana::sh4::InstructionKind::SubRegister:
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister:
        state.stack_may_alias[instruction.destination_register] =
            state.stack_may_alias[instruction.destination_register] ||
            state.stack_may_alias[instruction.source_register];
        state.inventory_stack_may_alias[instruction.destination_register] =
            state.inventory_stack_may_alias[instruction.destination_register] ||
            state.inventory_stack_may_alias[instruction.source_register];
        apply_binary(state[instruction.destination_register],
                     state[instruction.source_register],
                     instruction.kind);
        state.stack_offsets[instruction.destination_register].reset();
        return;
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

void apply_call(AbstractState& state,
                const katana::io::ExecutableImage& image,
                const std::uint32_t call_site,
                const std::optional<std::uint32_t> callee,
                const std::span<const std::uint32_t> candidate_callees,
                const bool candidate_callees_guarded,
                const bool candidate_callees_complete,
                const std::map<std::uint32_t, FunctionValueSummary>& summaries,
                std::vector<FunctionEvaluation::CallArguments>* call_arguments,
                const bool preserve_guarded_stack_inventory = false) {
    if (call_arguments != nullptr) {
        auto observation = state;
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
    if (image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC) {
        for (std::size_t index = 0u; index < state.size(); ++index) {
            make_unknown(state[index]);
            state.stack_offsets[index].reset();
            state.stack_may_alias[index] = true;
            state.inventory_stack_may_alias[index] = true;
        }
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
    make_unknown(state[15u]);
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
        const auto summary = summaries.find(candidate);
        if (summary == summaries.end()) {
            returned_complete = false;
            returned_may_alias_stack = true;
            returned_memory_complete = false;
            continue;
        }
        if (!summary->second.memory_complete) {
            returned_memory_complete = false;
        } else {
            std::map<std::uint32_t, AbstractValue> candidate_memory;
            for (const auto& memory : summary->second.memory_values) {
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
        const auto* returned = register_summary(summary->second, 0u);
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
    std::vector<StoredCodeAddressCandidate>& candidates) {
    using K = katana::sh4::InstructionKind;
    const auto& instruction = line.instruction;
    bool supported = false;
    bool stack_based = false;
    std::set<std::uint32_t> evidence_call_sites;
    std::set<std::uint32_t> evidence_callees;
    const auto include_provenance = [&](const AbstractValue& evidence) {
        evidence_call_sites.insert(evidence.call_sites.begin(), evidence.call_sites.end());
        evidence_callees.insert(evidence.callees.begin(), evidence.callees.end());
    };
    switch (instruction.kind) {
    case K::MovLongStore:
    case K::MovLongStorePreDecrement:
    case K::MovLongStoreDisplacement:
        supported = true;
        stack_based =
            state.inventory_stack_may_alias[instruction.destination_register];
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStoreR0Indexed:
        supported = true;
        stack_based = state.inventory_stack_may_alias[0u] ||
                      state.inventory_stack_may_alias
                          [instruction.destination_register];
        include_provenance(state[0u]);
        include_provenance(state[instruction.destination_register]);
        break;
    case K::MovLongStoreGbrDisplacement:
        supported = true;
        break;
    default:
        break;
    }
    if (!supported || stack_based) return;

    const auto& value = state[instruction.source_register];
    include_provenance(value);
    if (!value.known || value.values.empty() ||
        value.values.size() > maximum_summary_values || evidence_call_sites.empty())
        return;
    std::vector<std::uint32_t> validated_candidates;
    validated_candidates.reserve(value.values.size());
    bool all_candidates_valid = true;
    for (const auto candidate : value.values) {
        const auto validation = validate_decode_candidate(image, candidate);
        if (!validation.valid()) {
            all_candidates_valid = false;
            continue;
        }
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
    const std::map<std::uint32_t, FunctionValueSummary>& summaries,
    const AbstractState& initial_state,
    const bool collect_resolutions,
    const bool may_merge_stack_inventory = false,
    GuardedCodeInventoryCollector* const guarded_inventory_collector = nullptr,
    const std::optional<std::uint32_t> isolated_inventory_call_site =
        std::nullopt) {
    FunctionEvaluation evaluation;
    evaluation.summary.function_address = function.entry_address;
    evaluation.call_arguments.reserve(function.direct_callees.size());
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
            if (collect_resolutions && !call &&
                guarded_inventory_collector != nullptr) {
                std::vector<StoredCodeAddressCandidate> stored_candidates;
                observe_stored_code_addresses(
                    image, line, state, stored_candidates);
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
                           &evaluation.call_arguments,
                           may_merge_stack_inventory);
                delayed_call.reset();
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
                               &evaluation.call_arguments,
                               may_merge_stack_inventory);
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
        target.call_sites = std::move(call_sites);
        target.callees = std::move(callees);
        merged.stack_may_alias[index] = may_alias_stack;
        merged.inventory_stack_may_alias[index] =
            inventory_may_alias_stack;
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

bool requires_isolated_store_harvest(const CandidateInput& input) {
    if (input.observations.empty()) return false;
    if (input.unknown_ingress || input.expected_call_sites.empty() ||
        !std::all_of(
            input.expected_call_sites.begin(),
            input.expected_call_sites.end(),
            [&](const auto call_site) { return input.observations.contains(call_site); }))
        return true;
    for (const auto call_site : input.expected_call_sites) {
        const auto& observation = input.observations.at(call_site);
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
        if (input[index].values.size() > maximum_summary_values)
            make_unknown_preserving_provenance(input[index]);
    }
    return input;
}

} // namespace

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    return analyze_function_values(image, lines, function_entries, resolved_edges, {});
}

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges,
                        const FunctionValueAnalysisProgressCallback& progress_callback) {
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
    if (lines.empty() || function_entries.empty() ||
        image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC)
        return result;
    const auto blocks = build_basic_blocks(lines, resolved_edges, function_entries);
    block_count = blocks.size();
    report_progress("blocks-complete");
    std::unordered_map<std::uint32_t, const BasicBlock*> block_index;
    block_index.reserve(blocks.size());
    for (const auto& block : blocks)
        block_index.emplace(block.start_address, &block);
    const auto functions = discover_functions_from_blocks(blocks, function_entries, resolved_edges);
    const auto components = strong_components(functions);
    function_count = functions.size();
    result.strongly_connected_components = components.size();
    report_progress("functions-complete");
    std::unordered_map<std::uint32_t, IndirectCalleeCandidates> indirect_callees;
    indirect_callees.reserve(resolved_edges.size());
    for (const auto& edge : resolved_edges) {
        if (edge.kind != ResolvedControlFlowKind::Call) continue;
        auto& candidates = indirect_callees[edge.instruction_address];
        candidates.targets.push_back(edge.target_address);
        const auto evidence = resolved_edge_evidence(edge);
        candidates.guarded = candidates.guarded || evidence != ControlFlowEvidence::ProvenComplete;
        candidates.complete = candidates.complete && control_flow_evidence_complete(evidence);
    }
    for (auto& [call_site, candidates] : indirect_callees) {
        static_cast<void>(call_site);
        normalize(candidates.targets);
    }
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
    for (const auto& line : lines) {
        if (line.instruction.control_flow != katana::sh4::ControlFlowKind::Call ||
            !line.target_address.has_value())
            continue;
        if (const auto input = candidate_inputs.find(*line.target_address);
            input != candidate_inputs.end())
            input->second.expected_call_sites.insert(line.address);
    }
    for (const auto& edge : resolved_edges) {
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
                                            indirect_callees,
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
    GuardedCodeInventoryCollector guarded_inventory_collector;
    std::vector<const FunctionInfo*> resolution_functions;
    resolution_functions.reserve(functions.size());
    for (const auto& function : functions)
        resolution_functions.push_back(&function);
    std::sort(resolution_functions.begin(),
              resolution_functions.end(),
              [](const auto* left, const auto* right) {
                  return left->entry_address < right->entry_address;
              });
    for (const auto* function : resolution_functions) {
        auto evaluation = evaluate_function(image,
                                            *function,
                                            block_index,
                                            indirect_callees,
                                            summaries,
                                            candidate_inputs[function->entry_address].state,
                                            true,
                                            false,
                                            &guarded_inventory_collector);
        resolution_count += evaluation.resolutions.size();
        result.resolutions.insert(result.resolutions.end(),
                                  std::make_move_iterator(evaluation.resolutions.begin()),
                                  std::make_move_iterator(evaluation.resolutions.end()));
        const auto& input = candidate_inputs.at(function->entry_address);
        if (image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC &&
            !result.budget_exhausted && requires_isolated_store_harvest(input)) {
            for (const auto& [call_site, observation] : input.observations) {
                auto isolated_evaluation = evaluate_function(
                    image,
                    *function,
                    block_index,
                    indirect_callees,
                    summaries,
                    isolated_store_input(call_site, observation),
                    true,
                    true,
                    &guarded_inventory_collector,
                    call_site);
                static_cast<void>(isolated_evaluation);
            }
        }
        ++completed_functions;
        if (completed_functions <= 16u || completed_functions % 128u == 0u ||
            completed_functions == functions.size())
            report_progress("resolution-progress");
    }
    result.guarded_code_inventory = guarded_inventory_collector.finish();
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
