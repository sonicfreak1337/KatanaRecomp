#include "static_callback_inventory.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "guarded_native_entry_shape.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis::detail {
namespace {

constexpr std::size_t maximum_scalar_constants = 8u;
constexpr std::size_t maximum_code_constants = 64u;
constexpr std::size_t maximum_inventory_candidates = 16'384u;
constexpr std::size_t maximum_stack_values = 256u;
constexpr std::size_t maximum_static_code_pointer_table_entries = 256u;
constexpr std::uint32_t maximum_static_code_pointer_table_stride = 256u;

struct CallbackValue final {
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t input_mask = 0u;
    std::set<std::uint32_t> constants;
    bool constants_truncated = false;
    // Keep executable values in a separate lane. Ordinary scalar joins (mode
    // IDs, sizes, flags, offsets, and table indices) may legitimately widen
    // long before the much smaller callback-address domain does. Letting that
    // ordinary widening poison the code lane made the positive inventory both
    // incomplete and needlessly broad.
    std::set<std::uint32_t> code_constants;
    bool code_constants_truncated = false;
    // Guaranteed power-of-two byte alignment for an otherwise unknown
    // scalar. This keeps the shape of mutable table indices through SHLL
    // operations without pretending that the index value itself is known.
    std::uint32_t minimum_alignment = 1u;
    std::optional<std::int32_t> stack_address;
    bool may_be_stack = false;

    bool operator==(const CallbackValue&) const = default;
};

struct CallbackState final {
    std::array<CallbackValue, 16> registers;
    std::map<std::int32_t, CallbackValue> stack_values;

    bool operator==(const CallbackState&) const = default;
};

struct CallbackCall final {
    std::uint32_t instruction_address = 0u;
    std::uint32_t callee = 0u;
    std::array<CallbackValue, 4> arguments;
};

struct CallbackFunctionModel final {
    std::uint32_t entry = 0u;
    std::uint8_t local_sink_mask = 0u;
    std::vector<CallbackCall> calls;
    std::vector<StoredCodeAddressCandidate> local_candidates;
    bool local_candidates_truncated = false;
};

[[nodiscard]] std::optional<std::uint32_t> executable_constant(
    const katana::io::ExecutableImage& image,
    std::uint32_t value);

[[nodiscard]] std::set<std::int32_t> discover_callback_field_offsets(
    const std::span<const katana::sh4::DisassemblyLine> lines) {
    std::set<std::int32_t> offsets;
    constexpr std::size_t maximum_writer_distance = 32u;
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        const auto kind = lines[index].instruction.kind;
        if (kind != katana::sh4::InstructionKind::Jsr &&
            kind != katana::sh4::InstructionKind::Jmp)
            continue;
        auto tracked = lines[index].instruction.branch_register;
        auto expected_address = lines[index].address;
        for (std::size_t distance = 0u;
             distance < maximum_writer_distance && index > distance;
             ++distance) {
            const auto& candidate = lines[index - distance - 1u];
            if (candidate.address + 2u != expected_address) break;
            expected_address = candidate.address;
            if (candidate.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::Call ||
                candidate.instruction.control_flow ==
                    katana::sh4::ControlFlowKind::IndirectCall)
                break;
            const auto writes = general_register_write_mask(
                candidate.instruction);
            if ((writes & static_cast<std::uint16_t>(1u << tracked)) == 0u)
                continue;
            if (candidate.instruction.destination_register != tracked)
                break;
            if (candidate.instruction.kind ==
                katana::sh4::InstructionKind::MovRegister) {
                tracked = candidate.instruction.source_register;
                continue;
            }
            if (candidate.instruction.kind ==
                katana::sh4::InstructionKind::MovLongLoadDisplacement) {
                offsets.insert(candidate.instruction.displacement);
            } else if (candidate.instruction.kind ==
                       katana::sh4::InstructionKind::MovLongLoad) {
                offsets.insert(0);
            }
            break;
        }
    }
    return offsets;
}

[[nodiscard]] bool add_bounded_scalar_constant(CallbackValue& value,
                                               const std::uint32_t constant) {
    if (value.constants_truncated || value.constants.contains(constant))
        return false;
    if (value.constants.size() >= maximum_scalar_constants) {
        value.constants.clear();
        value.constants_truncated = true;
        return true;
    }
    value.constants.insert(constant);
    return true;
}

[[nodiscard]] bool add_bounded_code_constant(CallbackValue& value,
                                             const std::uint32_t constant,
                                             GuardedNativeEntryShapeCache&
                                                 native_entry_shapes) {
    if (value.code_constants_truncated ||
        value.code_constants.contains(constant))
        return false;
    const auto status = native_entry_shapes.classify(constant);
    if (status == GuardedNativeEntryShapeStatus::ShapeBudgetExceeded) {
        value.code_constants.clear();
        value.code_constants_truncated = true;
        return true;
    }
    if (status != GuardedNativeEntryShapeStatus::Valid) return false;
    if (value.code_constants.size() >= maximum_code_constants) {
        value.code_constants.clear();
        value.code_constants_truncated = true;
        return true;
    }
    value.code_constants.insert(constant);
    return true;
}

void add_constant(CallbackValue& value,
                  const katana::io::ExecutableImage& image,
                  const std::uint32_t constant,
                  GuardedNativeEntryShapeCache& native_entry_shapes) {
    static_cast<void>(add_bounded_scalar_constant(value, constant));
    if (const auto target = executable_constant(image, constant);
        target.has_value())
        static_cast<void>(add_bounded_code_constant(
            value, *target, native_entry_shapes));
}

template <typename Transform>
void transform_scalar_value(CallbackValue& value,
                            const katana::io::ExecutableImage& image,
                            GuardedNativeEntryShapeCache& native_entry_shapes,
                            Transform&& transform) {
    const auto before = value;
    value = {};
    value.input_mask = before.input_mask;
    value.minimum_alignment = before.minimum_alignment;
    if (before.constants_truncated) {
        value.constants_truncated = true;
        if (before.code_constants_truncated) {
            value.code_constants_truncated = true;
        } else {
            for (const auto constant : before.code_constants) {
                const auto transformed = transform(constant);
                if (const auto target = executable_constant(image,
                                                              transformed);
                    target.has_value())
                    static_cast<void>(add_bounded_code_constant(
                        value, *target, native_entry_shapes));
            }
        }
        return;
    }
    for (const auto constant : before.constants)
        add_constant(value, image, transform(constant), native_entry_shapes);
}

[[nodiscard]] bool join_value(CallbackValue& destination,
                              const CallbackValue& source,
                              GuardedNativeEntryShapeCache&
                                  native_entry_shapes) {
    const auto before = destination;
    destination.input_mask = static_cast<std::uint8_t>(
        destination.input_mask | source.input_mask);
    if (destination.constants_truncated || source.constants_truncated) {
        destination.constants.clear();
        destination.constants_truncated = true;
    } else {
        for (const auto constant : source.constants)
            static_cast<void>(
                add_bounded_scalar_constant(destination, constant));
    }
    if (destination.code_constants_truncated ||
        source.code_constants_truncated) {
        destination.code_constants.clear();
        destination.code_constants_truncated = true;
    } else {
        for (const auto constant : source.code_constants)
            static_cast<void>(
                add_bounded_code_constant(destination, constant,
                                          native_entry_shapes));
    }
    destination.minimum_alignment =
        std::min(destination.minimum_alignment, source.minimum_alignment);
    if (destination.stack_address != source.stack_address)
        destination.stack_address.reset();
    destination.may_be_stack = destination.may_be_stack || source.may_be_stack;
    return destination != before;
}

[[nodiscard]] bool join_state(CallbackState& destination,
                              const CallbackState& source,
                              GuardedNativeEntryShapeCache&
                                  native_entry_shapes) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.registers.size(); ++index)
        changed = join_value(destination.registers[index],
                             source.registers[index],
                             native_entry_shapes) || changed;
    // Stack spill values are reusable only if every incoming path carries the
    // exact same cell. Payload within a retained cell remains a may-union.
    for (auto current = destination.stack_values.begin();
         current != destination.stack_values.end();) {
        const auto incoming = source.stack_values.find(current->first);
        if (incoming == source.stack_values.end()) {
            current = destination.stack_values.erase(current);
            changed = true;
            continue;
        }
        changed = join_value(current->second, incoming->second,
                             native_entry_shapes) || changed;
        ++current;
    }
    return changed;
}

[[nodiscard]] std::optional<std::uint32_t> read_image_u32(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 4u);
    if (!resolved.has_value()) return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 4u);
    if (segment == nullptr || !segment->permissions.readable)
        return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || *offset > segment->bytes.size() ||
        4u > segment->bytes.size() - *offset)
        return std::nullopt;
    return image.read_u32_le(*resolved);
}

[[nodiscard]] std::optional<std::uint32_t> executable_constant(
    const katana::io::ExecutableImage& image,
    const std::uint32_t value) {
    const auto validation = validate_decode_candidate(image, value);
    return validation.valid()
               ? std::optional<std::uint32_t>{validation.resolved_address}
               : std::nullopt;
}

[[nodiscard]] CallbackValue load_static_image_values(
    const katana::io::ExecutableImage& image,
    const CallbackValue& address,
    const std::int32_t displacement,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    CallbackValue result;
    result.input_mask = address.input_mask;
    // This is a positive guarded inventory. An unknown ordinary address does
    // not make the independently tracked executable-value lane Top; it simply
    // contributes no statically proven target.
    if (address.constants_truncated) return result;
    for (const auto base : address.constants) {
        const auto effective =
            base + static_cast<std::uint32_t>(displacement);
        const auto loaded = read_image_u32(image, effective);
        if (!loaded.has_value()) continue;
        add_constant(result, image, *loaded, native_entry_shapes);
    }
    return result;
}

[[nodiscard]] CallbackValue scan_static_code_pointer_table(
    const katana::io::ExecutableImage& image,
    const CallbackValue& base,
    const CallbackValue& byte_index,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    CallbackValue result;
    result.input_mask = static_cast<std::uint8_t>(
        base.input_mask | byte_index.input_mask);
    if (base.constants_truncated || base.constants.size() != 1u ||
        !byte_index.constants.empty() || byte_index.constants_truncated ||
        byte_index.minimum_alignment < 4u ||
        byte_index.minimum_alignment >
            maximum_static_code_pointer_table_stride ||
        (byte_index.minimum_alignment & 3u) != 0u)
        return result;

    const auto table = *base.constants.begin();
    if ((table & 3u) != 0u) return result;
    const auto stride = byte_index.minimum_alignment;
    std::size_t entries = 0u;
    for (; entries <= maximum_static_code_pointer_table_entries; ++entries) {
        const auto address = static_cast<std::uint64_t>(table) +
                             entries * static_cast<std::uint64_t>(stride);
        if (address > std::numeric_limits<std::uint32_t>::max()) break;
        const auto raw = read_image_u32(image,
                                        static_cast<std::uint32_t>(address));
        if (!raw.has_value() ||
            !executable_constant(image, *raw).has_value())
            break;
        if (entries == maximum_static_code_pointer_table_entries) {
            result.code_constants.clear();
            result.code_constants_truncated = true;
            return result;
        }
        add_constant(result, image, *raw, native_entry_shapes);
    }
    // A single executable word can be an ordinary pointer-valued field. Two
    // or more stride-consistent records followed by a non-code terminator are
    // the narrowest useful identity-bound table shape. The stride is proven
    // by the SH-4 index arithmetic rather than guessed from surrounding data.
    // The index may originate in mutable title state; this remains guarded
    // positive inventory and never marks the indirect transfer complete.
    if (entries < 2u) return {};
    return result;
}

[[nodiscard]] CallbackValue load_static_indexed_value(
    const katana::io::ExecutableImage& image,
    const CallbackValue& left,
    const CallbackValue& right,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    CallbackValue result;
    result.input_mask = static_cast<std::uint8_t>(
        left.input_mask | right.input_mask);
    if (!left.constants_truncated && !right.constants_truncated &&
        !left.constants.empty() && !right.constants.empty()) {
        for (const auto lhs : left.constants) {
            for (const auto rhs : right.constants) {
                const auto loaded = read_image_u32(image, lhs + rhs);
                if (loaded.has_value())
                    add_constant(result, image, *loaded,
                                 native_entry_shapes);
            }
        }
        return result;
    }
    result = scan_static_code_pointer_table(image, left, right,
                                            native_entry_shapes);
    if (!result.code_constants.empty() ||
        result.code_constants_truncated)
        return result;
    return scan_static_code_pointer_table(image, right, left,
                                          native_entry_shapes);
}

void erase_overlapping_stack_values(CallbackState& state,
                                    const std::int32_t address,
                                    const std::size_t width) {
    const auto end = static_cast<std::int64_t>(address) +
                     static_cast<std::int64_t>(width);
    for (auto current = state.stack_values.begin();
         current != state.stack_values.end();) {
        const auto cell_end = static_cast<std::int64_t>(current->first) + 4;
        if (static_cast<std::int64_t>(current->first) < end &&
            static_cast<std::int64_t>(address) < cell_end)
            current = state.stack_values.erase(current);
        else
            ++current;
    }
}

[[nodiscard]] std::optional<std::int32_t> displaced_stack_address(
    const CallbackValue& base,
    const std::int32_t displacement) {
    if (!base.stack_address.has_value()) return std::nullopt;
    const auto value = static_cast<std::int64_t>(*base.stack_address) +
                       displacement;
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(value);
}

void set_unknown(CallbackValue& value, const bool preserve_stack_origin = false) {
    const bool may_be_stack = preserve_stack_origin && value.may_be_stack;
    value = {};
    value.may_be_stack = may_be_stack;
}

void scale_minimum_alignment(CallbackValue& value,
                             const std::uint32_t scale) noexcept {
    const auto widened = static_cast<std::uint64_t>(value.minimum_alignment) *
                         static_cast<std::uint64_t>(scale);
    value.minimum_alignment = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            widened, maximum_static_code_pointer_table_stride));
}

void add_immediate_to_minimum_alignment(CallbackValue& value,
                                        const std::int32_t immediate) noexcept {
    if (immediate == 0) return;
    const auto bits = static_cast<std::uint32_t>(immediate);
    const auto immediate_alignment = bits & (~bits + 1u);
    value.minimum_alignment =
        std::min(value.minimum_alignment, immediate_alignment);
}

void transform_add_immediate(CallbackValue& value,
                             const katana::io::ExecutableImage& image,
                             GuardedNativeEntryShapeCache&
                                 native_entry_shapes,
                             const std::int32_t immediate,
                             const bool stack_pointer) {
    add_immediate_to_minimum_alignment(value, immediate);
    if (value.constants_truncated) {
        value.constants.clear();
    } else {
        std::set<std::uint32_t> adjusted;
        for (const auto constant : value.constants)
            adjusted.insert(constant + static_cast<std::uint32_t>(immediate));
        value.constants = std::move(adjusted);
    }
    if (!value.code_constants_truncated) {
        CallbackValue adjusted;
        for (const auto constant : value.code_constants) {
            const auto candidate =
                constant + static_cast<std::uint32_t>(immediate);
            if (const auto target = executable_constant(image, candidate);
                target.has_value())
                static_cast<void>(add_bounded_code_constant(
                    adjusted, *target, native_entry_shapes));
        }
        // A non-code scalar can become a code address after a small add.
        if (!value.constants_truncated) {
            for (const auto constant : value.constants) {
                if (const auto target = executable_constant(image, constant);
                    target.has_value())
                    static_cast<void>(add_bounded_code_constant(
                        adjusted, *target, native_entry_shapes));
            }
        }
        value.code_constants = std::move(adjusted.code_constants);
        value.code_constants_truncated = adjusted.code_constants_truncated;
    }
    if (value.stack_address.has_value()) {
        const auto adjusted = static_cast<std::int64_t>(*value.stack_address) +
                              immediate;
        if (adjusted < std::numeric_limits<std::int32_t>::min() ||
            adjusted > std::numeric_limits<std::int32_t>::max())
            value.stack_address.reset();
        else
            value.stack_address = static_cast<std::int32_t>(adjusted);
    }
    // Arithmetic changes the incoming pointer. It no longer proves that the
    // original ABI parameter itself is stored as a callback.
    if (!stack_pointer) value.input_mask = 0u;
}

[[nodiscard]] std::size_t memory_width(
    const katana::sh4::InstructionKind kind) noexcept {
    using K = katana::sh4::InstructionKind;
    switch (kind) {
    case K::MovByteStore:
    case K::MovByteLoad:
    case K::MovByteStorePreDecrement:
    case K::MovByteLoadPostIncrement:
    case K::MovByteStoreDisplacement:
    case K::MovByteLoadDisplacement:
    case K::MovByteStoreR0Indexed:
    case K::MovByteLoadR0Indexed:
        return 1u;
    case K::MovWordStore:
    case K::MovWordLoad:
    case K::MovWordStorePreDecrement:
    case K::MovWordLoadPostIncrement:
    case K::MovWordStoreDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovWordStoreR0Indexed:
    case K::MovWordLoadR0Indexed:
        return 2u;
    case K::MovLongStore:
    case K::MovLongLoad:
    case K::MovLongStorePreDecrement:
    case K::MovLongLoadPostIncrement:
    case K::MovLongStoreDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovLongStoreR0Indexed:
    case K::MovLongLoadR0Indexed:
        return 4u;
    default:
        return 0u;
    }
}

void add_candidate(std::vector<StoredCodeAddressCandidate>& candidates,
                   const katana::io::ExecutableImage& image,
                   const std::uint32_t value,
                   const std::uint32_t source_address) {
    const auto target = executable_constant(image, value);
    if (!target.has_value()) return;
    auto found = std::find_if(
        candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.target_address == *target;
        });
    if (found == candidates.end()) {
        StoredCodeAddressCandidate candidate;
        candidate.target_address = *target;
        candidate.complete = false;
        candidate.guarded = true;
        candidates.push_back(std::move(candidate));
        found = std::prev(candidates.end());
    }
    found->store_instruction_addresses.push_back(source_address);
}

void observe_loaded_static_descriptor_table(
    CallbackFunctionModel& model,
    const katana::io::ExecutableImage& image,
    const CallbackValue& loaded,
    const std::uint32_t load_instruction_address,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    if (loaded.constants_truncated) return;
    // A reachable identity-bound structure can carry a pointer to a callback
    // descriptor array which is copied through mutable title RAM before the
    // eventual indirect call. Recognize the table at the earlier static load
    // rather than requiring that mutable RAM to retain its post-bootstrap
    // value. The smallest valid 32-bit-aligned record stride wins; every
    // target remains guarded positive inventory and is shape-validated again
    // before publication.
    for (const auto table : loaded.constants) {
        if ((table & 3u) != 0u) continue;
        const auto first = read_image_u32(image, table);
        if (!first.has_value() ||
            !executable_constant(image, *first).has_value())
            continue;
        CallbackValue base;
        base.constants.insert(table);
        for (std::uint32_t stride = 4u;
             stride <= maximum_static_code_pointer_table_stride;
             stride += 4u) {
            CallbackValue byte_index;
            byte_index.minimum_alignment = stride;
            const auto entries = scan_static_code_pointer_table(
                image, base, byte_index, native_entry_shapes);
            if (entries.code_constants_truncated) {
                model.local_candidates_truncated = true;
                break;
            }
            if (entries.code_constants.empty()) continue;
            for (const auto target : entries.code_constants)
                add_candidate(model.local_candidates, image, target,
                              load_instruction_address);
            break;
        }
    }
}

void observe_persistent_store(CallbackFunctionModel& model,
                              const katana::io::ExecutableImage& image,
                              const CallbackValue& source,
                              const std::uint32_t instruction_address,
                              const std::size_t width) {
    if (width != 4u) return;
    model.local_sink_mask = static_cast<std::uint8_t>(
        model.local_sink_mask | source.input_mask);
    if (source.code_constants_truncated)
        model.local_candidates_truncated = true;
    // The field offset is derived from an actual indirect-call load. Preserve
    // bounded executable scalars through this local model; the terminal
    // inventory gate below still requires either a complete standalone shape
    // or an independent non-root function-entry hint before publication.
    for (const auto constant : source.constants)
        add_candidate(model.local_candidates, image, constant,
                      instruction_address);
    for (const auto constant : source.code_constants)
        add_candidate(model.local_candidates, image, constant,
                      instruction_address);
}

void store_value(CallbackFunctionModel& model,
                 CallbackState& state,
                 const katana::io::ExecutableImage& image,
                 const CallbackValue& source,
                 const CallbackValue& destination,
                 const std::int32_t displacement,
                 const std::uint32_t instruction_address,
                 const std::size_t width,
                 const std::set<std::int32_t>& callback_field_offsets) {
    const auto stack_address = displaced_stack_address(destination,
                                                       displacement);
    if (stack_address.has_value()) {
        erase_overlapping_stack_values(state, *stack_address, width);
        if (width == 4u && state.stack_values.size() < maximum_stack_values)
            state.stack_values.insert_or_assign(*stack_address, source);
        return;
    }
    // An imprecise SP-derived address may still be a spill. It cannot prove a
    // persistent callback store, so remain fail-closed and do not promote it.
    if (!destination.may_be_stack &&
        callback_field_offsets.contains(displacement))
        observe_persistent_store(model, image, source,
                                 instruction_address, width);
}

[[nodiscard]] CallbackValue load_value(const CallbackState& state,
                                       const CallbackValue& source,
                                       const std::int32_t displacement,
                                       const std::size_t width) {
    const auto stack_address = displaced_stack_address(source, displacement);
    if (stack_address.has_value() && width == 4u) {
        const auto found = state.stack_values.find(*stack_address);
        if (found != state.stack_values.end()) return found->second;
    }
    return {};
}

void apply_instruction(CallbackFunctionModel& model,
                       CallbackState& state,
                       const katana::io::ExecutableImage& image,
                       const katana::sh4::DisassemblyLine& line,
                       const std::set<std::int32_t>& callback_field_offsets,
                       GuardedNativeEntryShapeCache&
                           native_entry_shapes) {
    using K = katana::sh4::InstructionKind;
    const auto& instruction = line.instruction;
    const auto destination = instruction.destination_register;
    const auto source = instruction.source_register;
    const auto width = memory_width(instruction.kind);

    switch (instruction.kind) {
    case K::Nop:
    case K::Ocbp:
    case K::Ocbwb:
    case K::Bra:
    case K::Bsr:
    case K::Braf:
    case K::Bsrf:
    case K::Bt:
    case K::Bf:
    case K::BtS:
    case K::BfS:
    case K::Jmp:
    case K::Jsr:
    case K::Rts:
        return;
    case K::MovImmediate:
        state.registers[destination] = {};
        add_constant(state.registers[destination], image,
                     static_cast<std::uint32_t>(instruction.immediate),
                     native_entry_shapes);
        return;
    case K::MovRegister:
        state.registers[destination] = state.registers[source];
        return;
    case K::ExtendUnsignedByte:
        state.registers[destination] = state.registers[source];
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value & 0xFFu; });
        return;
    case K::ExtendUnsignedWord:
        state.registers[destination] = state.registers[source];
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value & 0xFFFFu; });
        return;
    case K::ExtendSignedByte:
        state.registers[destination] = state.registers[source];
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) {
                return static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(value)));
            });
        return;
    case K::ExtendSignedWord:
        state.registers[destination] = state.registers[source];
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) {
                return static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int16_t>(value)));
            });
        return;
    case K::ShiftLogicalLeftOne:
    case K::ShiftArithmeticLeftOne:
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 1u; });
        scale_minimum_alignment(state.registers[destination], 2u);
        return;
    case K::ShiftLogicalLeftTwo:
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 2u; });
        scale_minimum_alignment(state.registers[destination], 4u);
        return;
    case K::ShiftLogicalLeftEight:
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 8u; });
        scale_minimum_alignment(state.registers[destination], 256u);
        return;
    case K::ShiftLogicalLeftSixteen:
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 16u; });
        scale_minimum_alignment(state.registers[destination], 65'536u);
        return;
    case K::AddImmediate:
        transform_add_immediate(state.registers[destination], image,
                                native_entry_shapes,
                                instruction.immediate,
                                destination == 15u);
        return;
    case K::MoveAddressPcRelative: {
        state.registers[0u] = {};
        const auto value = ((line.address + 4u) & ~3u) +
                           static_cast<std::uint32_t>(instruction.displacement);
        add_constant(state.registers[0u], image, value,
                     native_entry_shapes);
        return;
    }
    case K::MovLongLoadPcRelative: {
        state.registers[destination] = {};
        const auto literal_address =
            ((line.address + 4u) & ~3u) +
            static_cast<std::uint32_t>(instruction.displacement);
        if (const auto value = read_image_u32(image, literal_address);
            value.has_value())
            add_constant(state.registers[destination], image, *value,
                         native_entry_shapes);
        // A PC-relative literal proves only the scalar pointer value. Treating
        // the pointed-to bytes as a descriptor table here misclassifies
        // ordinary strings and resources whose first four bytes happen to
        // canonicalize into executable P0/P1 RAM. The later static memory
        // dereference is the required evidence that this literal actually
        // leads to an identity-bound table pointer.
        return;
    }
    case K::MovWordLoadPcRelative:
        set_unknown(state.registers[destination]);
        return;
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
        store_value(model, state, image, state.registers[source],
                    state.registers[destination], 0,
                    line.address, width, callback_field_offsets);
        return;
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        store_value(model, state, image, state.registers[source],
                    state.registers[destination], instruction.displacement,
                    line.address, width, callback_field_offsets);
        return;
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed: {
        if (state.registers[0u].constants.size() != 1u ||
            state.registers[0u].constants_truncated) {
            if (!state.registers[destination].may_be_stack &&
                callback_field_offsets.contains(0))
                observe_persistent_store(model, image,
                                         state.registers[source],
                                         line.address, width);
            return;
        }
        store_value(model, state, image, state.registers[source],
                    state.registers[destination],
                    static_cast<std::int32_t>(
                        *state.registers[0u].constants.begin()),
                    line.address, width, callback_field_offsets);
        return;
    }
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement: {
        auto& base = state.registers[destination];
        transform_add_immediate(base, image, native_entry_shapes,
                                -static_cast<std::int32_t>(width),
                                destination == 15u);
        store_value(model, state, image, state.registers[source], base, 0,
                    line.address, width, callback_field_offsets);
        return;
    }
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad: {
        state.registers[destination] =
            load_value(state, state.registers[source], 0, width);
        if (width == 4u) {
            const auto loaded = load_static_image_values(
                image, state.registers[source], 0,
                native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, loaded, line.address, native_entry_shapes);
            static_cast<void>(join_value(state.registers[destination], loaded,
                                         native_entry_shapes));
        }
        return;
    }
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
        state.registers[destination] = load_value(
            state, state.registers[source], instruction.displacement, width);
        if (width == 4u) {
            const auto loaded = load_static_image_values(
                image, state.registers[source], instruction.displacement,
                native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, loaded, line.address, native_entry_shapes);
            static_cast<void>(join_value(state.registers[destination], loaded,
                                         native_entry_shapes));
        }
        return;
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
        if (state.registers[0u].constants.size() == 1u &&
            !state.registers[0u].constants_truncated)
            state.registers[destination] = load_value(
                state, state.registers[source],
                static_cast<std::int32_t>(
                    *state.registers[0u].constants.begin()),
                width);
        else
            set_unknown(state.registers[destination]);
        if (width == 4u) {
            const auto loaded = load_static_indexed_value(
                image, state.registers[0u], state.registers[source],
                native_entry_shapes);
            static_cast<void>(join_value(state.registers[destination], loaded,
                                         native_entry_shapes));
        }
        return;
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement: {
        auto loaded = load_value(state, state.registers[source], 0, width);
        if (width == 4u) {
            const auto static_loaded = load_static_image_values(
                image, state.registers[source], 0, native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, static_loaded, line.address,
                native_entry_shapes);
            static_cast<void>(join_value(loaded, static_loaded,
                                         native_entry_shapes));
        }
        transform_add_immediate(state.registers[source], image,
                                native_entry_shapes,
                                static_cast<std::int32_t>(width),
                                source == 15u);
        state.registers[destination] = loaded;
        return;
    }
    case K::StoreSpecialRegisterPreDecrement: {
        auto& base = state.registers[destination];
        transform_add_immediate(base, image, native_entry_shapes, -4,
                                destination == 15u);
        if (const auto address = displaced_stack_address(base, 0);
            address.has_value())
            erase_overlapping_stack_values(state, *address, 4u);
        return;
    }
    case K::LoadSpecialRegisterPostIncrement:
        transform_add_immediate(state.registers[source], image,
                                native_entry_shapes, 4,
                                source == 15u);
        return;
    default:
        break;
    }

    const auto writes = general_register_write_mask(instruction);
    for (std::uint8_t index = 0u; index < state.registers.size(); ++index) {
        if ((writes & static_cast<std::uint16_t>(1u << index)) == 0u)
            continue;
        set_unknown(state.registers[index], index == 15u);
        if (index == 15u) state.registers[index].may_be_stack = true;
    }
}

[[nodiscard]] std::vector<std::uint32_t> call_targets(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& control,
    const CallbackState& before_delay,
    const std::set<std::uint32_t>& function_entries) {
    std::set<std::uint32_t> targets;
    if (control.target_address.has_value())
        targets.insert(*control.target_address);
    const auto kind = control.instruction.kind;
    if (kind == katana::sh4::InstructionKind::Jsr ||
        kind == katana::sh4::InstructionKind::Jmp) {
        const auto& branch =
            before_delay.registers[control.instruction.branch_register];
        if (!branch.code_constants_truncated)
            targets.insert(branch.code_constants.begin(),
                           branch.code_constants.end());
    }
    std::vector<std::uint32_t> result;
    for (const auto target : targets) {
        const auto resolved = executable_constant(image, target);
        if (resolved.has_value() && function_entries.contains(*resolved))
            result.push_back(*resolved);
    }
    return result;
}

void merge_call(CallbackFunctionModel& model,
                const CallbackCall& incoming,
                GuardedNativeEntryShapeCache& native_entry_shapes) {
    auto found = std::find_if(
        model.calls.begin(), model.calls.end(), [&](const auto& call) {
            return call.instruction_address == incoming.instruction_address &&
                   call.callee == incoming.callee;
        });
    if (found == model.calls.end()) {
        model.calls.push_back(incoming);
        return;
    }
    for (std::size_t index = 0u; index < found->arguments.size(); ++index)
        static_cast<void>(join_value(found->arguments[index],
                                     incoming.arguments[index],
                                     native_entry_shapes));
}

void clobber_call_volatile_registers(CallbackState& state) {
    for (std::size_t index = 0u; index <= 7u; ++index)
        state.registers[index] = {};
}

[[nodiscard]] CallbackFunctionModel build_function_model(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    const std::set<std::uint32_t>& function_entries,
    const std::set<std::int32_t>& callback_field_offsets,
    GuardedNativeEntryShapeCache& native_entry_shapes,
    std::size_t* const limited_evaluations) {
    CallbackFunctionModel model;
    model.entry = function.entry_address;
    const auto entry = blocks.find(function.entry_address);
    if (entry == blocks.end()) return model;

    CallbackState initial;
    for (std::uint8_t index = 0u; index < 4u; ++index)
        initial.registers[4u + index].input_mask =
            static_cast<std::uint8_t>(1u << index);
    initial.registers[15u].stack_address = 0;
    initial.registers[15u].may_be_stack = true;

    std::map<std::uint32_t, CallbackState> ingress;
    ingress.emplace(function.entry_address, initial);
    std::deque<std::uint32_t> pending{function.entry_address};
    std::set<std::uint32_t> queued{function.entry_address};
    const std::set<std::uint32_t> owned(function.block_addresses.begin(),
                                        function.block_addresses.end());
    // A block can gain four ABI-taint bits plus multiple independently
    // bounded scalar/code facts before reaching its monotone fixed point.
    // Thirty-two visits was below that lattice height for large SDK state
    // dispatchers and falsely turned a finite positive inventory into Top.
    const auto evaluation_budget =
        std::max<std::size_t>(256u, function.block_addresses.size() * 128u);
    std::size_t evaluations = 0u;

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        if (++evaluations > evaluation_budget) {
            ++*limited_evaluations;
            break;
        }
        const auto block = blocks.find(address);
        const auto state_it = ingress.find(address);
        if (block == blocks.end() || state_it == ingress.end()) continue;
        CallbackState state = state_it->second;
        const auto& lines = block->second->lines;

        const katana::sh4::DisassemblyLine* control = nullptr;
        CallbackState before_delay;
        std::vector<std::uint32_t> targets;
        for (const auto& line : lines) {
            const auto flow = line.instruction.control_flow;
            const bool call = flow == katana::sh4::ControlFlowKind::Call ||
                              flow == katana::sh4::ControlFlowKind::IndirectCall;
            const bool tail =
                flow == katana::sh4::ControlFlowKind::UnconditionalBranch ||
                flow == katana::sh4::ControlFlowKind::IndirectBranch;
            if (call || tail) {
                control = &line;
                before_delay = state;
                targets = call_targets(image, line, before_delay,
                                       function_entries);
                continue;
            }
            apply_instruction(model, state, image, line,
                              callback_field_offsets,
                              native_entry_shapes);
        }

        if (control != nullptr && !targets.empty()) {
            for (const auto callee : targets) {
                CallbackCall call;
                call.instruction_address = control->address;
                call.callee = callee;
                for (std::size_t index = 0u; index < call.arguments.size();
                     ++index)
                    call.arguments[index] = state.registers[4u + index];
                merge_call(model, call, native_entry_shapes);
            }
        }
        if (control != nullptr &&
            (control->instruction.control_flow ==
                 katana::sh4::ControlFlowKind::Call ||
             control->instruction.control_flow ==
                 katana::sh4::ControlFlowKind::IndirectCall))
            clobber_call_volatile_registers(state);

        for (const auto successor : block->second->successors) {
            if (!owned.contains(successor)) continue;
            const auto [next, inserted] = ingress.emplace(successor, state);
            const bool changed =
                !inserted && join_state(next->second, state,
                                        native_entry_shapes);
            if ((inserted || changed) && queued.insert(successor).second)
                pending.push_back(successor);
        }
    }
    return model;
}

void canonicalize_candidate(StoredCodeAddressCandidate& candidate) {
    auto canonicalize = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    canonicalize(candidate.store_instruction_addresses);
    canonicalize(candidate.evidence_call_sites);
    canonicalize(candidate.evidence_callees);
}

} // namespace

GuardedCodeInventory analyze_static_callback_inventory(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionCandidate> function_candidates,
    const std::span<const std::uint32_t> external_block_entries,
    const std::span<const std::uint32_t> non_root_function_entry_hints,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    GuardedCodeInventory inventory;
    inventory.raw_stored_candidate_budget = maximum_inventory_candidates;
    inventory.candidate_budget = maximum_inventory_candidates;
    if (lines.empty() ||
        (function_candidates.empty() && external_block_entries.empty()))
        return inventory;

    std::vector<FunctionBoundary> boundaries;
    boundaries.reserve(function_candidates.size());
    for (const auto& candidate : function_candidates)
        boundaries.push_back({candidate.address, candidate.size});
    std::sort(boundaries.begin(), boundaries.end(),
              [](const auto& left, const auto& right) {
                  return left.entry_address < right.entry_address;
              });
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.entry_address ==
                                            right.entry_address;
                                 }),
                     boundaries.end());

    if (!std::is_sorted(external_block_entries.begin(),
                        external_block_entries.end()) ||
        std::adjacent_find(external_block_entries.begin(),
                           external_block_entries.end()) !=
            external_block_entries.end())
        throw std::invalid_argument(
            "Externe Callback-Blockeinstiege sind nicht kanonisch.");

    std::vector<std::uint32_t> leaders;
    leaders.reserve(boundaries.size() * 2u + external_block_entries.size());
    for (const auto& boundary : boundaries) {
        leaders.push_back(boundary.entry_address);
        if (boundary.size != 0u &&
            boundary.entry_address <=
                std::numeric_limits<std::uint32_t>::max() - boundary.size)
            leaders.push_back(boundary.entry_address + boundary.size);
    }
    leaders.insert(leaders.end(), external_block_entries.begin(),
                   external_block_entries.end());
    const auto blocks = build_basic_blocks(lines, {}, leaders);
    auto functions = discover_functions_from_blocks(blocks, boundaries);
    // A guarded callback or continuation can enter a disconnected CFG
    // component without constituting descriptive function metadata. Such a
    // component is already an identity-bound external execution surface, but
    // ordinary function discovery cannot own it until its registrar has been
    // analyzed. Give only genuinely unowned external roots a local analysis
    // owner; interior entries retain their existing owner.
    std::set<std::uint32_t> owned_blocks;
    for (const auto& function : functions)
        owned_blocks.insert(function.block_addresses.begin(),
                            function.block_addresses.end());
    bool supplemented = false;
    for (const auto entry : external_block_entries) {
        if (owned_blocks.contains(entry)) continue;
        const auto block = std::find_if(
            blocks.begin(), blocks.end(),
            [&](const auto& candidate) {
                return candidate.start_address == entry;
            });
        if (block == blocks.end())
            throw std::invalid_argument(
                "Externer Callback-Blockeinstieg wurde nicht "
                "materialisiert: " +
                std::to_string(entry));
        boundaries.push_back({entry, 0u});
        supplemented = true;
    }
    if (supplemented) {
        std::sort(boundaries.begin(), boundaries.end(),
                  [](const auto& left, const auto& right) {
                      return left.entry_address < right.entry_address;
                  });
        boundaries.erase(
            std::unique(boundaries.begin(), boundaries.end(),
                        [](const auto& left, const auto& right) {
                            return left.entry_address == right.entry_address;
                        }),
            boundaries.end());
        functions = discover_functions_from_blocks(blocks, boundaries);
    }
    const auto callback_field_offsets =
        discover_callback_field_offsets(lines);

    std::unordered_map<std::uint32_t, const BasicBlock*> block_index;
    block_index.reserve(blocks.size());
    for (const auto& block : blocks)
        block_index.emplace(block.start_address, &block);
    std::set<std::uint32_t> function_entries;
    for (const auto& function : functions)
        function_entries.insert(function.entry_address);

    std::size_t limited_evaluations = 0u;
    bool candidate_values_truncated = false;
    bool forwarding_truncated = false;
    std::map<std::uint32_t, CallbackFunctionModel> models;
    for (const auto& function : functions) {
        auto model = build_function_model(image, function, block_index,
                                          function_entries,
                                          callback_field_offsets,
                                          native_entry_shapes,
                                          &limited_evaluations);
        models.insert_or_assign(function.entry_address, std::move(model));
    }

    std::map<std::uint32_t, std::uint8_t> sink_masks;
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::size_t>>>
        callers_by_callee;
    std::deque<std::uint32_t> pending;
    std::set<std::uint32_t> queued;
    for (const auto& [entry, model] : models) {
        sink_masks[entry] = model.local_sink_mask;
        if (model.local_sink_mask != 0u && queued.insert(entry).second)
            pending.push_back(entry);
        for (std::size_t index = 0u; index < model.calls.size(); ++index)
            callers_by_callee[model.calls[index].callee].push_back(
                {entry, index});
    }
    std::size_t propagation_steps = 0u;
    const auto propagation_budget =
        std::max<std::size_t>(64u, models.size() * 8u);
    while (!pending.empty()) {
        const auto callee = pending.front();
        pending.pop_front();
        queued.erase(callee);
        if (++propagation_steps > propagation_budget) {
            forwarding_truncated = true;
            break;
        }
        const auto sink = sink_masks[callee];
        const auto callers = callers_by_callee.find(callee);
        if (callers == callers_by_callee.end()) continue;
        for (const auto& [caller, call_index] : callers->second) {
            const auto model = models.find(caller);
            if (model == models.end() || call_index >= model->second.calls.size())
                continue;
            std::uint8_t propagated = 0u;
            const auto& call = model->second.calls[call_index];
            for (std::size_t argument = 0u; argument < 4u; ++argument) {
                if ((sink & static_cast<std::uint8_t>(1u << argument)) == 0u)
                    continue;
                propagated = static_cast<std::uint8_t>(
                    propagated | call.arguments[argument].input_mask);
            }
            auto& caller_sink = sink_masks[caller];
            const auto next = static_cast<std::uint8_t>(caller_sink |
                                                        propagated);
            if (next == caller_sink) continue;
            caller_sink = next;
            if (queued.insert(caller).second) pending.push_back(caller);
        }
    }

    std::map<std::uint32_t, StoredCodeAddressCandidate> candidates;
    std::size_t raw_candidates = 0u;
    const auto merge_candidate = [&](const StoredCodeAddressCandidate& source) {
        ++raw_candidates;
        auto& destination = candidates[source.target_address];
        destination.target_address = source.target_address;
        destination.complete = false;
        destination.guarded = true;
        destination.store_instruction_addresses.insert(
            destination.store_instruction_addresses.end(),
            source.store_instruction_addresses.begin(),
            source.store_instruction_addresses.end());
        destination.evidence_call_sites.insert(
            destination.evidence_call_sites.end(),
            source.evidence_call_sites.begin(),
            source.evidence_call_sites.end());
        destination.evidence_callees.insert(
            destination.evidence_callees.end(),
            source.evidence_callees.begin(),
            source.evidence_callees.end());
    };
    for (const auto& [entry, model] : models) {
        static_cast<void>(entry);
        for (const auto& candidate : model.local_candidates)
            merge_candidate(candidate);
        candidate_values_truncated =
            candidate_values_truncated ||
            model.local_candidates_truncated;
        for (const auto& call : model.calls) {
            const auto sink = sink_masks.find(call.callee);
            if (sink == sink_masks.end()) continue;
            for (std::size_t argument = 0u; argument < 4u; ++argument) {
                if ((sink->second & static_cast<std::uint8_t>(1u << argument)) ==
                    0u)
                    continue;
                std::set<std::uint32_t> forwarded_constants(
                    call.arguments[argument].constants.begin(),
                    call.arguments[argument].constants.end());
                forwarded_constants.insert(
                    call.arguments[argument].code_constants.begin(),
                    call.arguments[argument].code_constants.end());
                for (const auto constant : forwarded_constants) {
                    const auto target = executable_constant(image, constant);
                    if (!target.has_value()) continue;
                    StoredCodeAddressCandidate candidate;
                    candidate.target_address = *target;
                    candidate.guarded = true;
                    candidate.evidence_call_sites = {
                        call.instruction_address};
                    candidate.evidence_callees = {call.callee};
                    merge_candidate(candidate);
                }
                if (call.arguments[argument].code_constants_truncated)
                    candidate_values_truncated = true;
            }
        }
    }

    if (!std::is_sorted(non_root_function_entry_hints.begin(),
                        non_root_function_entry_hints.end()) ||
        std::adjacent_find(non_root_function_entry_hints.begin(),
                           non_root_function_entry_hints.end()) !=
            non_root_function_entry_hints.end())
        throw std::invalid_argument(
            "Nicht-rootende Funktionseinstiegshinweise sind nicht "
            "kanonisch.");
    for (auto candidate = candidates.begin(); candidate != candidates.end();) {
        const auto independently_bound = std::binary_search(
            non_root_function_entry_hints.begin(),
            non_root_function_entry_hints.end(), candidate->first);
        if (!independently_bound &&
            native_entry_shapes.classify(candidate->first) !=
                GuardedNativeEntryShapeStatus::Valid) {
            candidate = candidates.erase(candidate);
            continue;
        }
        ++candidate;
    }

    inventory.raw_stored_candidate_count = raw_candidates;
    inventory.candidate_count = candidates.size();
    const bool candidate_budget_truncated =
        raw_candidates > maximum_inventory_candidates ||
        candidates.size() > maximum_inventory_candidates;
    const bool truncated = limited_evaluations != 0u ||
                           candidate_values_truncated ||
                           forwarding_truncated ||
                           candidate_budget_truncated;
    inventory.walk_diagnostics.local_fixpoint_limited_evaluations =
        limited_evaluations;
    inventory.walk_diagnostics.inventory_candidate_values_truncated =
        candidate_values_truncated;
    inventory.walk_diagnostics.forwarded_store_context_limited_functions =
        forwarding_truncated ? 1u : 0u;
    inventory.raw_stored_candidates_truncated = truncated;
    inventory.candidate_budget_exhausted =
        candidates.size() > maximum_inventory_candidates;
    inventory.candidate_inventory_truncated = truncated;
    inventory.stored_code_addresses.reserve(
        std::min(candidates.size(), maximum_inventory_candidates));
    for (auto& [target, candidate] : candidates) {
        static_cast<void>(target);
        if (inventory.stored_code_addresses.size() >=
            maximum_inventory_candidates)
            break;
        canonicalize_candidate(candidate);
        inventory.stored_code_addresses.push_back(std::move(candidate));
    }
    return inventory;
}

} // namespace katana::analysis::detail
