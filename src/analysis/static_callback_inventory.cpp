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
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis::detail {
namespace {

constexpr std::size_t maximum_scalar_constants = 8u;
// A typed indexed callback table may contain up to 256 identity-bound
// entries.  The executable-value lane must be able to carry one complete
// admitted table; using the old 64-value scalar-oriented cap made an otherwise
// bounded 65..256-entry table report semantic inventory loss even while the
// analysis-wide 16K candidate budget was mostly empty.
constexpr std::size_t maximum_code_constants = 256u;
constexpr std::size_t maximum_receiver_constants = 256u;
constexpr std::size_t maximum_receiver_progressions = 64u;
constexpr std::uint64_t maximum_receiver_progression_values = 256u;
constexpr std::size_t maximum_inventory_candidates = 16'384u;
constexpr std::size_t maximum_stack_values = 256u;
constexpr std::size_t maximum_callback_field_origins = 256u;
constexpr std::size_t maximum_persistent_store_observations = 4096u;
constexpr std::size_t minimum_static_code_pointer_vector_entries = 4u;
constexpr std::size_t maximum_static_code_pointer_table_entries = 256u;
constexpr std::uint32_t maximum_static_code_pointer_table_stride = 256u;

struct ReceiverProgression final {
    std::uint32_t first = 0u;
    std::uint32_t last = 0u;
    std::uint32_t stride = 1u;

    bool operator==(const ReceiverProgression&) const = default;
};

struct CallbackFieldOrigin final {
    // Receiver provenance is relative to the owning function. Exact receiver
    // constants remain comparable across functions; incoming ABI argument
    // bits are comparable only inside the same owner.
    std::uint8_t receiver_input_mask = 0u;
    std::set<std::uint32_t> receiver_constants;
    std::vector<ReceiverProgression> receiver_progressions;
    bool receiver_constants_truncated = false;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    std::uint32_t load_instruction_address = 0u;

    bool operator==(const CallbackFieldOrigin&) const = default;
};

struct CallbackAffineInput final {
    // Zero-based ABI argument index: r4..r7.
    std::uint8_t argument = 0u;
    std::uint32_t scale = 1u;

    bool operator==(const CallbackAffineInput&) const = default;
};

struct CallbackPcLiteralIdentity final {
    std::uint32_t literal_address = 0u;
    std::uint32_t value = 0u;

    bool operator==(const CallbackPcLiteralIdentity&) const = default;
};

struct CallbackValue final {
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t input_mask = 0u;
    std::set<std::uint32_t> constants;
    bool constants_truncated = false;
    // True only when constants is the complete finite scalar domain on every
    // path represented by this value. Ordinary positive constants remain
    // useful without this bit, but caller-bounded closure must not consume
    // them as an exhaustive argument domain.
    bool constants_complete = false;
    // Keep executable values in a separate lane. Ordinary scalar joins (mode
    // IDs, sizes, flags, offsets, and table indices) may legitimately widen
    // long before the much smaller callback-address domain does. Letting that
    // ordinary widening poison the code lane made the positive inventory both
    // incomplete and needlessly broad.
    std::set<std::uint32_t> code_constants;
    bool code_constants_truncated = false;
    // Exhaustive executable-value domain. This is intentionally stricter
    // than the positive code lane and is established only by exact literal
    // loads (and lossless joins of such loads).
    bool code_constants_complete = false;
    // Record/object receiver identity is a separate proof lane.  Eight
    // ordinary scalars are enough for modes and sizes but not for a function
    // that selects among many identity-bound task records.  Mixing those
    // domains previously erased exact store/load receiver relations.
    std::set<std::uint32_t> receiver_constants;
    std::vector<ReceiverProgression> receiver_progressions;
    bool receiver_constants_truncated = false;
    // A 32-bit value loaded from non-stack memory may be a record/object
    // receiver even when mutable title RAM prevents the static image from
    // naming its concrete address. Keep that provenance distinct from a
    // wholly unknown scalar so an indirect load through the value can publish
    // a conservative callback-field shape. The shape remains positive,
    // guarded inventory and never completes the dynamic target set.
    bool memory_derived_receiver = false;
    // Guaranteed power-of-two byte alignment for an otherwise unknown
    // scalar. This keeps the shape of mutable table indices through SHLL
    // operations without pretending that the index value itself is known.
    std::uint32_t minimum_alignment = 1u;
    // Narrow provenance used only by caller-bounded indexed stores. Any
    // arithmetic other than a checked constant left shift drops this proof.
    std::optional<CallbackAffineInput> affine_input;
    // Identity of a value loaded directly from a PC-relative literal slot.
    // Keeping the slot as well as the scalar prevents a merely plausible RAM
    // address from being treated as an identity-bound persistent table.
    std::optional<CallbackPcLiteralIdentity> pc_literal_identity;
    std::optional<std::int32_t> stack_address;
    bool may_be_stack = false;
    std::vector<CallbackFieldOrigin> field_origins;
    bool field_origins_truncated = false;

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

struct CallbackFieldSink final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    CallbackFieldOrigin field;
    bool call = false;

    bool operator==(const CallbackFieldSink&) const = default;
};

struct CallbackPersistentStore final {
    CallbackValue source;
    CallbackValue receiver;
    std::int32_t displacement = 0;
    std::uint32_t instruction_address = 0u;
    std::uint8_t width = 0u;
    bool indexed_addressing = false;

    bool operator==(const CallbackPersistentStore&) const = default;
};

struct CallbackIndexedPersistentStore final {
    CallbackValue source;
    std::optional<CallbackPcLiteralIdentity> base;
    std::optional<CallbackAffineInput> byte_offset;
    std::uint32_t instruction_address = 0u;
    std::uint8_t width = 0u;
    bool destination_identity_complete = false;

    bool operator==(const CallbackIndexedPersistentStore&) const = default;
};

struct CallbackFunctionModel final {
    std::uint32_t entry = 0u;
    std::uint8_t local_sink_mask = 0u;
    std::uint8_t local_persistent_pointer_mask = 0u;
    std::vector<CallbackCall> calls;
    std::vector<CallbackFieldSink> field_sinks;
    std::vector<CallbackPersistentStore> persistent_stores;
    std::map<std::uint32_t, CallbackIndexedPersistentStore>
        indexed_persistent_stores;
    std::vector<StoredCodeAddressCandidate> local_candidates;
    bool local_candidates_truncated = false;
    bool field_sinks_truncated = false;
    bool persistent_stores_truncated = false;
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

[[nodiscard]] std::vector<StaticCallbackFieldSinkContract>
discover_structural_callback_field_sinks(
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks) {
    std::vector<StaticCallbackFieldSinkContract> result;
    constexpr std::size_t maximum_writer_distance = 32u;
    for (const auto block_address : function.block_addresses) {
        const auto block = blocks.find(block_address);
        if (block == blocks.end()) continue;
        const auto& lines = block->second->lines;
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
                const auto writes =
                    general_register_write_mask(candidate.instruction);
                if ((writes & static_cast<std::uint16_t>(1u << tracked)) ==
                    0u)
                    continue;
                if (candidate.instruction.destination_register != tracked)
                    break;
                if (candidate.instruction.kind ==
                    katana::sh4::InstructionKind::MovRegister) {
                    tracked = candidate.instruction.source_register;
                    continue;
                }
                std::optional<std::int32_t> displacement;
                if (candidate.instruction.kind ==
                    katana::sh4::InstructionKind::MovLongLoadDisplacement)
                    displacement = candidate.instruction.displacement;
                else if (candidate.instruction.kind ==
                         katana::sh4::InstructionKind::MovLongLoad)
                    displacement = 0;
                if (displacement.has_value())
                    result.push_back(
                        {function.entry_address,
                         lines[index].address,
                         candidate.address,
                         *displacement,
                         static_cast<std::uint8_t>(4u),
                         kind == katana::sh4::InstructionKind::Jsr});
                break;
            }
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.function_address,
                                  left.call_instruction_address,
                                  left.load_instruction_address,
                                  left.displacement,
                                  left.width,
                                  left.call) <
                         std::tie(right.function_address,
                                  right.call_instruction_address,
                                  right.load_instruction_address,
                                  right.displacement,
                                  right.width,
                                  right.call);
              });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
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

[[nodiscard]] bool plausible_receiver_constant(
    const katana::io::ExecutableImage& image,
    const std::uint32_t constant) {
    const auto physical = constant & 0x1FFFFFFFu;
    return (physical >= 0x0C000000u && physical < 0x10000000u) ||
           image.resolve_segment_address(constant, 1u).has_value();
}

[[nodiscard]] bool receiver_progression_contains(
    const ReceiverProgression& progression,
    const std::uint32_t constant) {
    return constant >= progression.first && constant <= progression.last &&
           (constant - progression.first) % progression.stride == 0u;
}

[[nodiscard]] bool add_receiver_progression(
    std::vector<ReceiverProgression>& progressions,
    ReceiverProgression progression,
    bool* const changed = nullptr) {
    if (progression.stride == 0u || progression.first > progression.last)
        throw std::invalid_argument(
            "Ungueltige Callback-Receiver-Progression.");
    if (changed != nullptr) *changed = false;
    const auto progression_value_count = [](const ReceiverProgression& value) {
        return (static_cast<std::uint64_t>(value.last) - value.first) /
                   value.stride +
               1u;
    };
    if (progression_value_count(progression) >
        maximum_receiver_progression_values)
        return false;
    for (auto& existing : progressions) {
        if (existing.stride != progression.stride ||
            (existing.first % existing.stride) !=
                (progression.first % progression.stride))
            continue;
        const auto separated_after =
            static_cast<std::uint64_t>(progression.first) >
            static_cast<std::uint64_t>(existing.last) + existing.stride;
        const auto separated_before =
            static_cast<std::uint64_t>(existing.first) >
            static_cast<std::uint64_t>(progression.last) + progression.stride;
        if (separated_after || separated_before) continue;
        ReceiverProgression merged{
            std::min(existing.first, progression.first),
            std::max(existing.last, progression.last), existing.stride};
        // A loop-carried address recurrence can otherwise extend an exact
        // progression by one record forever.  Widen it to the explicit loss
        // state as soon as it exceeds the same finite identity inventory that
        // exact receiver constants use.  This keeps the analysis monotone and
        // fail-closed without turning the local work budget into semantics.
        if (progression_value_count(merged) >
            maximum_receiver_progression_values)
            return false;
        if (existing == merged) return true;
        existing = merged;
        if (changed != nullptr) *changed = true;
        return true;
    }
    if (progressions.size() >= maximum_receiver_progressions) return false;
    progressions.push_back(progression);
    if (changed != nullptr) *changed = true;
    return true;
}

[[nodiscard]] bool compact_receiver_constants(CallbackValue& value,
                                               const std::uint32_t added) {
    std::vector<std::uint32_t> values(value.receiver_constants.begin(),
                                      value.receiver_constants.end());
    values.push_back(added);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    std::set<std::uint32_t> residual;
    std::size_t index = 0u;
    while (index < values.size()) {
        if (index + 2u >= values.size()) {
            residual.insert(values.begin() +
                                static_cast<std::ptrdiff_t>(index),
                            values.end());
            break;
        }
        const auto stride = values[index + 1u] - values[index];
        if (stride == 0u ||
            values[index + 2u] - values[index + 1u] != stride) {
            residual.insert(values[index]);
            ++index;
            continue;
        }
        auto end = index + 2u;
        while (end + 1u < values.size() &&
               values[end + 1u] - values[end] == stride)
            ++end;
        if (!add_receiver_progression(
                value.receiver_progressions,
                {values[index], values[end], stride}))
            return false;
        index = end + 1u;
    }
    for (auto constant = residual.begin(); constant != residual.end();) {
        const auto covered = std::any_of(
            value.receiver_progressions.begin(),
            value.receiver_progressions.end(),
            [&](const auto& progression) {
                return receiver_progression_contains(progression, *constant);
            });
        if (covered)
            constant = residual.erase(constant);
        else
            ++constant;
    }
    if (residual.size() > maximum_receiver_constants) return false;
    value.receiver_constants = std::move(residual);
    return true;
}

enum class ReceiverIntersection : std::uint8_t {
    None,
    Exact,
    Unproven,
};

[[nodiscard]] ReceiverIntersection receiver_progressions_intersect(
    const ReceiverProgression& left,
    const ReceiverProgression& right) {
    const auto begin = std::max(left.first, right.first);
    const auto end = std::min(left.last, right.last);
    if (begin > end) return ReceiverIntersection::None;
    const auto gcd = std::gcd(left.stride, right.stride);
    const auto delta = left.first > right.first
                           ? left.first - right.first
                           : right.first - left.first;
    if (delta % gcd != 0u) return ReceiverIntersection::None;
    if (left.stride == right.stride &&
        left.first % left.stride == right.first % right.stride)
        return ReceiverIntersection::Exact;

    constexpr auto maximum_exact_intersection_checks =
        maximum_receiver_progression_values;
    const auto left_count =
        (static_cast<std::uint64_t>(left.last) - left.first) / left.stride +
        1u;
    const auto right_count =
        (static_cast<std::uint64_t>(right.last) - right.first) /
            right.stride +
        1u;
    const auto* smaller = &left;
    const auto* larger = &right;
    if (right_count < left_count) {
        smaller = &right;
        larger = &left;
    }
    if (std::min(left_count, right_count) >
        maximum_exact_intersection_checks)
        return ReceiverIntersection::Unproven;
    for (std::uint64_t value = smaller->first; value <= smaller->last;
         value += smaller->stride) {
        if (receiver_progression_contains(
                *larger, static_cast<std::uint32_t>(value)))
            return ReceiverIntersection::Exact;
    }
    return ReceiverIntersection::None;
}

[[nodiscard]] ReceiverIntersection receiver_evidence_intersects(
    const CallbackFieldOrigin& sink,
    const CallbackValue& store_receiver) {
    // A truncated receiver lane still contains a sound exact subset.  Check
    // that subset before reporting the omitted remainder as unproven; clearing
    // or ignoring it loses real store/load relations merely because another
    // path widened beyond the bounded inventory.
    for (const auto constant : sink.receiver_constants) {
        if (store_receiver.receiver_constants.contains(constant))
            return ReceiverIntersection::Exact;
        if (std::any_of(store_receiver.receiver_progressions.begin(),
                        store_receiver.receiver_progressions.end(),
                        [&](const auto& progression) {
                            return receiver_progression_contains(progression,
                                                                 constant);
                        }))
            return ReceiverIntersection::Exact;
    }
    for (const auto constant : store_receiver.receiver_constants) {
        if (std::any_of(sink.receiver_progressions.begin(),
                        sink.receiver_progressions.end(),
                        [&](const auto& progression) {
                            return receiver_progression_contains(progression,
                                                                 constant);
                        }))
            return ReceiverIntersection::Exact;
    }
    bool unproven = false;
    for (const auto& left : sink.receiver_progressions) {
        for (const auto& right : store_receiver.receiver_progressions) {
            const auto intersection =
                receiver_progressions_intersect(left, right);
            if (intersection == ReceiverIntersection::Exact)
                return intersection;
            unproven = unproven ||
                       intersection == ReceiverIntersection::Unproven;
        }
    }
    if (sink.receiver_constants_truncated ||
        store_receiver.receiver_constants_truncated)
        return ReceiverIntersection::Unproven;
    return unproven ? ReceiverIntersection::Unproven
                    : ReceiverIntersection::None;
}

[[nodiscard]] bool same_local_record_receiver(
    const CallbackValue& left,
    const CallbackValue& right) {
    // A load instruction plus the identity of its addressed field is a local
    // SSA-like receiver origin.  Requiring the complete origin vector keeps
    // two unrelated heap/list loads in one function distinct even when both
    // values have otherwise widened to "memory-derived receiver".
    return left.memory_derived_receiver && right.memory_derived_receiver &&
           !left.field_origins_truncated &&
           !right.field_origins_truncated &&
           !left.field_origins.empty() &&
           left.field_origins == right.field_origins;
}

[[nodiscard]] bool exact_executable_default_store(
    const CallbackPersistentStore& store) {
    // An exact PC-relative executable default written to one record field is
    // independent type evidence for alternative incoming values written to
    // that same field.  This remains positive callback inventory only: it
    // neither completes the later indirect target set nor authorizes runtime
    // dispatch without the usual image/entry/byte-identity checks.
    return !store.indexed_addressing && store.source.input_mask == 0u &&
           store.source.code_constants_complete &&
           !store.source.code_constants_truncated &&
           !store.source.code_constants.empty();
}

[[nodiscard]] bool add_bounded_receiver_constant(
    CallbackValue& value,
    const std::uint32_t constant) {
    // Truncation is the explicit receiver-Top state.  Once reached, further
    // exact witnesses cannot make the conservative lattice value more
    // precise and must not repeatedly trigger the expensive compact/sort
    // path on every fixpoint visit.
    if (value.receiver_constants_truncated ||
        value.receiver_constants.contains(constant) ||
        std::any_of(value.receiver_progressions.begin(),
                    value.receiver_progressions.end(),
                    [&](const auto& progression) {
                        return receiver_progression_contains(progression,
                                                             constant);
                    }))
        return false;
    if (value.receiver_constants.size() >= maximum_receiver_constants) {
        if (!compact_receiver_constants(value, constant))
            value.receiver_constants_truncated = true;
        return true;
    }
    value.receiver_constants.insert(constant);
    return true;
}

void add_constant(CallbackValue& value,
                  const katana::io::ExecutableImage& image,
                  const std::uint32_t constant,
                  GuardedNativeEntryShapeCache& native_entry_shapes) {
    static_cast<void>(add_bounded_scalar_constant(value, constant));
    if (plausible_receiver_constant(image, constant))
        static_cast<void>(add_bounded_receiver_constant(value, constant));
    if (const auto target = executable_constant(image, constant);
        target.has_value())
        static_cast<void>(add_bounded_code_constant(
            value, *target, native_entry_shapes));
}

[[nodiscard]] bool merge_callback_field_origin(
    CallbackFieldOrigin& destination,
    const CallbackFieldOrigin& source) {
    bool changed = false;
    const auto input_mask = static_cast<std::uint8_t>(
        destination.receiver_input_mask | source.receiver_input_mask);
    changed = changed || input_mask != destination.receiver_input_mask;
    destination.receiver_input_mask = input_mask;
    if (destination.receiver_constants_truncated ||
        source.receiver_constants_truncated) {
        changed = changed || !destination.receiver_constants_truncated;
        destination.receiver_constants_truncated = true;
        return changed;
    }
    CallbackValue combined;
    combined.receiver_constants = destination.receiver_constants;
    combined.receiver_progressions = destination.receiver_progressions;
    for (const auto& progression : source.receiver_progressions) {
        if (!add_receiver_progression(combined.receiver_progressions,
                                      progression)) {
            combined.receiver_constants_truncated = true;
            break;
        }
    }
    for (const auto constant : source.receiver_constants)
        static_cast<void>(add_bounded_receiver_constant(combined, constant));
    changed = changed ||
              destination.receiver_constants != combined.receiver_constants ||
              destination.receiver_progressions !=
                  combined.receiver_progressions ||
              destination.receiver_constants_truncated !=
                  combined.receiver_constants_truncated;
    destination.receiver_constants = std::move(combined.receiver_constants);
    destination.receiver_progressions =
        std::move(combined.receiver_progressions);
    destination.receiver_constants_truncated =
        combined.receiver_constants_truncated;
    return changed;
}

[[nodiscard]] bool add_callback_field_origin(CallbackValue& value,
                                             CallbackFieldOrigin origin) {
    if (value.field_origins_truncated) return false;
    const auto existing = std::find_if(
        value.field_origins.begin(), value.field_origins.end(),
        [&](const auto& candidate) {
            return candidate.displacement == origin.displacement &&
                   candidate.width == origin.width &&
                   candidate.load_instruction_address ==
                       origin.load_instruction_address;
        });
    if (existing != value.field_origins.end())
        return merge_callback_field_origin(*existing, origin);
    if (value.field_origins.size() == maximum_callback_field_origins) {
        value.field_origins.clear();
        value.field_origins_truncated = true;
        return true;
    }
    value.field_origins.push_back(std::move(origin));
    return true;
}

void attach_callback_field_origin(CallbackValue& loaded,
                                  const CallbackValue& receiver,
                                  const std::int32_t displacement,
                                  const std::size_t width,
                                  const std::uint32_t instruction_address) {
    const bool receiver_identity_unknown =
        receiver.memory_derived_receiver &&
        receiver.input_mask == 0u &&
        receiver.receiver_constants.empty() &&
        receiver.receiver_progressions.empty() &&
        !receiver.receiver_constants_truncated;
    if (width != sizeof(std::uint32_t) || receiver.may_be_stack ||
        (receiver.input_mask == 0u &&
         receiver.receiver_constants.empty() &&
         receiver.receiver_progressions.empty() &&
         !receiver.receiver_constants_truncated &&
         !receiver_identity_unknown))
        return;
    CallbackFieldOrigin origin;
    origin.receiver_input_mask = receiver.input_mask;
    origin.receiver_constants = receiver.receiver_constants;
    origin.receiver_progressions = receiver.receiver_progressions;
    origin.receiver_constants_truncated =
        receiver.receiver_constants_truncated || receiver_identity_unknown;
    origin.displacement = displacement;
    origin.width = static_cast<std::uint8_t>(width);
    origin.load_instruction_address = instruction_address;
    static_cast<void>(
        add_callback_field_origin(loaded, std::move(origin)));
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
    value.receiver_constants_truncated =
        before.receiver_constants_truncated;
    std::uint64_t progression_values = 0u;
    for (const auto& progression : before.receiver_progressions) {
        for (std::uint64_t constant = progression.first;
             constant <= progression.last;
             constant += progression.stride) {
            if (progression_values++ >= maximum_receiver_constants) {
                value.receiver_constants_truncated = true;
                break;
            }
            const auto transformed =
                transform(static_cast<std::uint32_t>(constant));
            if (plausible_receiver_constant(image, transformed))
                static_cast<void>(add_bounded_receiver_constant(
                    value, transformed));
        }
    }
    for (const auto constant : before.receiver_constants)
        if (const auto transformed = transform(constant);
            plausible_receiver_constant(image, transformed))
            static_cast<void>(add_bounded_receiver_constant(
                value, transformed));
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
    value.constants_complete = before.constants_complete &&
                               !value.constants_truncated;
}

[[nodiscard]] bool join_value(CallbackValue& destination,
                              const CallbackValue& source,
                              GuardedNativeEntryShapeCache&
                                  native_entry_shapes) {
    bool changed = false;
    const auto input_mask = static_cast<std::uint8_t>(
        destination.input_mask | source.input_mask);
    changed = changed || input_mask != destination.input_mask;
    destination.input_mask = input_mask;
    const bool constants_complete = destination.constants_complete &&
                                    source.constants_complete;
    if (destination.constants_truncated || source.constants_truncated) {
        changed = changed || !destination.constants_truncated ||
                  !destination.constants.empty();
        destination.constants.clear();
        destination.constants_truncated = true;
    } else {
        for (const auto constant : source.constants)
            changed = add_bounded_scalar_constant(destination, constant) ||
                      changed;
    }
    const bool joined_constants_complete =
        constants_complete && !destination.constants_truncated;
    changed = changed || joined_constants_complete !=
                              destination.constants_complete;
    destination.constants_complete = joined_constants_complete;
    if (destination.code_constants_truncated ||
        source.code_constants_truncated) {
        changed = changed || !destination.code_constants_truncated ||
                  !destination.code_constants.empty();
        destination.code_constants.clear();
        destination.code_constants_truncated = true;
    } else {
        for (const auto constant : source.code_constants)
            changed = add_bounded_code_constant(destination, constant,
                                                native_entry_shapes) ||
                      changed;
    }
    const bool joined_code_constants_complete =
        destination.code_constants_complete &&
        source.code_constants_complete &&
        !destination.code_constants_truncated;
    changed = changed || joined_code_constants_complete !=
                              destination.code_constants_complete;
    destination.code_constants_complete =
        joined_code_constants_complete;
    if (destination.receiver_constants_truncated ||
        source.receiver_constants_truncated) {
        changed = changed || !destination.receiver_constants_truncated;
        destination.receiver_constants_truncated = true;
    } else {
        for (const auto& progression : source.receiver_progressions) {
            bool progression_changed = false;
            if (!add_receiver_progression(destination.receiver_progressions,
                                          progression,
                                          &progression_changed)) {
                destination.receiver_constants_truncated = true;
                changed = true;
                break;
            }
            changed = changed || progression_changed;
        }
        if (!destination.receiver_constants_truncated) {
            for (const auto constant : source.receiver_constants)
                changed = add_bounded_receiver_constant(destination,
                                                        constant) ||
                          changed;
        }
    }
    if (source.memory_derived_receiver &&
        !destination.memory_derived_receiver) {
        destination.memory_derived_receiver = true;
        changed = true;
    }
    const auto minimum_alignment =
        std::min(destination.minimum_alignment, source.minimum_alignment);
    changed = changed || minimum_alignment != destination.minimum_alignment;
    destination.minimum_alignment = minimum_alignment;
    if (destination.affine_input != source.affine_input) {
        changed = changed || destination.affine_input.has_value();
        destination.affine_input.reset();
    }
    if (destination.pc_literal_identity != source.pc_literal_identity) {
        changed = changed || destination.pc_literal_identity.has_value();
        destination.pc_literal_identity.reset();
    }
    if (destination.stack_address != source.stack_address) {
        changed = changed || destination.stack_address.has_value();
        destination.stack_address.reset();
    }
    if (source.may_be_stack && !destination.may_be_stack) {
        destination.may_be_stack = true;
        changed = true;
    }
    if (destination.field_origins_truncated ||
        source.field_origins_truncated) {
        changed = changed || !destination.field_origins_truncated ||
                  !destination.field_origins.empty();
        destination.field_origins.clear();
        destination.field_origins_truncated = true;
    } else {
        for (const auto& origin : source.field_origins)
            changed = add_callback_field_origin(destination, origin) ||
                      changed;
    }
    return changed;
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

void scale_affine_input(CallbackValue& value,
                        const std::uint32_t scale) noexcept {
    if (!value.affine_input.has_value()) return;
    if (scale == 0u || value.affine_input->scale >
                           std::numeric_limits<std::uint32_t>::max() / scale) {
        value.affine_input.reset();
        return;
    }
    value.affine_input->scale *= scale;
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
    {
        CallbackValue adjusted_receivers;
        adjusted_receivers.receiver_constants_truncated =
            value.receiver_constants_truncated;
        for (const auto& progression : value.receiver_progressions) {
            const auto first = static_cast<std::int64_t>(progression.first) +
                               immediate;
            const auto last = static_cast<std::int64_t>(progression.last) +
                              immediate;
            if (first < 0 || last < 0 ||
                first > std::numeric_limits<std::uint32_t>::max() ||
                last > std::numeric_limits<std::uint32_t>::max() ||
                !plausible_receiver_constant(
                    image, static_cast<std::uint32_t>(first)) ||
                !plausible_receiver_constant(
                    image, static_cast<std::uint32_t>(last)) ||
                !add_receiver_progression(
                    adjusted_receivers.receiver_progressions,
                    {static_cast<std::uint32_t>(first),
                     static_cast<std::uint32_t>(last),
                     progression.stride})) {
                adjusted_receivers.receiver_constants_truncated = true;
                continue;
            }
        }
        for (const auto constant : value.receiver_constants) {
            const auto candidate =
                constant + static_cast<std::uint32_t>(immediate);
            if (plausible_receiver_constant(image, candidate))
                static_cast<void>(add_bounded_receiver_constant(
                    adjusted_receivers, candidate));
        }
        value.receiver_constants =
            std::move(adjusted_receivers.receiver_constants);
        value.receiver_progressions =
            std::move(adjusted_receivers.receiver_progressions);
        value.receiver_constants_truncated =
            adjusted_receivers.receiver_constants_truncated;
    }
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
    if (immediate != 0) {
        value.affine_input.reset();
        value.pc_literal_identity.reset();
        value.code_constants_complete = false;
    }
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

struct StaticCodePointerVectorInventory final {
    std::vector<StoredCodeAddressCandidate> candidates;
    bool truncated = false;
};

[[nodiscard]] StaticCodePointerVectorInventory
discover_static_code_pointer_vectors(
    const katana::io::ExecutableImage& image,
    const std::span<const std::uint32_t> non_root_function_entry_hints,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    StaticCodePointerVectorInventory inventory;
    // Constructors commonly install a static callback descriptor into a
    // mutable object before any consumer can expose the descriptor base to
    // value analysis. That creates a real dependency cycle: the constructor
    // is reachable only through the very vector whose address it publishes.
    //
    // Four contiguous, aligned and independently decodable function entries
    // in an identity-bound image are a sufficiently strong positive shape to
    // break that cycle. This remains guarded inventory only. It neither
    // resolves an indirect transfer nor claims the vector is complete at
    // runtime. Shorter runs stay excluded because ordinary structures often
    // contain one or two incidental code pointers.
    for (const auto& segment : image.segments()) {
        // Runtime modules and overlays own independent address identities and
        // retirement epochs. Their vectors must be discovered by the bound
        // latent/module analysis, never imported into the initial-image
        // closure merely because the current snapshot also contains bytes at
        // their source address.
        if (segment.load_phase != katana::io::ImageLoadPhase::Initial ||
            !segment.permissions.readable || segment.bytes.size() < 16u)
            continue;
        const auto first_offset = static_cast<std::size_t>(
            (4u - (segment.virtual_address & 3u)) & 3u);
        auto offset = first_offset;
        while (offset <= segment.bytes.size() - 4u) {
            struct Entry final {
                std::uint32_t target = 0u;
                std::uint32_t slot = 0u;
            };
            std::vector<Entry> entries;
            auto cursor = offset;
            for (; cursor <= segment.bytes.size() - 4u; cursor += 4u) {
                const auto slot64 =
                    static_cast<std::uint64_t>(segment.virtual_address) +
                    cursor;
                if (slot64 > std::numeric_limits<std::uint32_t>::max())
                    break;
                const auto slot = static_cast<std::uint32_t>(slot64);
                const auto raw = read_image_u32(image, slot);
                if (!raw.has_value()) break;
                const auto target = executable_constant(image, *raw);
                if (!target.has_value()) break;
                const auto* target_segment =
                    image.find_segment(*target, sizeof(std::uint16_t));
                if (target_segment == nullptr ||
                    target_segment->load_phase != segment.load_phase)
                    break;
                if (!std::binary_search(
                        non_root_function_entry_hints.begin(),
                        non_root_function_entry_hints.end(), *target))
                    break;

                const auto status = native_entry_shapes.classify(*target);
                if (status ==
                    GuardedNativeEntryShapeStatus::ShapeBudgetExceeded) {
                    inventory.truncated = true;
                    return inventory;
                }
                if (status != GuardedNativeEntryShapeStatus::Valid) break;
                entries.push_back({*target, slot});
                if (entries.size() >
                    maximum_static_code_pointer_table_entries) {
                    inventory.truncated = true;
                    return inventory;
                }
            }

            if (entries.size() >=
                minimum_static_code_pointer_vector_entries) {
                for (const auto& entry : entries)
                    add_candidate(inventory.candidates, image, entry.target,
                                  entry.slot);
            }
            // cursor names the first rejected word. It cannot begin a valid
            // vector; resume at the following aligned slot. At end-of-segment
            // the overflow-safe assignment simply terminates the outer loop.
            if (cursor > segment.bytes.size() - 4u) break;
            offset = cursor + 4u;
        }
    }
    return inventory;
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
                              const std::size_t width,
                              const bool receiver_identity_proven) {
    if (width != 4u) return;
    // A raw field displacement plus an unproven receiver is not an ABI type.
    // Keep concrete executable literals as guarded positive inventory, but
    // only advertise an incoming argument as a callback when the store and
    // the indirect-load sink share an exact or same-local receiver. Otherwise
    // an ordinary data pointer stored at (for example) record +4 poisons every
    // caller of the constructor as a higher-order callback API.
    if (receiver_identity_proven) {
        model.local_sink_mask = static_cast<std::uint8_t>(
            model.local_sink_mask | source.input_mask);
    }
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

void observe_potential_persistent_store(
    CallbackFunctionModel& model,
    const CallbackValue& source,
    const CallbackValue& receiver,
    const std::int32_t displacement,
    const std::uint32_t instruction_address,
    const std::size_t width,
    const bool indexed_addressing,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    if (width != sizeof(std::uint32_t) || receiver.may_be_stack ||
        (source.input_mask == 0u && source.code_constants.empty() &&
         !source.code_constants_truncated))
        return;
    const auto existing = std::find_if(
        model.persistent_stores.begin(), model.persistent_stores.end(),
        [&](const auto& candidate) {
            return candidate.displacement == displacement &&
                   candidate.instruction_address == instruction_address &&
                   candidate.width == width;
        });
    if (existing != model.persistent_stores.end()) {
        static_cast<void>(join_value(existing->source, source,
                                     native_entry_shapes));
        static_cast<void>(join_value(existing->receiver, receiver,
                                     native_entry_shapes));
        existing->indexed_addressing =
            existing->indexed_addressing || indexed_addressing;
        return;
    }
    if (model.persistent_stores.size() ==
        maximum_persistent_store_observations) {
        model.persistent_stores_truncated = true;
        return;
    }
    model.persistent_stores.push_back(
        {source, receiver, displacement, instruction_address,
         static_cast<std::uint8_t>(width), indexed_addressing});
}

void observe_indexed_persistent_store(
    CallbackFunctionModel& model,
    const CallbackValue& source,
    const CallbackValue& base,
    const CallbackValue& byte_offset,
    const std::uint32_t instruction_address,
    const std::size_t width,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    const bool exact_literal_base =
        base.constants_complete && !base.constants_truncated &&
        base.constants.size() == 1u && base.pc_literal_identity.has_value() &&
        *base.constants.begin() == base.pc_literal_identity->value;
    const bool identity_complete =
        width == sizeof(std::uint32_t) && !byte_offset.may_be_stack &&
        exact_literal_base && byte_offset.affine_input.has_value() &&
        byte_offset.input_mask == static_cast<std::uint8_t>(
                                      1u <<
                                      byte_offset.affine_input->argument);

    const auto existing =
        model.indexed_persistent_stores.find(instruction_address);
    if (existing != model.indexed_persistent_stores.end()) {
        static_cast<void>(join_value(existing->second.source, source,
                                     native_entry_shapes));
        const bool same_identity =
            identity_complete &&
            existing->second.destination_identity_complete &&
            existing->second.base == base.pc_literal_identity &&
            existing->second.byte_offset == byte_offset.affine_input &&
            existing->second.width == width;
        existing->second.destination_identity_complete = same_identity;
        if (!same_identity) {
            existing->second.base.reset();
            existing->second.byte_offset.reset();
        }
        return;
    }
    if (model.indexed_persistent_stores.size() ==
        maximum_persistent_store_observations) {
        model.persistent_stores_truncated = true;
        return;
    }
    model.indexed_persistent_stores.emplace(
        instruction_address,
        CallbackIndexedPersistentStore{
            source,
            exact_literal_base
                ? base.pc_literal_identity
                : std::optional<CallbackPcLiteralIdentity>{},
            byte_offset.affine_input,
            instruction_address,
            static_cast<std::uint8_t>(width),
            identity_complete});
}

void store_value(CallbackFunctionModel& model,
                 CallbackState& state,
                 const CallbackValue& source,
                 const CallbackValue& destination,
                 const std::int32_t displacement,
                 const std::uint32_t instruction_address,
                 const std::size_t width,
                 GuardedNativeEntryShapeCache& native_entry_shapes,
                 const bool indexed_addressing = false) {
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
    observe_potential_persistent_store(
        model, source, destination, displacement,
        instruction_address, width, indexed_addressing,
        native_entry_shapes);
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
    CallbackValue result;
    const bool source_has_receiver_provenance =
        source.input_mask != 0u || !source.constants.empty() ||
        !source.receiver_constants.empty() ||
        !source.receiver_progressions.empty() ||
        source.memory_derived_receiver;
    result.memory_derived_receiver =
        width == sizeof(std::uint32_t) && !source.may_be_stack &&
        source_has_receiver_provenance;
    return result;
}

void apply_instruction(CallbackFunctionModel& model,
                       CallbackState& state,
                       const katana::io::ExecutableImage& image,
                       const katana::sh4::DisassemblyLine& line,
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
        state.registers[destination].constants_complete =
            !state.registers[destination].constants_truncated;
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
    case K::ShiftArithmeticLeftOne: {
        const auto affine = state.registers[destination].affine_input;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 1u; });
        state.registers[destination].affine_input = affine;
        scale_affine_input(state.registers[destination], 2u);
        scale_minimum_alignment(state.registers[destination], 2u);
        return;
    }
    case K::ShiftLogicalLeftTwo: {
        const auto affine = state.registers[destination].affine_input;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 2u; });
        state.registers[destination].affine_input = affine;
        scale_affine_input(state.registers[destination], 4u);
        scale_minimum_alignment(state.registers[destination], 4u);
        return;
    }
    case K::ShiftLogicalLeftEight: {
        const auto affine = state.registers[destination].affine_input;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 8u; });
        state.registers[destination].affine_input = affine;
        scale_affine_input(state.registers[destination], 256u);
        scale_minimum_alignment(state.registers[destination], 256u);
        return;
    }
    case K::ShiftLogicalLeftSixteen: {
        const auto affine = state.registers[destination].affine_input;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 16u; });
        state.registers[destination].affine_input = affine;
        scale_affine_input(state.registers[destination], 65'536u);
        scale_minimum_alignment(state.registers[destination], 65'536u);
        return;
    }
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
        state.registers[0u].constants_complete =
            !state.registers[0u].constants_truncated;
        return;
    }
    case K::MovLongLoadPcRelative: {
        state.registers[destination] = {};
        const auto literal_address =
            ((line.address + 4u) & ~3u) +
            static_cast<std::uint32_t>(instruction.displacement);
        if (const auto value = read_image_u32(image, literal_address);
            value.has_value()) {
            add_constant(state.registers[destination], image, *value,
                         native_entry_shapes);
            state.registers[destination].constants_complete =
                !state.registers[destination].constants_truncated;
            state.registers[destination].pc_literal_identity =
                CallbackPcLiteralIdentity{literal_address, *value};
            state.registers[destination].code_constants_complete =
                !state.registers[destination].code_constants_truncated &&
                !state.registers[destination].code_constants.empty();
        }
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
        store_value(model, state, state.registers[source],
                    state.registers[destination], 0,
                    line.address, width, native_entry_shapes);
        return;
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        store_value(model, state, state.registers[source],
                    state.registers[destination], instruction.displacement,
                    line.address, width, native_entry_shapes);
        return;
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed: {
        observe_indexed_persistent_store(
            model, state.registers[source], state.registers[0u],
            state.registers[destination], line.address, width,
            native_entry_shapes);
        if (state.registers[0u].constants.size() != 1u ||
            state.registers[0u].constants_truncated)
            return;
        store_value(model, state, state.registers[source],
                    state.registers[destination],
                    static_cast<std::int32_t>(
                        *state.registers[0u].constants.begin()),
                    line.address, width, native_entry_shapes, true);
        return;
    }
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement: {
        auto& base = state.registers[destination];
        transform_add_immediate(base, image, native_entry_shapes,
                                -static_cast<std::int32_t>(width),
                                destination == 15u);
        store_value(model, state, state.registers[source], base, 0,
                    line.address, width, native_entry_shapes);
        return;
    }
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad: {
        // Snapshot every address operand before publishing the loaded value.
        // Rm == Rn is legal and must not make the positive static-image lane
        // dereference the value that was just loaded instead of the original
        // base address.
        const auto base = state.registers[source];
        auto loaded_value = load_value(state, base, 0, width);
        if (width == 4u) {
            const auto loaded = load_static_image_values(
                image, base, 0, native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, loaded, line.address, native_entry_shapes);
            static_cast<void>(join_value(loaded_value, loaded,
                                         native_entry_shapes));
            attach_callback_field_origin(loaded_value, base, 0u, width,
                                         line.address);
        }
        state.registers[destination] = std::move(loaded_value);
        return;
    }
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement: {
        const auto base = state.registers[source];
        auto loaded_value =
            load_value(state, base, instruction.displacement, width);
        if (width == 4u) {
            const auto loaded = load_static_image_values(
                image, base, instruction.displacement,
                native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, loaded, line.address, native_entry_shapes);
            static_cast<void>(join_value(loaded_value, loaded,
                                         native_entry_shapes));
            attach_callback_field_origin(loaded_value, base,
                                         instruction.displacement, width,
                                         line.address);
        }
        state.registers[destination] = std::move(loaded_value);
        return;
    }
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed: {
        // R0, Rm and Rn may all alias. Preserve both address components until
        // the dynamic and static load domains have consumed them.
        const auto index = state.registers[0u];
        const auto base = state.registers[source];
        CallbackValue loaded_value;
        if (index.constants.size() == 1u &&
            !index.constants_truncated)
            loaded_value = load_value(
                state, base,
                static_cast<std::int32_t>(
                    *index.constants.begin()),
                width);
        else
            set_unknown(loaded_value);
        if (width == 4u) {
            const auto loaded = load_static_indexed_value(
                image, index, base, native_entry_shapes);
            static_cast<void>(join_value(loaded_value, loaded,
                                         native_entry_shapes));
            if (index.constants.size() == 1u &&
                !index.constants_truncated)
                attach_callback_field_origin(
                    loaded_value, base,
                    static_cast<std::int32_t>(*index.constants.begin()),
                    width, line.address);
        }
        state.registers[destination] = std::move(loaded_value);
        return;
    }
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement: {
        const auto base = state.registers[source];
        auto loaded = load_value(state, base, 0, width);
        if (width == 4u) {
            const auto static_loaded = load_static_image_values(
                image, base, 0, native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, static_loaded, line.address,
                native_entry_shapes);
            static_cast<void>(join_value(loaded, static_loaded,
                                         native_entry_shapes));
            attach_callback_field_origin(loaded, base, 0u, width,
                                         line.address);
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
    const CallbackValue& branch_before_delay,
    const std::set<std::uint32_t>& function_entries) {
    std::set<std::uint32_t> targets;
    if (control.target_address.has_value())
        targets.insert(*control.target_address);
    const auto kind = control.instruction.kind;
    if (kind == katana::sh4::InstructionKind::Jsr ||
        kind == katana::sh4::InstructionKind::Jmp) {
        if (!branch_before_delay.code_constants_truncated)
            targets.insert(branch_before_delay.code_constants.begin(),
                           branch_before_delay.code_constants.end());
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
    GuardedNativeEntryShapeCache& native_entry_shapes,
    std::size_t* const limited_evaluations) {
    CallbackFunctionModel model;
    model.entry = function.entry_address;
    const auto entry = blocks.find(function.entry_address);
    if (entry == blocks.end()) return model;

    CallbackState initial;
    for (std::uint8_t index = 0u; index < 4u; ++index) {
        initial.registers[4u + index].input_mask =
            static_cast<std::uint8_t>(1u << index);
        initial.registers[4u + index].affine_input =
            CallbackAffineInput{index, 1u};
    }
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
    // Work is monotone: an ingress block is queued only after at least one
    // bounded scalar/code/receiver/provenance fact changes.  The receiver and
    // typed-table lanes each admit 256 exact values, so the old 128 visits per
    // block was below the lattice height and incorrectly reported loss for
    // finite dispatcher functions.  This checked bound stays local to the
    // function and does not turn timeout or memory limits into semantics.
    constexpr std::size_t evaluations_per_block = 4'096u;
    if (function.block_addresses.size() >
        std::numeric_limits<std::size_t>::max() /
            evaluations_per_block)
        throw std::overflow_error(
            "Static-Callback-Fixpunktbudget laeuft ueber.");
    const auto evaluation_budget = std::max<std::size_t>(
        evaluations_per_block,
        function.block_addresses.size() * evaluations_per_block);
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
        CallbackValue branch_before_delay;
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
                branch_before_delay = {};
                const auto kind = line.instruction.kind;
                if (kind == katana::sh4::InstructionKind::Jsr ||
                    kind == katana::sh4::InstructionKind::Jmp) {
                    const auto branch_register =
                        line.instruction.branch_register;
                    if (branch_register < state.registers.size())
                        branch_before_delay =
                            state.registers[branch_register];
                }
                targets = call_targets(image, line, branch_before_delay,
                                       function_entries);
                continue;
            }
            apply_instruction(model, state, image, line,
                              native_entry_shapes);
        }

        // A higher-order API can invoke a callback directly without first
        // persisting it in a task record.  The branch register is sampled
        // before the SH-4 delay slot, so retain the ABI taint from that exact
        // state.  This is only a positive sink summary: it does not claim an
        // indirect target set is complete and therefore cannot turn an
        // unknown callback into an authoritative CFG edge by itself.
        if (control != nullptr &&
            (control->instruction.control_flow ==
                 katana::sh4::ControlFlowKind::IndirectCall ||
             control->instruction.control_flow ==
                 katana::sh4::ControlFlowKind::IndirectBranch)) {
            const auto branch_register =
                control->instruction.branch_register;
            if (branch_register < state.registers.size()) {
                const auto& branch = branch_before_delay;
                model.local_sink_mask = static_cast<std::uint8_t>(
                    model.local_sink_mask |
                    branch.input_mask);
                if (branch.field_origins_truncated)
                    model.field_sinks_truncated = true;
                for (const auto& field : branch.field_origins) {
                    CallbackFieldSink sink{
                        model.entry,
                        control->address,
                        field,
                        control->instruction.control_flow ==
                            katana::sh4::ControlFlowKind::IndirectCall};
                    const auto existing = std::find_if(
                        model.field_sinks.begin(),
                        model.field_sinks.end(),
                        [&](const auto& candidate) {
                            return candidate.function_address ==
                                       sink.function_address &&
                                   candidate.call_instruction_address ==
                                       sink.call_instruction_address &&
                                   candidate.field.load_instruction_address ==
                                       sink.field.load_instruction_address &&
                                   candidate.field.displacement ==
                                       sink.field.displacement &&
                                   candidate.field.width == sink.field.width &&
                                   candidate.call == sink.call;
                        });
                    if (existing != model.field_sinks.end()) {
                        static_cast<void>(merge_callback_field_origin(
                            existing->field, sink.field));
                    } else {
                        model.field_sinks.push_back(std::move(sink));
                    }
                }
            }
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

[[nodiscard]] bool persistent_main_ram_span(const std::uint32_t address,
                                             const std::size_t width) {
    if (width == 0u) return false;
    const auto end = static_cast<std::uint64_t>(address) + width - 1u;
    if (end > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto first_physical = address & 0x1FFFFFFFu;
    const auto last_physical =
        static_cast<std::uint32_t>(end) & 0x1FFFFFFFu;
    return first_physical >= 0x0C000000u &&
           last_physical >= first_physical &&
           last_physical < 0x10000000u;
}

} // namespace

std::vector<std::int32_t> discover_static_callback_field_offsets(
    const std::span<const katana::sh4::DisassemblyLine> lines) {
    const auto offsets = discover_callback_field_offsets(lines);
    return {offsets.begin(), offsets.end()};
}

GuardedCodeInventory analyze_static_callback_inventory(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionCandidate> function_candidates,
    const std::span<const std::uint32_t> external_block_entries,
    const std::span<const std::uint32_t> non_root_function_entry_hints,
    GuardedNativeEntryShapeCache& native_entry_shapes,
    std::vector<StaticCallbackSinkContract>* const
        callback_sink_contracts,
    std::vector<StaticPersistentPointerSinkContract>* const
        persistent_pointer_sink_contracts,
    std::vector<StaticCallbackFieldSinkContract>* const
        callback_field_sink_contracts) {
    if (callback_sink_contracts != nullptr)
        callback_sink_contracts->clear();
    if (persistent_pointer_sink_contracts != nullptr)
        persistent_pointer_sink_contracts->clear();
    if (callback_field_sink_contracts != nullptr)
        callback_field_sink_contracts->clear();
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
    const auto blocks = build_basic_blocks(
        lines, {}, leaders, non_root_function_entry_hints);
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
        if (native_entry_shapes.is_physical_delay_slot(entry) &&
            !std::binary_search(non_root_function_entry_hints.begin(),
                                non_root_function_entry_hints.end(),
                                entry))
            continue;
        const auto containing_block = std::find_if(
            blocks.begin(), blocks.end(), [&](const auto& candidate) {
                return candidate.start_address < entry &&
                       entry <= candidate.end_address;
            });
        if (containing_block != blocks.end() &&
            owned_blocks.contains(containing_block->start_address))
            continue;
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
    std::size_t receiver_may_alias_stores = 0u;
    std::map<std::uint32_t, CallbackFunctionModel> models;
    for (const auto& function : functions) {
        auto model = build_function_model(image, function, block_index,
                                          function_entries,
                                          native_entry_shapes,
                                          &limited_evaluations);
        models.insert_or_assign(function.entry_address, std::move(model));
    }

    // A receiver lane is allowed to become wide, but a lost receiver relation
    // makes the positive callback inventory incomplete.  Emit only the first
    // few exact loss sites so a contract failure identifies the responsible
    // owner/field instead of inviting a blind global budget increase.
    std::size_t receiver_loss_diagnostics = 0u;
    constexpr std::size_t maximum_receiver_loss_diagnostics = 12u;
    for (const auto& [entry, model] : models) {
        for (const auto& sink : model.field_sinks) {
            if (!sink.field.receiver_constants_truncated ||
                receiver_loss_diagnostics >=
                    maximum_receiver_loss_diagnostics)
                continue;
            std::cerr << "KATANA_STATIC_CALLBACK_RECEIVER_LOSS kind=sink"
                      << " owner=0x" << std::hex << entry
                      << " instruction=0x" << sink.call_instruction_address
                      << " load=0x" << sink.field.load_instruction_address
                      << std::dec
                      << " displacement=" << sink.field.displacement
                      << " width=" << static_cast<unsigned>(sink.field.width)
                      << '\n';
            ++receiver_loss_diagnostics;
        }
        for (const auto& store : model.persistent_stores) {
            if (!store.receiver.receiver_constants_truncated ||
                receiver_loss_diagnostics >=
                    maximum_receiver_loss_diagnostics)
                continue;
            std::cerr << "KATANA_STATIC_CALLBACK_RECEIVER_LOSS kind=store"
                      << " owner=0x" << std::hex << entry
                      << " instruction=0x" << store.instruction_address
                      << std::dec
                      << " displacement=" << store.displacement
                      << " width=" << static_cast<unsigned>(store.width)
                      << '\n';
            ++receiver_loss_diagnostics;
        }
    }

    // A displacement alone is not a callback contract.  Relate a persistent
    // store to a later indirect call/jump only when both operations address
    // the same proven receiver: either the same exact record address across
    // owners, or the same incoming ABI receiver within one owner.  This keeps
    // ordinary fields which happen to share +0/+4/+16 out of the executable
    // inventory while retaining task records, vtables, and callback lists.
    using CallbackFieldShape = std::pair<std::int32_t, std::uint8_t>;
    std::map<CallbackFieldShape, std::vector<const CallbackFieldSink*>>
        field_sinks_by_shape;
    for (const auto& [entry, model] : models) {
        static_cast<void>(entry);
        candidate_values_truncated =
            candidate_values_truncated || model.field_sinks_truncated ||
            model.persistent_stores_truncated;
        for (const auto& sink : model.field_sinks)
            field_sinks_by_shape[{sink.field.displacement,
                                  sink.field.width}]
                .push_back(&sink);
    }
    for (auto& [entry, model] : models) {
        for (const auto& store : model.persistent_stores) {
            model.local_persistent_pointer_mask =
                static_cast<std::uint8_t>(
                    model.local_persistent_pointer_mask |
                    store.source.input_mask);
            // R0-indexed stores use a separate caller-domain/address proof.
            // Treating their dynamic index as a record receiver and their
            // table base as a signed field displacement would allow the older
            // field-sink path to bypass that proof.
            if (store.indexed_addressing) continue;

            // Constructors commonly install either a caller-supplied handler
            // or an exact built-in default into the same freshly allocated
            // record field.  The matching local receiver origin and exact
            // executable alternative prove the incoming lane is callback-
            // typed even when a mutable free-list/list-head transition keeps
            // the later scheduler receiver identity intentionally abstract.
            const bool locally_typed_callback =
                store.source.input_mask != 0u &&
                std::any_of(
                    model.persistent_stores.begin(),
                    model.persistent_stores.end(),
                    [&](const auto& witness) {
                        return witness.displacement == store.displacement &&
                               witness.width == store.width &&
                               exact_executable_default_store(witness) &&
                               same_local_record_receiver(store.receiver,
                                                          witness.receiver);
                    });
            if (locally_typed_callback) {
                observe_persistent_store(model, image, store.source,
                                         store.instruction_address,
                                         store.width, true);
                continue;
            }
            const auto sinks = field_sinks_by_shape.find(
                {store.displacement, store.width});
            if (sinks == field_sinks_by_shape.end()) continue;
            bool matched = false;
            bool receiver_may_alias = false;
            for (const auto* const sink : sinks->second) {
                const bool same_local_receiver =
                    sink->function_address == entry &&
                    (sink->field.receiver_input_mask &
                     store.receiver.input_mask) != 0u;
                const auto intersection = receiver_evidence_intersects(
                    sink->field, store.receiver);
                const bool same_exact_receiver =
                    intersection == ReceiverIntersection::Exact;
                const bool possible_receiver =
                    intersection == ReceiverIntersection::Unproven;
                receiver_may_alias = receiver_may_alias ||
                                     possible_receiver;
                if (!same_local_receiver && !same_exact_receiver) continue;
                matched = true;
                observe_persistent_store(model, image, store.source,
                                         store.instruction_address,
                                         store.width, true);
                break;
            }
            if (!matched && receiver_may_alias) {
                // Receiver Top is a may-alias state, not lost executable
                // evidence. Conservatively relate a callback-valued store to
                // a same-shaped indirect-load sink. This can retain extra
                // identity-checked guarded roots, but cannot omit a callback
                // merely because a record loop exceeded the exact receiver
                // inventory.
                observe_persistent_store(model, image, store.source,
                                         store.instruction_address,
                                         store.width, false);
                ++receiver_may_alias_stores;
            }
        }
    }

    std::map<std::uint32_t, std::uint8_t> sink_masks;
    std::map<std::uint32_t, std::uint8_t> persistent_pointer_masks;
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::size_t>>>
        callers_by_callee;
    std::deque<std::uint32_t> pending;
    std::set<std::uint32_t> queued;
    for (const auto& [entry, model] : models) {
        sink_masks[entry] = model.local_sink_mask;
        persistent_pointer_masks[entry] =
            model.local_persistent_pointer_mask;
        if (model.local_sink_mask != 0u && queued.insert(entry).second)
            pending.push_back(entry);
        for (std::size_t index = 0u; index < model.calls.size(); ++index)
            callers_by_callee[model.calls[index].callee].push_back(
                {entry, index});
    }

    const auto propagate_masks =
        [&](std::map<std::uint32_t, std::uint8_t>& masks) {
            std::deque<std::uint32_t> work;
            std::set<std::uint32_t> scheduled;
            for (const auto [entry, mask] : masks) {
                if (mask != 0u && scheduled.insert(entry).second)
                    work.push_back(entry);
            }
            std::size_t steps = 0u;
            const auto budget =
                std::max<std::size_t>(64u, models.size() * 8u);
            while (!work.empty()) {
                const auto callee = work.front();
                work.pop_front();
                scheduled.erase(callee);
                if (++steps > budget) {
                    forwarding_truncated = true;
                    break;
                }
                const auto mask = masks[callee];
                const auto callers = callers_by_callee.find(callee);
                if (callers == callers_by_callee.end()) continue;
                for (const auto& [caller, call_index] : callers->second) {
                    const auto model = models.find(caller);
                    if (model == models.end() ||
                        call_index >= model->second.calls.size())
                        continue;
                    std::uint8_t propagated = 0u;
                    const auto& call = model->second.calls[call_index];
                    for (std::size_t argument = 0u; argument < 4u;
                         ++argument) {
                        if ((mask & static_cast<std::uint8_t>(
                                        1u << argument)) == 0u)
                            continue;
                        propagated = static_cast<std::uint8_t>(
                            propagated |
                            call.arguments[argument].input_mask);
                    }
                    auto& caller_mask = masks[caller];
                    const auto next = static_cast<std::uint8_t>(
                        caller_mask | propagated);
                    if (next == caller_mask) continue;
                    caller_mask = next;
                    if (scheduled.insert(caller).second)
                        work.push_back(caller);
                }
            }
        };

    propagate_masks(persistent_pointer_masks);
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

    if (callback_sink_contracts != nullptr) {
        callback_sink_contracts->reserve(sink_masks.size());
        for (const auto [function_address, argument_mask] : sink_masks) {
            if (argument_mask == 0u) continue;
            callback_sink_contracts->push_back(
                {function_address, argument_mask});
        }
    }


    if (persistent_pointer_sink_contracts != nullptr) {
        persistent_pointer_sink_contracts->reserve(
            persistent_pointer_masks.size());
        for (const auto [function_address, argument_mask] :
             persistent_pointer_masks) {
            if (argument_mask == 0u) continue;
            persistent_pointer_sink_contracts->push_back(
                {function_address, argument_mask});
        }
    }

    if (callback_field_sink_contracts != nullptr) {
        for (const auto& function : functions) {
            const auto structural =
                discover_structural_callback_field_sinks(
                    function, block_index);
            callback_field_sink_contracts->insert(
                callback_field_sink_contracts->end(),
                structural.begin(), structural.end());
        }
        for (const auto& [entry, model] : models) {
            static_cast<void>(entry);
            for (const auto& sink : model.field_sinks) {
                callback_field_sink_contracts->push_back(
                    {sink.function_address,
                     sink.call_instruction_address,
                     sink.field.load_instruction_address,
                     sink.field.displacement,
                     sink.field.width,
                     sink.call});
            }
        }
        std::sort(
            callback_field_sink_contracts->begin(),
            callback_field_sink_contracts->end(),
            [](const auto& left, const auto& right) {
                return std::tie(left.function_address,
                                left.call_instruction_address,
                                left.load_instruction_address,
                                left.displacement,
                                left.width,
                                left.call) <
                       std::tie(right.function_address,
                                right.call_instruction_address,
                                right.load_instruction_address,
                                right.displacement,
                                right.width,
                                right.call);
            });
        callback_field_sink_contracts->erase(
            std::unique(callback_field_sink_contracts->begin(),
                        callback_field_sink_contracts->end()),
            callback_field_sink_contracts->end());
    }

    if (!std::is_sorted(non_root_function_entry_hints.begin(),
                        non_root_function_entry_hints.end()) ||
        std::adjacent_find(non_root_function_entry_hints.begin(),
                           non_root_function_entry_hints.end()) !=
            non_root_function_entry_hints.end())
        throw std::invalid_argument(
            "Nicht-rootende Funktionseinstiegshinweise sind nicht "
            "kanonisch.");

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
    const auto static_vectors = discover_static_code_pointer_vectors(
        image, non_root_function_entry_hints, native_entry_shapes);
    for (const auto& candidate : static_vectors.candidates)
        merge_candidate(candidate);
    candidate_values_truncated =
        candidate_values_truncated || static_vectors.truncated;
    for (const auto& [entry, model] : models) {
        for (const auto& candidate : model.local_candidates)
            merge_candidate(candidate);
        // Some bootstrap registrars store code literals into a global vector
        // through an incoming selector. Admit those literals only when the
        // callee preserves an affine ABI-argument offset, the base retains its
        // exact PC-literal identity, and every statically known caller supplies
        // a complete finite argument domain. A plausible RAM address or an
        // alignment-only index is deliberately insufficient.
        for (const auto& [store_site, store] :
             model.indexed_persistent_stores) {
            static_cast<void>(store_site);
            if (!store.destination_identity_complete ||
                !store.base.has_value() ||
                !store.byte_offset.has_value() ||
                store.width != sizeof(std::uint32_t))
                continue;
            const auto callers = callers_by_callee.find(entry);
            if (callers == callers_by_callee.end() ||
                callers->second.empty())
                continue;

            bool caller_domain_complete = true;
            std::vector<std::uint32_t> evidence_call_sites;
            std::set<std::uint32_t> effective_slots;
            for (const auto& [caller, call_index] : callers->second) {
                const auto owner = models.find(caller);
                if (owner == models.end() ||
                    call_index >= owner->second.calls.size() ||
                    store.byte_offset->argument >= 4u) {
                    caller_domain_complete = false;
                    break;
                }
                const auto& call = owner->second.calls[call_index];
                const auto& argument =
                    call.arguments[store.byte_offset->argument];
                if (!argument.constants_complete ||
                    argument.constants_truncated ||
                    argument.constants.empty() ||
                    argument.input_mask != 0u) {
                    caller_domain_complete = false;
                    break;
                }
                for (const auto constant : argument.constants) {
                    const auto byte_offset =
                        static_cast<std::uint64_t>(constant) *
                        store.byte_offset->scale;
                    const auto effective =
                        static_cast<std::uint64_t>(store.base->value) +
                        byte_offset;
                    if (byte_offset >
                            std::numeric_limits<std::uint32_t>::max() ||
                        effective >
                            std::numeric_limits<std::uint32_t>::max() ||
                        (effective & (sizeof(std::uint32_t) - 1u)) != 0u ||
                        !persistent_main_ram_span(
                            static_cast<std::uint32_t>(effective),
                            sizeof(std::uint32_t))) {
                        caller_domain_complete = false;
                        break;
                    }
                    effective_slots.insert(
                        static_cast<std::uint32_t>(effective));
                }
                if (!caller_domain_complete) break;
                evidence_call_sites.push_back(call.instruction_address);
            }
            // Two or more independently addressed 32-bit slots are the
            // minimum structural evidence that the literal base owns a table
            // rather than an arbitrary scalar/heap cell.
            if (!caller_domain_complete || effective_slots.size() < 2u)
                continue;
            if (store.source.code_constants_truncated) {
                candidate_values_truncated = true;
                continue;
            }
            if (!store.source.code_constants_complete ||
                store.source.code_constants.empty())
                continue;
            for (const auto target : store.source.code_constants) {
                StoredCodeAddressCandidate candidate;
                candidate.target_address = target;
                candidate.guarded = true;
                candidate.store_instruction_addresses = {
                    store.instruction_address};
                candidate.evidence_call_sites = evidence_call_sites;
                candidate.evidence_callees = {entry};
                merge_candidate(candidate);
            }
        }
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
    if (candidate_values_truncated) {
        const auto count_models = [&](const auto selector) {
            return std::count_if(
                models.begin(), models.end(),
                [&](const auto& item) { return selector(item.second); });
        };
        std::size_t truncated_forwarded_arguments = 0u;
        for (const auto& [entry, model] : models) {
            static_cast<void>(entry);
            for (const auto& call : model.calls)
                truncated_forwarded_arguments += std::count_if(
                    call.arguments.begin(), call.arguments.end(),
                    [](const auto& argument) {
                        return argument.code_constants_truncated;
                    });
        }
        std::cerr
            << "KATANA_STATIC_CALLBACK_INVENTORY_LOSS "
            << "field_sink_functions="
            << count_models([](const auto& model) {
                   return model.field_sinks_truncated;
               })
            << " persistent_store_functions="
            << count_models([](const auto& model) {
                   return model.persistent_stores_truncated;
               })
            << " local_candidate_functions="
            << count_models([](const auto& model) {
                   return model.local_candidates_truncated;
               })
            << " forwarded_arguments="
            << truncated_forwarded_arguments
            << " receiver_may_alias_stores="
            << receiver_may_alias_stores
            << " static_vector=" << static_vectors.truncated << '\n';
    }
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
