#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/instruction.hpp"

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
constexpr std::size_t maximum_memory_values = 256u;
constexpr std::size_t maximum_fixpoint_iterations = 65'536u;
constexpr std::int32_t maximum_stack_distance = 4'096;

std::vector<std::vector<std::uint32_t>>
strong_components(const std::span<const FunctionInfo> functions) {
    std::unordered_map<std::uint32_t, const FunctionInfo*> by_address;
    for (const auto& function : functions)
        by_address.emplace(function.entry_address, &function);
    std::unordered_map<std::uint32_t, std::size_t> index;
    std::unordered_map<std::uint32_t, std::size_t> lowlink;
    std::unordered_set<std::uint32_t> on_stack;
    std::vector<std::uint32_t> stack;
    std::vector<std::vector<std::uint32_t>> components;
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

void make_unknown(AbstractValue& value) {
    value.known = false;
    value.guarded = false;
    value.complete = false;
    value.values.clear();
    value.call_sites.clear();
    value.callees.clear();
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
    const auto before = destination;
    destination.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
    destination.callees.insert(source.callees.begin(), source.callees.end());
    if (!destination.known || !source.known) {
        destination.known = false;
        destination.guarded = false;
        destination.complete = false;
        destination.values.clear();
    } else {
        destination.guarded = destination.guarded || source.guarded;
        destination.complete = destination.complete && source.complete;
        destination.values.insert(
            destination.values.end(), source.values.begin(), source.values.end());
        normalize(destination.values);
        if (destination.values.size() > maximum_summary_values) make_unknown(destination);
    }
    return destination != before;
}

bool merge_state(AbstractState& destination, const AbstractState& source) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.size(); ++index)
        changed = merge_value(destination[index], source[index]) || changed;
    for (std::size_t index = 0u; index < destination.stack_offsets.size(); ++index) {
        if (destination.stack_offsets[index] == source.stack_offsets[index]) continue;
        if (destination.stack_offsets[index].has_value()) changed = true;
        destination.stack_offsets[index].reset();
    }
    for (auto slot = destination.stack_values.begin(); slot != destination.stack_values.end();) {
        const auto source_slot = source.stack_values.find(slot->first);
        if (source_slot == source.stack_values.end()) {
            slot = destination.stack_values.erase(slot);
            changed = true;
            continue;
        }
        changed = merge_value(slot->second, source_slot->second) || changed;
        ++slot;
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
        }
    }
}

void apply_binary(AbstractValue& destination,
                  const AbstractValue& source,
                  const katana::sh4::InstructionKind kind) {
    if (!destination.known || !source.known) {
        make_unknown(destination);
        return;
    }
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
                make_unknown(destination);
                return;
            }
        }
    }
    normalize(values);
    destination.values = std::move(values);
    destination.guarded = destination.guarded || source.guarded;
    destination.complete = destination.complete && source.complete;
    destination.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
    destination.callees.insert(source.callees.begin(), source.callees.end());
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
                            const std::size_t width) {
    if (!offset.has_value()) {
        state.stack_values.clear();
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
                       const std::size_t width,
                       const AbstractValue& value) {
    invalidate_stack_range(state, offset, width);
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
                                             const AbstractValue& right) {
    if (!left.known || !right.known) return {};
    std::vector<std::uint32_t> addresses;
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
                    const katana::io::ExecutableImage& image) {
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
        return;
    case katana::sh4::InstructionKind::MovRegister:
        state[instruction.destination_register] = state[instruction.source_register];
        state.stack_offsets[instruction.destination_register] =
            state.stack_offsets[instruction.source_register];
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        adjust_stack_offset(state, instruction.destination_register, instruction.immediate);
        return;
    case katana::sh4::InstructionKind::AddRegister:
    case katana::sh4::InstructionKind::SubRegister:
    case katana::sh4::InstructionKind::AndRegister:
    case katana::sh4::InstructionKind::OrRegister:
    case katana::sh4::InstructionKind::XorRegister:
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
        return;
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_value(state[0u],
                  ((line.address + 4u) & ~3u) +
                      static_cast<std::uint32_t>(instruction.displacement));
        state.stack_offsets[0u].reset();
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
        return;
    }
    case katana::sh4::InstructionKind::MovByteStore:
    case katana::sh4::InstructionKind::MovWordStore:
    case katana::sh4::InstructionKind::MovLongStore: {
        const auto width = instruction.kind == katana::sh4::InstructionKind::MovByteStore   ? 1u
                           : instruction.kind == katana::sh4::InstructionKind::MovWordStore ? 2u
                                                                                            : 4u;
        const auto offset = stack_slot(state, instruction.destination_register);
        store_stack_value(state, offset, width, state[instruction.source_register]);
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
        store_stack_value(state, offset, width, state[instruction.source_register]);
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
        store_stack_value(state, offset, width, state[instruction.source_register]);
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
        return;
    }
    case katana::sh4::InstructionKind::MovByteStoreR0Indexed:
    case katana::sh4::InstructionKind::MovWordStoreR0Indexed:
    case katana::sh4::InstructionKind::MovLongStoreR0Indexed: {
        const auto width =
            instruction.kind == katana::sh4::InstructionKind::MovByteStoreR0Indexed   ? 1u
            : instruction.kind == katana::sh4::InstructionKind::MovWordStoreR0Indexed ? 2u
                                                                                      : 4u;
        state.stack_values.clear();
        auto evidence = state[0u];
        apply_binary(evidence,
                     state[instruction.destination_register],
                     katana::sh4::InstructionKind::AddRegister);
        store_memory_values(state,
                            indexed_addresses(state[0u], state[instruction.destination_register]),
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
        invalidate_stack_range(state, offset, 4u);
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
                           indexed_addresses(state[0u], state[instruction.source_register]),
                           width,
                           image,
                           &evidence);
        state.stack_offsets[instruction.destination_register].reset();
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
                std::vector<FunctionEvaluation::CallArguments>* call_arguments) {
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
        for (const auto candidate : candidate_callees)
            call_arguments->push_back({call_site, candidate, observation});
    }
    if (image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC) {
        for (auto& value : state)
            make_unknown(value);
        state.stack_values.clear();
        state.memory_values.clear();
        return;
    }
    const bool escaped_stack_alias =
        std::any_of(state.stack_offsets.begin(),
                    state.stack_offsets.begin() + 8,
                    [](const auto offset) { return offset.has_value(); });
    if (escaped_stack_alias) state.stack_values.clear();
    state.memory_values.clear();
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        make_unknown(state[index]);
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.stack_offsets[index].reset();
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
    bool returned_memory_complete = candidate_callees_complete;
    bool returned_memory_initialized = false;
    std::map<std::uint32_t, AbstractValue> returned_memory;
    for (const auto candidate : callees) {
        const auto summary = summaries.find(candidate);
        if (summary == summaries.end()) {
            returned_complete = false;
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
        if (returned == nullptr || !returned->complete || returned->values.empty()) {
            returned_complete = false;
            continue;
        }
        returned_values.insert(
            returned_values.end(), returned->values.begin(), returned->values.end());
        returned_guarded = returned_guarded || returned->guarded;
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
        if (!validate_decode_candidate(image, target).valid()) return {};
        targets.push_back(target);
    }
    normalize(targets);
    return targets;
}

FunctionEvaluation evaluate_function(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const std::unordered_map<std::uint32_t, IndirectCalleeCandidates>& indirect_callees,
    const std::map<std::uint32_t, FunctionValueSummary>& summaries,
    const AbstractState& initial_state,
    const bool collect_resolutions) {
    FunctionEvaluation evaluation;
    evaluation.summary.function_address = function.entry_address;
    std::unordered_set<std::uint32_t> members(function.block_addresses.begin(),
                                              function.block_addresses.end());
    std::unordered_map<std::uint32_t, AbstractState> inputs;
    std::deque<std::uint32_t> pending;
    inputs.emplace(function.entry_address, initial_state);
    pending.push_back(function.entry_address);
    std::vector<std::pair<std::uint32_t, AbstractState>> returns;

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        const auto block = blocks.find(address);
        if (block == blocks.end()) continue;
        auto state = inputs[address];
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
            if (!call) apply_transfer(state, line, image);
            if (delayed_call.has_value()) {
                apply_call(state,
                           image,
                           delayed_call->call_site,
                           delayed_call->direct_callee,
                           delayed_call->candidate_callees,
                           delayed_call->candidate_callees_guarded,
                           delayed_call->candidate_callees_complete,
                           summaries,
                           &evaluation.call_arguments);
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
                               &evaluation.call_arguments);
            }
        }
        if (controlling_line(*block->second).instruction.kind ==
            katana::sh4::InstructionKind::Rts) {
            returns.emplace_back(controlling_line(*block->second).address, state);
        }
        for (const auto successor : block->second->successors) {
            if (!members.contains(successor)) continue;
            const auto [input, inserted] = inputs.emplace(successor, state);
            if (inserted || merge_state(input->second, state)) pending.push_back(successor);
        }
    }

    const std::array<std::uint8_t, 8u> summary_registers{0u, 8u, 9u, 10u, 11u, 12u, 13u, 14u};
    for (const auto register_index : summary_registers) {
        FunctionRegisterValueSummary summary;
        summary.register_index = register_index;
        summary.abi_preserved =
            register_index >= 8u && image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC;
        for (const auto& [return_site, state] : returns) {
            static_cast<void>(state);
            summary.return_sites.push_back(return_site);
        }
        if (summary.abi_preserved) {
            summary.reason = returns.empty() ? "no-return" : "abi-preserved-input";
            evaluation.summary.registers.push_back(std::move(summary));
            continue;
        }
        bool complete = !returns.empty();
        std::set<std::uint32_t> values;
        std::set<std::uint32_t> evidence;
        for (const auto& [return_site, state] : returns) {
            static_cast<void>(return_site);
            const auto& value = state[register_index];
            if (!value.known || !value.complete || value.values.empty()) {
                complete = false;
                continue;
            }
            values.insert(value.values.begin(), value.values.end());
            evidence.insert(value.callees.begin(), value.callees.end());
            summary.guarded = summary.guarded || value.guarded;
        }
        if (values.size() > maximum_summary_values) complete = false;
        summary.complete = complete;
        if (complete) summary.values.assign(values.begin(), values.end());
        summary.evidence_callees.assign(evidence.begin(), evidence.end());
        summary.reason =
            complete ? (summary.values.size() == 1u ? "constant-return" : "finite-return-set")
                     : (returns.empty() ? "no-return" : "return-path-unknown");
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

bool merge_candidate_input(CandidateInput& destination,
                           const FunctionEvaluation::CallArguments& observation) {
    const auto previous = destination.state;
    destination.observations[observation.call_site] = observation.state;
    AbstractState merged;
    merged.stack_offsets[15u] = 0;
    if (destination.unknown_ingress || destination.expected_call_sites.empty() ||
        !std::all_of(
            destination.expected_call_sites.begin(),
            destination.expected_call_sites.end(),
            [&](const auto call_site) { return destination.observations.contains(call_site); })) {
        destination.state = std::move(merged);
        return destination.state != previous;
    }
    for (std::uint8_t index = 0u; index < 15u; ++index) {
        auto& target = merged[index];
        bool first = true;
        for (const auto call_site : destination.expected_call_sites) {
            const auto& source = destination.observations.at(call_site)[index];
            if (!source.known || source.values.empty()) {
                make_unknown(target);
                first = false;
                break;
            }
            if (first) {
                target = source;
                first = false;
            } else {
                target.values.insert(
                    target.values.end(), source.values.begin(), source.values.end());
                normalize(target.values);
                target.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
                target.callees.insert(source.callees.begin(), source.callees.end());
                target.complete = target.complete && source.complete;
            }
            target.guarded = true;
            target.call_sites.insert(call_site);
            if (target.values.size() > maximum_summary_values) {
                make_unknown(target);
                break;
            }
        }
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
    destination.state = std::move(merged);
    return destination.state != previous;
}

} // namespace

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    FunctionValueAnalysisResult result;
    result.iteration_budget = maximum_fixpoint_iterations;
    if (lines.empty() || function_entries.empty() ||
        image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC)
        return result;
    const auto blocks = build_basic_blocks(lines, resolved_edges, function_entries);
    std::unordered_map<std::uint32_t, const BasicBlock*> block_index;
    for (const auto& block : blocks)
        block_index.emplace(block.start_address, &block);
    const auto functions = discover_functions_from_blocks(blocks, function_entries, resolved_edges);
    const auto components = strong_components(functions);
    result.strongly_connected_components = components.size();
    std::unordered_map<std::uint32_t, IndirectCalleeCandidates> indirect_callees;
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
    for (const auto& component : components) {
        for (const auto address : component) {
            pending.push_back(address);
            queued.insert(address);
        }
    }
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
        auto evaluation = evaluate_function(image,
                                            *function->second,
                                            block_index,
                                            indirect_callees,
                                            summaries,
                                            candidate_inputs[address].state,
                                            false);
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
    }

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
    for (const auto& function : functions) {
        auto evaluation = evaluate_function(image,
                                            function,
                                            block_index,
                                            indirect_callees,
                                            summaries,
                                            candidate_inputs[function.entry_address].state,
                                            true);
        result.resolutions.insert(result.resolutions.end(),
                                  std::make_move_iterator(evaluation.resolutions.begin()),
                                  std::make_move_iterator(evaluation.resolutions.end()));
    }
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
    return result;
}

} // namespace katana::analysis
