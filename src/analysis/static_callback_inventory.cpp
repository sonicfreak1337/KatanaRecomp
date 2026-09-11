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
#include <memory>
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
constexpr std::size_t minimum_repeated_short_code_pointer_vector_entries = 2u;
constexpr std::size_t maximum_repeated_short_code_pointer_vector_keys =
    maximum_inventory_candidates * 4u;
constexpr std::size_t maximum_static_code_pointer_table_entries = 256u;
constexpr std::uint32_t maximum_static_code_pointer_table_stride = 256u;

struct ReceiverProgression final {
    std::uint32_t first = 0u;
    std::uint32_t last = 0u;
    std::uint32_t stride = 1u;

    bool operator==(const ReceiverProgression&) const = default;
};

struct CallbackRecordTableOrigin final {
    // Exact authenticated address of a mutable record-head table plus its
    // selector-derived byte stride. Different PC literal cells may name the
    // same table, so the pointee address (not the literal slot) is the family
    // identity within one bound image generation. A selector may normalize a
    // reserved value to one exact constant before it is scaled; that remains
    // a record-table family proof, but not an exhaustive selector-domain
    // proof.
    std::uint32_t table_address = 0u;
    std::uint32_t stride = 0u;

    bool operator==(const CallbackRecordTableOrigin&) const = default;
};

struct CallbackFieldOrigin final {
    // Receiver provenance is relative to the owning function. Exact receiver
    // constants remain comparable across functions; incoming ABI argument
    // bits are comparable only inside the same owner.
    std::uint8_t receiver_input_mask = 0u;
    // Must-alias proof; broad receiver_input_mask is only may-influence.
    std::uint8_t receiver_base_argument_mask = 0u;
    // Record-family lineage is narrower than ordinary data influence.  It is
    // preserved only by exact register/stack copies and pointer-field loads,
    // allowing a linked record reached from an incoming record to retain the
    // caller ABI without turning arbitrary loaded data into a callback.
    std::uint8_t receiver_record_family_input_mask = 0u;
    std::optional<CallbackRecordTableOrigin> receiver_record_table_origin;
    std::set<std::uint32_t> receiver_constants;
    std::vector<ReceiverProgression> receiver_progressions;
    bool receiver_constants_truncated = false;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    std::uint32_t load_instruction_address = 0u;

    bool operator==(const CallbackFieldOrigin&) const = default;
};

struct CopiedFieldIdentity final {
    std::uint32_t load_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t argument = 0u;

    bool operator==(const CopiedFieldIdentity&) const = default;
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

struct CallbackScaledIndexOrigin final {
    std::uint32_t load_instruction_address = 0u;
    std::uint32_t scale = 1u;
    std::uint32_t selector_expression_address = 0u;

    bool operator==(const CallbackScaledIndexOrigin&) const = default;
};

struct CallbackTablePointerOrigin final {
    std::int32_t header_table_pointer_displacement = 0;

    bool operator==(const CallbackTablePointerOrigin&) const = default;
};

struct CallbackRecordPointerOrigin final {
    std::int32_t header_table_pointer_displacement = 0;
    std::uint32_t record_stride = 0u;

    bool operator==(const CallbackRecordPointerOrigin&) const = default;
};

struct CallbackRecordLoadOrigin final {
    std::int32_t header_table_pointer_displacement = 0;
    std::uint32_t record_stride = 0u;
    std::int32_t record_displacement = 0;
    std::uint32_t load_instruction_address = 0u;

    bool operator==(const CallbackRecordLoadOrigin&) const = default;
};

// Invocation-local producer/consumer lineage through exact global pointer
// cells. This does not model mutable RAM or establish a complete target set.
enum class GlobalVectorOriginKind : std::uint8_t {
    CallResult, PointerCell, IndexedRecord, RecordVector, CallbackWord,
    ResidentTable, ResidentCallback, IndexedHeaderRecord, HeaderCallback
};
struct GlobalVectorOrigin final {
    GlobalVectorOriginKind kind = GlobalVectorOriginKind::CallResult;
    std::uint32_t address = 0u; // callee or canonical global cell
    std::uint32_t stride = 0u;
    std::int32_t displacement = 0;
    std::uint32_t load_instruction_address = 0u;
    // Keep both selector identities, rather than replacing two independent
    // indices with a guessed flat stride or a complete selector range.
    std::array<CallbackScaledIndexOrigin, 2u> resident_indices{};
    std::uint8_t resident_index_count = 0u;
    std::int32_t header_displacement = 0;
    bool operator==(const GlobalVectorOrigin&) const = default;
};

struct CallbackValue final {
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t input_mask = 0u;
    // Subset of input_mask whose bits still denote the byte-identical incoming
    // ABI value.  input_mask intentionally tracks broad address/data influence
    // through memory loads; callback-sink contracts must not consume that
    // broader lane because a pointer to a descriptor is not the callback word
    // stored in the descriptor.  Only register moves, exact stack spills and
    // zero-valued arithmetic preserve this identity lane.
    std::uint8_t direct_input_mask = 0u;
    // Bit 0..3 identifies an incoming record pointer or a pointer reached by
    // following one or more 32-bit fields from that record. Arithmetic drops
    // this lane; it exists solely to prove record->callback(record) families.
    std::uint8_t record_family_input_mask = 0u;
    std::set<std::uint32_t> constants;
    bool constants_truncated = false;
    // True only when constants is the complete finite scalar domain on every
    // path represented by this value. Ordinary positive constants remain
    // useful without this bit, but caller-bounded closure must not consume
    // them as an exhaustive argument domain.
    bool constants_complete = false;
    // Positive origin proof for fixed R0 field displacements. PC literals and
    // memory loads must not gain this proof when arithmetic erases their other
    // provenance. Copies preserve it; joins require it on every incoming path.
    bool immediate_scalar_origin = false;
    // MUST origin for conditional source-call arguments. Unlike scalar
    // constants, this lane never survives a load from mutable memory/stack.
    bool source_register_constant = false;
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
    // slot_origin denotes table[index]; member_origin denotes a record loaded
    // from that slot (or reached through a 32-bit link from such a record).
    std::optional<CallbackRecordTableOrigin> record_table_slot_origin;
    std::optional<CallbackRecordTableOrigin> record_table_member_origin;
    // Structural provenance for descriptor tables whose header supplies a
    // record base and whose runtime selector is expanded into a bounded byte
    // stride.  This lane deliberately carries no address or target value: it
    // only survives lossless register moves/arithmetic and is consumed later
    // by the identity-bound latent-module scanner.
    std::optional<CallbackScaledIndexOrigin> scaled_index_origin;
    std::optional<CallbackTablePointerOrigin> table_pointer_origin;
    std::optional<CallbackRecordPointerOrigin> record_pointer_origin;
    std::optional<CallbackRecordLoadOrigin> record_load_origin;
    std::optional<GlobalVectorOrigin> global_vector_origin;
    std::optional<std::int32_t> stack_address;
    bool may_be_stack = false;
    std::vector<CallbackFieldOrigin> field_origins;
    bool field_origins_truncated = false;
    // Unlike the positive field_origins union, this is a must-origin for the
    // unchanged loaded word. Unknown/different joins and arithmetic drop it.
    std::optional<CopiedFieldIdentity> copied_field_identity;

    bool operator==(const CallbackValue&) const = default;
};

struct CallbackState final {
    std::array<CallbackValue, 16> registers;
    std::map<std::int32_t, CallbackValue> stack_values;

    bool operator==(const CallbackState&) const = default;
};

[[nodiscard]] std::uint8_t exact_incoming_argument_mask(const CallbackValue& value) {
    return value.affine_input.has_value() && value.affine_input->scale == 1u &&
           value.affine_input->argument < 4u
        ? static_cast<std::uint8_t>(1u << value.affine_input->argument) : 0u;
}

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
    std::uint8_t receiver_argument_mask = 0u;

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

struct CallbackRecordTablePublication final {
    CallbackValue record;
    CallbackRecordTableOrigin table;
    std::uint32_t instruction_address = 0u;

    bool operator==(const CallbackRecordTablePublication&) const = default;
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
    std::uint8_t local_record_sink_mask = 0u;
    std::uint8_t local_persistent_pointer_mask = 0u;
    std::vector<CallbackCall> calls;
    std::vector<CallbackFieldSink> field_sinks;
    std::vector<CallbackPersistentStore> persistent_stores;
    std::vector<CallbackRecordTablePublication> record_table_publications;
    std::map<std::uint32_t, CallbackIndexedPersistentStore>
        indexed_persistent_stores;
    std::vector<StoredCodeAddressCandidate> local_candidates;
    bool local_candidates_truncated = false;
    bool field_sinks_truncated = false;
    bool persistent_stores_truncated = false;
    std::set<std::uint32_t> finite_return_constants;
    bool finite_return_complete = false;
    // A site has a value only if all visits retain the same exact lineage.
    std::map<std::uint32_t, std::optional<GlobalVectorOrigin>> global_vector_sinks;
    // Observe even unknown and stack writes: a later visit must not revive a
    // copy proof after another path used a different value or destination.
    std::map<std::uint32_t, std::optional<CopiedFieldIdentity>> field_copy_stores;
    bool field_copy_stores_truncated = false;
};

// The retained cache owns only the immutable per-function model.  Values
// derived from cross-function receiver/sink composition stay invocation-local;
// copying these small summaries avoids cloning every call, field origin and
// persistent-store vector on each fixpoint cache hit.
struct CallbackFunctionAnalysisState final {
    std::shared_ptr<const CallbackFunctionModel> model;
    std::uint8_t local_sink_mask = 0u;
    std::uint8_t local_record_sink_mask = 0u;
    std::uint8_t local_persistent_pointer_mask = 0u;
    std::vector<StoredCodeAddressCandidate> local_candidates;
    bool local_candidates_truncated = false;
};

[[nodiscard]] CallbackFunctionAnalysisState make_analysis_state(
    std::shared_ptr<const CallbackFunctionModel> model) {
    CallbackFunctionAnalysisState result;
    result.local_sink_mask = model->local_sink_mask;
    result.local_record_sink_mask = model->local_record_sink_mask;
    result.local_persistent_pointer_mask =
        model->local_persistent_pointer_mask;
    result.local_candidates = model->local_candidates;
    result.local_candidates_truncated = model->local_candidates_truncated;
    result.model = std::move(model);
    return result;
}

// These are retained-session cache budgets, not a claim about the analyzer's
// total transient memory. Invocation-local composition remains governed by
// the existing callback candidate/forwarding budgets and is never retained.
constexpr std::size_t maximum_model_cache_entries = 8192u;
constexpr std::size_t maximum_model_cache_bytes = 256u * 1024u * 1024u;

[[nodiscard]] std::size_t saturated_add(const std::size_t left,
                                         const std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        return std::numeric_limits<std::size_t>::max();
    return left + right;
}

[[nodiscard]] std::size_t saturated_multiply(const std::size_t left,
                                              const std::size_t right) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        return std::numeric_limits<std::size_t>::max();
    return left * right;
}

template <typename T>
[[nodiscard]] std::size_t retained_set_bytes(const std::set<T>& values) {
    // A node-based container has no capacity to report.  Include the value
    // and four links/color/allocator words per retained node; this is a
    // deliberately conservative accounting bound for admission decisions.
    constexpr std::size_t node_overhead = 4u * sizeof(void*);
    return saturated_multiply(
        values.size(), saturated_add(sizeof(T), node_overhead));
}

[[nodiscard]] std::size_t estimate_callback_field_origin_bytes(
    const CallbackFieldOrigin& origin) {
    std::size_t bytes = retained_set_bytes(origin.receiver_constants);
    bytes = saturated_add(
        bytes, saturated_multiply(origin.receiver_progressions.capacity(),
                                  sizeof(ReceiverProgression)));
    return bytes;
}

[[nodiscard]] std::size_t estimate_callback_value_bytes(
    const CallbackValue& value) {
    std::size_t bytes = retained_set_bytes(value.constants);
    bytes = saturated_add(bytes, retained_set_bytes(value.code_constants));
    bytes = saturated_add(bytes, retained_set_bytes(value.receiver_constants));
    bytes = saturated_add(
        bytes, saturated_multiply(value.receiver_progressions.capacity(),
                                  sizeof(ReceiverProgression)));
    bytes = saturated_add(
        bytes, saturated_multiply(value.field_origins.capacity(),
                                  sizeof(CallbackFieldOrigin)));
    for (const auto& origin : value.field_origins)
        bytes = saturated_add(bytes,
                              estimate_callback_field_origin_bytes(origin));
    return bytes;
}

[[nodiscard]] std::size_t estimate_stored_candidate_bytes(
    const StoredCodeAddressCandidate& candidate) {
    std::size_t bytes = saturated_multiply(
        candidate.store_instruction_addresses.capacity(), sizeof(std::uint32_t));
    bytes = saturated_add(
        bytes, saturated_multiply(candidate.evidence_call_sites.capacity(),
                                  sizeof(std::uint32_t)));
    bytes = saturated_add(
        bytes, saturated_multiply(candidate.evidence_callees.capacity(),
                                  sizeof(std::uint32_t)));
    return bytes;
}

[[nodiscard]] std::size_t estimate_callback_model_bytes(
    const CallbackFunctionModel& model) {
    std::size_t bytes = sizeof(CallbackFunctionModel);
    bytes = saturated_add(bytes, retained_set_bytes(model.finite_return_constants));
    bytes = saturated_add(bytes, saturated_multiply(
        model.global_vector_sinks.size(), sizeof(GlobalVectorOrigin) + 6u * sizeof(void*)));
    bytes = saturated_add(bytes, saturated_multiply(
        model.field_copy_stores.size(), sizeof(CopiedFieldIdentity) + 6u * sizeof(void*)));
    bytes = saturated_add(
        bytes, saturated_multiply(model.calls.capacity(),
                                  sizeof(CallbackCall)));
    for (const auto& call : model.calls)
        for (const auto& argument : call.arguments)
            bytes = saturated_add(
                bytes, estimate_callback_value_bytes(argument));

    bytes = saturated_add(
        bytes, saturated_multiply(model.field_sinks.capacity(),
                                  sizeof(CallbackFieldSink)));
    for (const auto& sink : model.field_sinks)
        bytes = saturated_add(
            bytes, estimate_callback_field_origin_bytes(sink.field));

    bytes = saturated_add(
        bytes, saturated_multiply(model.persistent_stores.capacity(),
                                  sizeof(CallbackPersistentStore)));
    for (const auto& store : model.persistent_stores) {
        bytes = saturated_add(
            bytes, estimate_callback_value_bytes(store.source));
        bytes = saturated_add(bytes,
                              estimate_callback_value_bytes(store.receiver));
    }
    bytes = saturated_add(
        bytes, saturated_multiply(
                   model.record_table_publications.capacity(),
                   sizeof(CallbackRecordTablePublication)));
    for (const auto& publication : model.record_table_publications)
        bytes = saturated_add(
            bytes, estimate_callback_value_bytes(publication.record));

    constexpr std::size_t map_node_overhead = 4u * sizeof(void*);
    bytes = saturated_add(
        bytes, saturated_multiply(
                   model.indexed_persistent_stores.size(),
                   saturated_add(
                       sizeof(std::pair<const std::uint32_t,
                                         CallbackIndexedPersistentStore>),
                       map_node_overhead)));
    for (const auto& [site, store] : model.indexed_persistent_stores) {
        static_cast<void>(site);
        bytes = saturated_add(
            bytes, estimate_callback_value_bytes(store.source));
    }

    bytes = saturated_add(
        bytes, saturated_multiply(model.local_candidates.capacity(),
                                  sizeof(StoredCodeAddressCandidate)));
    for (const auto& candidate : model.local_candidates)
        bytes = saturated_add(
            bytes, estimate_stored_candidate_bytes(candidate));
    return bytes;
}

struct CallbackModelLineBinding final {
    std::uint32_t address = 0u;
    std::uint16_t opcode = 0u;
    bool is_delay_slot = false;
    std::optional<std::uint32_t> target_address;

    bool operator==(const CallbackModelLineBinding&) const = default;
};

struct CallbackModelBlockBinding final {
    std::uint32_t start_address = 0u;
    std::uint32_t end_address = 0u;
    std::vector<std::uint32_t> successors;
    bool has_indirect_successor = false;
    std::vector<CallbackModelLineBinding> lines;

    bool operator==(const CallbackModelBlockBinding&) const = default;
};

struct CallbackModelInputBinding final {
    std::uint32_t entry_address = 0u;
    std::uint32_t size = 0u;
    std::vector<std::uint32_t> block_addresses;
    std::vector<std::uint32_t> direct_callees;
    std::vector<std::uint32_t> indirect_call_sites;
    std::vector<std::uint32_t> shared_block_addresses;
    std::vector<std::uint32_t> tail_jump_targets;
    ControlFlowEvidence evidence = ControlFlowEvidence::ProvenComplete;
    std::vector<CallbackModelBlockBinding> blocks;

    bool operator==(const CallbackModelInputBinding&) const = default;
};

[[nodiscard]] std::size_t estimate_callback_binding_bytes(
    const CallbackModelInputBinding& binding) {
    std::size_t bytes = sizeof(CallbackModelInputBinding);
    const auto add_addresses = [&](const auto& addresses) {
        bytes = saturated_add(
            bytes, saturated_multiply(addresses.capacity(),
                                      sizeof(std::uint32_t)));
    };
    add_addresses(binding.block_addresses);
    add_addresses(binding.direct_callees);
    add_addresses(binding.indirect_call_sites);
    add_addresses(binding.shared_block_addresses);
    add_addresses(binding.tail_jump_targets);
    bytes = saturated_add(
        bytes, saturated_multiply(binding.blocks.capacity(),
                                  sizeof(CallbackModelBlockBinding)));
    for (const auto& block : binding.blocks) {
        add_addresses(block.successors);
        bytes = saturated_add(
            bytes, saturated_multiply(block.lines.capacity(),
                                      sizeof(CallbackModelLineBinding)));
    }
    // Account conservatively for the shared owner and the node-based cache
    // map envelope in addition to the dynamic binding/model payloads.
    return saturated_add(bytes, 6u * sizeof(void*));
}

[[nodiscard]] std::optional<CallbackModelInputBinding>
callback_model_input_binding(
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks) {
    CallbackModelInputBinding binding;
    binding.entry_address = function.entry_address;
    binding.size = function.size;
    binding.block_addresses = function.block_addresses;
    binding.direct_callees = function.direct_callees;
    binding.indirect_call_sites = function.indirect_call_sites;
    binding.shared_block_addresses = function.shared_block_addresses;
    binding.tail_jump_targets = function.tail_jump_targets;
    binding.evidence = function.evidence;
    binding.blocks.reserve(function.block_addresses.size());
    for (const auto address : function.block_addresses) {
        const auto found = blocks.find(address);
        if (found == blocks.end()) return std::nullopt;
        const auto& block = *found->second;
        CallbackModelBlockBinding block_binding;
        block_binding.start_address = block.start_address;
        block_binding.end_address = block.end_address;
        block_binding.successors = block.successors;
        block_binding.has_indirect_successor = block.has_indirect_successor;
        block_binding.lines.reserve(block.lines.size());
        for (const auto& line : block.lines) {
            block_binding.lines.push_back({line.address, line.opcode,
                                           line.is_delay_slot,
                                           line.target_address});
        }
        binding.blocks.push_back(std::move(block_binding));
    }
    return binding;
}

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
                if (displacement.has_value()) {
                    std::uint8_t receiver_argument_mask = 0u;
                    const auto load_index = index - distance - 1u;
                    const auto receiver_register =
                        candidate.instruction.source_register;
                    if (receiver_register < 16u && index + 1u < lines.size() &&
                        lines[index + 1u].address ==
                            lines[index].address + 2u &&
                        lines[index + 1u].is_delay_slot) {
                        std::uint16_t aliases = static_cast<std::uint16_t>(
                            1u << receiver_register);
                        for (std::size_t cursor = load_index;
                             cursor <= index + 1u; ++cursor) {
                            const auto& instruction =
                                lines[cursor].instruction;
                            const auto register_writes =
                                general_register_write_mask(instruction);
                            const bool moved_alias =
                                instruction.kind ==
                                    katana::sh4::InstructionKind::MovRegister &&
                                instruction.source_register < 16u &&
                                instruction.destination_register < 16u &&
                                (aliases & static_cast<std::uint16_t>(
                                               1u << instruction.source_register)) !=
                                    0u;
                            aliases = static_cast<std::uint16_t>(
                                aliases &
                                static_cast<std::uint16_t>(~register_writes));
                            if (moved_alias)
                                aliases = static_cast<std::uint16_t>(
                                    aliases |
                                    static_cast<std::uint16_t>(
                                        1u << instruction.destination_register));
                        }
                        for (std::uint8_t argument = 0u; argument < 4u;
                             ++argument) {
                            if ((aliases & static_cast<std::uint16_t>(
                                               1u << (4u + argument))) != 0u)
                                receiver_argument_mask =
                                    static_cast<std::uint8_t>(
                                        receiver_argument_mask |
                                        static_cast<std::uint8_t>(
                                            1u << argument));
                        }
                    }
                    result.push_back(
                        {function.entry_address,
                         lines[index].address,
                         candidate.address,
                         *displacement,
                         static_cast<std::uint8_t>(4u),
                         kind == katana::sh4::InstructionKind::Jsr,
                         receiver_argument_mask});
                }
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
    const auto canonical = native_entry_shapes.canonical_address(constant);
    if (!canonical.has_value()) return false;
    if (value.code_constants_truncated ||
        value.code_constants.contains(*canonical))
        return false;
    const auto status = native_entry_shapes.classify(*canonical);
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
    value.code_constants.insert(*canonical);
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

[[nodiscard]] std::optional<CallbackRecordTableOrigin>
exact_record_table_slot_origin(const CallbackValue& table,
                               const CallbackValue& index) {
    std::optional<std::uint32_t> stride;
    if (index.affine_input.has_value()) {
        stride = index.affine_input->scale;
    } else if (index.input_mask != 0u && !index.memory_derived_receiver &&
               !index.may_be_stack) {
        // A common task-list selector maps one reserved incoming mode to a
        // fixed bucket before SHLL2. The control-flow join intentionally drops
        // affine_input because the value is no longer a pure affine function,
        // while input_mask plus guaranteed alignment still proves the exact
        // table[index * stride] address family. This lane is consumed only to
        // relate record publication to a later receiver load; it never claims
        // a complete index domain or indirect target set.
        stride = index.minimum_alignment;
    }
    if (!table.pc_literal_identity.has_value() ||
        !table.constants_complete || table.constants_truncated ||
        table.constants.size() != 1u ||
        *table.constants.begin() != table.pc_literal_identity->value ||
        !stride.has_value() ||
        *stride < sizeof(std::uint32_t) ||
        *stride > maximum_static_code_pointer_table_stride ||
        (*stride & 3u) != 0u)
        return std::nullopt;
    return CallbackRecordTableOrigin{
        table.pc_literal_identity->value,
        *stride};
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
    const auto base_mask = static_cast<std::uint8_t>(
        destination.receiver_base_argument_mask & source.receiver_base_argument_mask);
    changed = changed || base_mask != destination.receiver_base_argument_mask;
    destination.receiver_base_argument_mask = base_mask;
    const auto record_family_input_mask = static_cast<std::uint8_t>(
        destination.receiver_record_family_input_mask |
        source.receiver_record_family_input_mask);
    changed = changed ||
              record_family_input_mask !=
                  destination.receiver_record_family_input_mask;
    destination.receiver_record_family_input_mask =
        record_family_input_mask;
    if (destination.receiver_record_table_origin !=
        source.receiver_record_table_origin) {
        changed = changed ||
                  destination.receiver_record_table_origin.has_value();
        destination.receiver_record_table_origin.reset();
    }
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
    origin.receiver_base_argument_mask = exact_incoming_argument_mask(receiver);
    origin.receiver_record_family_input_mask =
        receiver.record_family_input_mask;
    origin.receiver_record_table_origin =
        receiver.record_table_member_origin;
    origin.receiver_constants = receiver.receiver_constants;
    origin.receiver_progressions = receiver.receiver_progressions;
    origin.receiver_constants_truncated =
        receiver.receiver_constants_truncated || receiver_identity_unknown;
    origin.displacement = displacement;
    origin.width = static_cast<std::uint8_t>(width);
    origin.load_instruction_address = instruction_address;
    if (origin.receiver_base_argument_mask != 0u && displacement >= 0 &&
        displacement <= 4096 && (displacement & 3) == 0) {
        for (std::uint8_t argument = 0u; argument < 4u; ++argument) {
            if (origin.receiver_base_argument_mask != (1u << argument)) continue;
            loaded.copied_field_identity =
                CopiedFieldIdentity{instruction_address, displacement, argument};
            break;
        }
    }
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
    value.immediate_scalar_origin = before.immediate_scalar_origin;
    value.source_register_constant = before.source_register_constant;
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
    const auto direct_input_mask = static_cast<std::uint8_t>(
        destination.direct_input_mask | source.direct_input_mask);
    changed = changed ||
              direct_input_mask != destination.direct_input_mask;
    destination.direct_input_mask = direct_input_mask;
    const auto record_family_input_mask = static_cast<std::uint8_t>(
        destination.record_family_input_mask |
        source.record_family_input_mask);
    changed = changed ||
              record_family_input_mask !=
                  destination.record_family_input_mask;
    destination.record_family_input_mask = record_family_input_mask;
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
    const bool immediate_scalar_origin = destination.immediate_scalar_origin &&
                                         source.immediate_scalar_origin;
    changed = changed || immediate_scalar_origin !=
                              destination.immediate_scalar_origin;
    destination.immediate_scalar_origin = immediate_scalar_origin;
    const bool source_register_constant = destination.source_register_constant &&
                                          source.source_register_constant;
    changed = changed || source_register_constant != destination.source_register_constant;
    destination.source_register_constant = source_register_constant;
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
    if (destination.record_table_slot_origin !=
        source.record_table_slot_origin) {
        changed = changed ||
                  destination.record_table_slot_origin.has_value();
        destination.record_table_slot_origin.reset();
    }
    if (destination.record_table_member_origin !=
        source.record_table_member_origin) {
        changed = changed ||
                  destination.record_table_member_origin.has_value();
        destination.record_table_member_origin.reset();
    }
    if (destination.scaled_index_origin != source.scaled_index_origin) {
        changed = changed || destination.scaled_index_origin.has_value();
        destination.scaled_index_origin.reset();
    }
    if (destination.table_pointer_origin != source.table_pointer_origin) {
        changed = changed || destination.table_pointer_origin.has_value();
        destination.table_pointer_origin.reset();
    }
    if (destination.record_pointer_origin != source.record_pointer_origin) {
        changed = changed || destination.record_pointer_origin.has_value();
        destination.record_pointer_origin.reset();
    }
    if (destination.record_load_origin != source.record_load_origin) {
        changed = changed || destination.record_load_origin.has_value();
        destination.record_load_origin.reset();
    }
    if (destination.global_vector_origin != source.global_vector_origin) {
        changed = changed || destination.global_vector_origin.has_value();
        destination.global_vector_origin.reset();
    }
    if (destination.copied_field_identity != source.copied_field_identity) {
        changed = changed || destination.copied_field_identity.has_value();
        destination.copied_field_identity.reset();
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
    result.record_family_input_mask =
        address.record_family_input_mask;
    if (address.record_table_slot_origin.has_value())
        result.record_table_member_origin =
            address.record_table_slot_origin;
    else if (address.record_table_member_origin.has_value())
        result.record_table_member_origin =
            address.record_table_member_origin;
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

void scale_index_origin(CallbackValue& value,
                        const std::uint32_t scale) noexcept {
    if (!value.scaled_index_origin.has_value()) return;
    if (scale == 0u ||
        value.scaled_index_origin->scale >
            maximum_static_code_pointer_table_stride / scale) {
        value.scaled_index_origin.reset();
        return;
    }
    value.scaled_index_origin->scale *= scale;
}

[[nodiscard]] std::optional<CallbackScaledIndexOrigin> normalized_index_origin(
    std::optional<CallbackScaledIndexOrigin> origin, const std::uint32_t pc) {
    if (!origin.has_value() || origin->scale != 1u) return std::nullopt;
    origin->selector_expression_address = pc;
    return origin;
}

[[nodiscard]] std::optional<GlobalVectorOrigin> resident_table_origin(
    const CallbackValue& table, const CallbackValue& index) {
    if (table.global_vector_origin.has_value() &&
        table.global_vector_origin->kind == GlobalVectorOriginKind::ResidentTable &&
        table.global_vector_origin->resident_index_count == 1u &&
        !table.may_be_stack && !index.may_be_stack && index.scaled_index_origin.has_value()) {
        const auto& term = *index.scaled_index_origin;
        const auto& first = table.global_vector_origin->resident_indices[0u];
        if (term.load_instruction_address == first.load_instruction_address ||
            term.scale < 4u || term.scale > maximum_static_code_pointer_table_stride ||
            (term.scale & 3u) != 0u || index.minimum_alignment % term.scale != 0u ||
            std::max(first.scale, term.scale) % std::min(first.scale, term.scale) != 0u)
            return std::nullopt;
        auto result = *table.global_vector_origin;
        result.resident_indices[result.resident_index_count++] = term;
        return result;
    }
    if (!table.pc_literal_identity.has_value() || !table.constants_complete ||
        table.constants_truncated || table.constants.size() != 1u ||
        *table.constants.begin() != table.pc_literal_identity->value ||
        table.may_be_stack || index.may_be_stack ||
        !index.scaled_index_origin.has_value()) return std::nullopt;
    const auto stride = index.scaled_index_origin->scale;
    if (stride < 4u || stride > maximum_static_code_pointer_table_stride ||
        (stride & 3u) != 0u || index.minimum_alignment % stride != 0u) return std::nullopt;
    auto result = GlobalVectorOrigin{GlobalVectorOriginKind::ResidentTable,
        table.pc_literal_identity->value, stride};
    result.resident_indices[0u] = *index.scaled_index_origin;
    result.resident_index_count = 1u;
    return result;
}

void attach_record_load_origin(CallbackValue& loaded,
                               const CallbackValue& base,
                               const std::int32_t displacement,
                               const std::size_t width,
                               const std::uint32_t instruction_address) {
    if (width != sizeof(std::uint32_t) || displacement < 0 ||
        (displacement & 3) != 0 || base.may_be_stack)
        return;
    if (base.pc_literal_identity.has_value()) {
        const auto raw = static_cast<std::uint64_t>(base.pc_literal_identity->value) +
                         static_cast<std::uint32_t>(displacement);
        if (raw <= std::numeric_limits<std::uint32_t>::max()) {
            const auto value = static_cast<std::uint32_t>(raw);
            const auto region = value & 0xE0000000u;
            const auto physical = value & 0x1FFFFFFFu;
            if ((region == 0u || region == 0x80000000u || region == 0xA0000000u) &&
                physical >= 0x0C000000u && physical < 0x10000000u &&
                (physical & 3u) == 0u)
                loaded.global_vector_origin = GlobalVectorOrigin{
                    GlobalVectorOriginKind::PointerCell, physical | 0x80000000u};
        }
    }
    if (base.global_vector_origin.has_value()) {
        const auto& origin = *base.global_vector_origin;
        if (origin.kind == GlobalVectorOriginKind::IndexedRecord &&
            static_cast<std::uint64_t>(displacement) + 4u <= origin.stride)
            loaded.global_vector_origin = GlobalVectorOrigin{
                GlobalVectorOriginKind::RecordVector, origin.address,
                origin.stride, displacement, instruction_address};
        else if (origin.kind == GlobalVectorOriginKind::PointerCell &&
                 displacement <= 252)
            loaded.global_vector_origin = GlobalVectorOrigin{
                GlobalVectorOriginKind::CallbackWord, origin.address,
                0u, displacement, instruction_address};
        else if (origin.kind == GlobalVectorOriginKind::ResidentTable && displacement == 0) {
            loaded.global_vector_origin = origin;
            loaded.global_vector_origin->kind = GlobalVectorOriginKind::ResidentCallback;
            loaded.global_vector_origin->load_instruction_address = instruction_address;
        } else if (origin.kind == GlobalVectorOriginKind::IndexedHeaderRecord &&
                   static_cast<std::uint64_t>(displacement) + 4u <= origin.stride) {
            loaded.global_vector_origin = origin;
            loaded.global_vector_origin->kind = GlobalVectorOriginKind::HeaderCallback;
            loaded.global_vector_origin->displacement = displacement;
            loaded.global_vector_origin->load_instruction_address = instruction_address;
        }
    }
    if (base.record_pointer_origin.has_value()) {
        const auto& record = *base.record_pointer_origin;
        if (record.record_stride < sizeof(std::uint32_t) ||
            static_cast<std::uint64_t>(displacement) +
                    sizeof(std::uint32_t) >
                record.record_stride)
            return;
        loaded.record_load_origin = CallbackRecordLoadOrigin{
            record.header_table_pointer_displacement,
            record.record_stride,
            displacement,
            instruction_address};
        return;
    }
    // A self-describing table stores its element count immediately before
    // the table pointer. Requiring at least one preceding word rules out a
    // raw pointer-valued array base; the module scanner later proves the
    // exact count, end address, entries, image identity and generation.
    if (displacement >= static_cast<std::int32_t>(sizeof(std::uint32_t)))
        loaded.table_pointer_origin =
            CallbackTablePointerOrigin{displacement};
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
        value.copied_field_identity.reset();
        value.scaled_index_origin.reset();
        value.direct_input_mask = 0u;
        value.affine_input.reset();
        value.pc_literal_identity.reset();
        value.table_pointer_origin.reset();
        value.record_pointer_origin.reset();
        value.record_load_origin.reset();
        value.global_vector_origin.reset();
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

struct StaticCodePointerVectorEntry final {
    std::uint32_t target = 0u;
    std::uint32_t slot = 0u;
};

struct ShortCodePointerVectorOccurrence final {
    std::array<StaticCodePointerVectorEntry,
               minimum_repeated_short_code_pointer_vector_entries>
        entries{};
};

struct RepeatedShortCodePointerVectorEvidence final {
    std::optional<ShortCodePointerVectorOccurrence> first;
    std::optional<ShortCodePointerVectorOccurrence> second;
};

using RepeatedShortCodePointerVectorMap =
    std::map<std::tuple<std::size_t, std::uint32_t, std::uint32_t>,
             RepeatedShortCodePointerVectorEvidence>;

[[nodiscard]] bool same_static_vector_source_component(
    const katana::io::ImageSegment& left,
    const katana::io::ImageSegment& right) noexcept {
    if (left.source_kind != right.source_kind ||
        left.load_phase != right.load_phase)
        return false;
    if (!left.local_source_name.empty() ||
        !right.local_source_name.empty())
        return !left.local_source_name.empty() &&
               left.local_source_name == right.local_source_name;
    // Without an explicit source identity, fail closed at exact-segment
    // scope. This still admits an independently rooted single-segment raw
    // image, but cannot merge unrelated anonymous modules merely because
    // their load phases happen to match.
    return &left == &right;
}

[[nodiscard]] std::optional<StaticCodePointerVectorEntry>
static_code_pointer_vector_entry_at(
    const katana::io::ExecutableImage& image,
    const katana::io::ImageSegment& segment,
    const std::span<const katana::io::ImageSegment* const>
        analyzed_source_components,
    const std::size_t entry_offset) {
    const auto slot64 =
        static_cast<std::uint64_t>(segment.virtual_address) + entry_offset;
    if (slot64 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto slot = static_cast<std::uint32_t>(slot64);
    const auto raw = read_image_u32(image, slot);
    if (!raw.has_value()) return std::nullopt;
    const auto target = executable_constant(image, *raw);
    if (!target.has_value()) return std::nullopt;
    const auto* target_segment =
        image.find_segment(*target, sizeof(std::uint16_t));
    if (target_segment == nullptr ||
        target_segment->load_phase != segment.load_phase)
        return std::nullopt;
    const auto active_target_component = std::any_of(
        analyzed_source_components.begin(), analyzed_source_components.end(),
        [&](const auto* active) {
            return active != nullptr &&
                   same_static_vector_source_component(
                       *target_segment, *active);
        });
    if (!active_target_component) return std::nullopt;
    return StaticCodePointerVectorEntry{*target, slot};
}

[[nodiscard]] StaticCodePointerVectorInventory
discover_static_code_pointer_vectors(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::io::ImageSegment* const>
        analyzed_source_components,
    GuardedNativeEntryShapeCache& native_entry_shapes,
    const std::span<const std::uint32_t> anchors = {}) {
    StaticCodePointerVectorInventory inventory;
    std::vector<const katana::io::ImageSegment*>
        vector_source_components;
    RepeatedShortCodePointerVectorMap repeated_short_vectors;
    // Constructors commonly install a static callback descriptor into a
    // mutable object before any consumer can expose the descriptor base to
    // value analysis. That creates a real dependency cycle: the constructor
    // is reachable only through the very vector whose address it publishes.
    //
    // Four contiguous, aligned and independently decodable function entries
    // in an identity-bound image are a sufficiently strong positive shape to
    // break that cycle. A two-entry descriptor is admitted only when the
    // exact ordered pair occurs twice, without overlap, in the same bound
    // source component and both targets independently satisfy the same native
    // entry-shape proof. This covers repeated short vtables without turning an
    // incidental scalar pair into inventory. Both rules remain guarded
    // inventory only. They neither resolve an indirect transfer nor claim the
    // vector is complete at runtime.
    for (const auto& segment : image.segments()) {
        // A vector may intentionally live in a carrier component such as the
        // Dreamcast system bootstrap while naming callbacks in the selected
        // disc executable. The carrier therefore need not own an analyzed
        // function. Only the promoted target must belong to an active source
        // component in this ExecutableImage generation.
        if (!segment.permissions.readable || segment.bytes.size() < 8u)
            continue;
        auto component = std::find_if(
            vector_source_components.begin(),
            vector_source_components.end(),
            [&](const auto* representative) {
                return representative != nullptr &&
                       same_static_vector_source_component(
                           segment, *representative);
            });
        if (component == vector_source_components.end()) {
            vector_source_components.push_back(&segment);
            component = std::prev(vector_source_components.end());
        }
        const auto component_index = static_cast<std::size_t>(
            std::distance(vector_source_components.begin(), component));
        const auto first_offset = static_cast<std::size_t>(
            (4u - (segment.virtual_address & 3u)) & 3u);
        auto offset = first_offset;
        while (offset <= segment.bytes.size() - 4u) {
            const auto entry_at = [&](const std::size_t entry_offset) {
                return static_code_pointer_vector_entry_at(
                    image, segment, analyzed_source_components, entry_offset);
            };
            // First identify a raw contiguous code-pointer run without
            // decoding any target. Ordinary data contains many isolated code
            // pointers; validating each of those would let unrelated data
            // consume the global entry-shape budget before a real vector is
            // reached. Only a run which can satisfy the four-entry contract
            // pays for independent CFG validation. Do not apply the 256-entry
            // exhaustive typed-table limit here: this inventory is guarded
            // positive evidence, and large images legitimately concatenate
            // multiple callback families. The global candidate and shape-work
            // budgets below remain the fail-closed bounds.
            auto cursor = offset;
            for (; cursor <= segment.bytes.size() - 4u &&
                   entry_at(cursor).has_value();
                 cursor += 4u) {}

            const auto raw_entry_count = (cursor - offset) / 4u;
            if (!anchors.empty()) {
                const auto first_slot =
                    static_cast<std::uint64_t>(segment.virtual_address) + offset;
                const auto end_slot =
                    static_cast<std::uint64_t>(segment.virtual_address) + cursor;
                const auto anchor = std::lower_bound(
                    anchors.begin(), anchors.end(), first_slot);
                if (anchor == anchors.end() || *anchor >= end_slot) {
                    if (cursor > segment.bytes.size() - 4u) break;
                    offset = cursor + 4u;
                    continue;
                }
            }
            if (raw_entry_count >=
                minimum_repeated_short_code_pointer_vector_entries) {
                for (auto pair_offset = offset;
                     pair_offset + 8u <= cursor;
                     pair_offset += 4u) {
                    const auto first = entry_at(pair_offset);
                    const auto second = entry_at(pair_offset + 4u);
                    if (!first.has_value() || !second.has_value()) {
                        inventory.truncated = true;
                        return inventory;
                    }
                    const auto key = std::tuple{
                        component_index, first->target, second->target};
                    const auto [found, inserted] =
                        repeated_short_vectors.try_emplace(key);
                    if (inserted && repeated_short_vectors.size() >
                                        maximum_repeated_short_code_pointer_vector_keys) {
                        inventory.truncated = true;
                        return inventory;
                    }
                    const ShortCodePointerVectorOccurrence occurrence{
                        {*first, *second}};
                    if (!found->second.first.has_value()) {
                        found->second.first = occurrence;
                        continue;
                    }
                    if (found->second.second.has_value()) continue;
                    const auto first_start = static_cast<std::uint64_t>(
                        found->second.first->entries.front().slot);
                    const auto candidate_start =
                        static_cast<std::uint64_t>(first->slot);
                    if (first_start + 8u <= candidate_start ||
                        candidate_start + 8u <= first_start)
                        found->second.second = occurrence;
                }
            }
            if (raw_entry_count >=
                minimum_static_code_pointer_vector_entries) {
                std::array<StaticCodePointerVectorEntry,
                           minimum_static_code_pointer_vector_entries>
                    pending{};
                std::size_t pending_count = 0u;
                bool valid_run_admitted = false;
                const auto publish =
                    [&](const StaticCodePointerVectorEntry& entry) {
                    add_candidate(inventory.candidates, image,
                                  entry.target, entry.slot);
                    return inventory.candidates.size() <=
                           maximum_inventory_candidates;
                };
                for (auto entry_offset = offset;
                     entry_offset < cursor;
                     entry_offset += 4u) {
                    const auto entry = entry_at(entry_offset);
                    if (!entry.has_value()) {
                        inventory.truncated = true;
                        return inventory;
                    }
                    const auto status =
                        native_entry_shapes.classify(entry->target);
                    if (status == GuardedNativeEntryShapeStatus::
                                      ShapeBudgetExceeded) {
                        inventory.truncated = true;
                        return inventory;
                    }
                    if (status == GuardedNativeEntryShapeStatus::Valid) {
                        if (valid_run_admitted) {
                            if (!publish(*entry)) {
                                inventory.truncated = true;
                                return inventory;
                            }
                            continue;
                        }
                        pending[pending_count++] = *entry;
                        if (pending_count < pending.size()) continue;
                        for (const auto& admitted : pending) {
                            if (!publish(admitted)) {
                                inventory.truncated = true;
                                return inventory;
                            }
                        }
                        valid_run_admitted = true;
                        continue;
                    }
                    pending_count = 0u;
                    valid_run_admitted = false;
                }
            }
            // cursor names the first rejected word. It cannot begin a valid
            // vector; resume at the following aligned slot. At end-of-segment
            // the overflow-safe assignment simply terminates the outer loop.
            if (cursor > segment.bytes.size() - 4u) break;
            offset = cursor + 4u;
        }
    }
    const auto publish_unique =
        [&](const StaticCodePointerVectorEntry& entry) {
        const auto existing = std::find_if(
            inventory.candidates.begin(), inventory.candidates.end(),
            [&](const auto& candidate) {
                return candidate.target_address == entry.target;
            });
        if (existing != inventory.candidates.end() &&
            std::find(existing->store_instruction_addresses.begin(),
                      existing->store_instruction_addresses.end(),
                      entry.slot) !=
                existing->store_instruction_addresses.end())
            return true;
        add_candidate(inventory.candidates, image, entry.target, entry.slot);
        return inventory.candidates.size() <= maximum_inventory_candidates;
    };
    for (const auto& [key, evidence] : repeated_short_vectors) {
        (void)key;
        if (!evidence.first.has_value() ||
            !evidence.second.has_value())
            continue;
        bool valid = true;
        for (const auto& entry : evidence.first->entries) {
            const auto status = native_entry_shapes.classify(entry.target);
            if (status ==
                GuardedNativeEntryShapeStatus::ShapeBudgetExceeded) {
                inventory.truncated = true;
                return inventory;
            }
            valid = valid &&
                    status == GuardedNativeEntryShapeStatus::Valid;
        }
        if (!valid) continue;
        for (const auto* occurrence :
             {&*evidence.first, &*evidence.second}) {
            for (const auto& entry : occurrence->entries) {
                if (publish_unique(entry)) continue;
                inventory.truncated = true;
                return inventory;
            }
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

void observe_persistent_store(CallbackFunctionAnalysisState& state,
                              const katana::io::ExecutableImage& image,
                              const CallbackValue& source,
                              const std::uint32_t instruction_address,
                              const std::size_t width,
                              const bool receiver_identity_proven,
                              const bool record_receiver_proven) {
    if (width != 4u) return;
    // A raw field displacement plus an unproven receiver is not an ABI type.
    // Keep concrete executable literals as guarded positive inventory, but
    // only advertise an incoming argument as a callback when the store and
    // the indirect-load sink share an exact or same-local receiver. Otherwise
    // an ordinary data pointer stored at (for example) record +4 poisons every
    // caller of the constructor as a higher-order callback API.
    if (receiver_identity_proven) {
        state.local_sink_mask = static_cast<std::uint8_t>(
            state.local_sink_mask | source.direct_input_mask);
        if (record_receiver_proven)
            state.local_record_sink_mask = static_cast<std::uint8_t>(
                state.local_record_sink_mask |
                source.direct_input_mask);
    }
    if (source.code_constants_truncated)
        state.local_candidates_truncated = true;
    // The field offset is derived from an actual indirect-call load. Preserve
    // bounded executable scalars through this local model; the terminal
    // inventory gate below still requires either a complete standalone shape
    // or an independent non-root function-entry hint before publication.
    for (const auto constant : source.constants)
        add_candidate(state.local_candidates, image, constant,
                      instruction_address);
    for (const auto constant : source.code_constants)
        add_candidate(state.local_candidates, image, constant,
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
         !source.code_constants_truncated && !source.global_vector_origin.has_value()))
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
    const auto copy = width == 4u && !destination.may_be_stack && !indexed_addressing
        ? source.copied_field_identity : std::nullopt;
    const auto observed = model.field_copy_stores.find(instruction_address);
    if (observed != model.field_copy_stores.end()) {
        if (observed->second != copy) observed->second.reset();
    } else if (model.field_copy_stores.size() < maximum_persistent_store_observations) {
        model.field_copy_stores.emplace(instruction_address, copy);
    } else {
        model.field_copy_stores_truncated = true;
    }
    const auto stack_address = displaced_stack_address(destination,
                                                       displacement);
    if (stack_address.has_value()) {
        erase_overlapping_stack_values(state, *stack_address, width);
        if (width == 4u && state.stack_values.size() < maximum_stack_values)
            state.stack_values.insert_or_assign(*stack_address, source);
        return;
    }
    if (destination.may_be_stack) {
        for (auto& [slot, value] : state.stack_values) {
            static_cast<void>(slot);
            value.copied_field_identity.reset();
        }
    }
    // An imprecise SP-derived address may still be a spill. It cannot prove a
    // persistent callback store, so remain fail-closed and do not promote it.
    if (width == sizeof(std::uint32_t) && !source.may_be_stack &&
        destination.record_table_slot_origin.has_value()) {
        const auto existing = std::find_if(
            model.record_table_publications.begin(),
            model.record_table_publications.end(),
            [&](const auto& publication) {
                return publication.instruction_address ==
                           instruction_address &&
                       publication.table ==
                           *destination.record_table_slot_origin;
            });
        if (existing != model.record_table_publications.end()) {
            static_cast<void>(join_value(existing->record, source,
                                         native_entry_shapes));
        } else if (model.record_table_publications.size() <
                   maximum_persistent_store_observations) {
            model.record_table_publications.push_back(
                {source, *destination.record_table_slot_origin,
                 instruction_address});
        } else {
            model.persistent_stores_truncated = true;
        }
    }
    observe_potential_persistent_store(
        model, source, destination, displacement,
        instruction_address, width, indexed_addressing,
        native_entry_shapes);
}

[[nodiscard]] bool has_exact_stack_value(const CallbackState& state,
                                         const CallbackValue& source,
                                         const std::int32_t displacement,
                                         const std::size_t width) {
    const auto address = displaced_stack_address(source, displacement);
    return width == 4u && address.has_value() &&
           state.stack_values.contains(*address);
}

[[nodiscard]] CallbackValue load_value(const CallbackState& state,
                                       const CallbackValue& source,
                                       const std::int32_t displacement,
                                       const std::size_t width) {
    const auto stack_address = displaced_stack_address(source, displacement);
    if (stack_address.has_value() && width == 4u) {
        const auto found = state.stack_values.find(*stack_address);
        if (found != state.stack_values.end()) {
            auto result = found->second;
            result.source_register_constant = false;
            return result;
        }
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
    if (width == sizeof(std::uint32_t) && !source.may_be_stack)
        result.record_family_input_mask =
            source.record_family_input_mask;
    if (width == sizeof(std::uint32_t) && !source.may_be_stack) {
        if (source.record_table_slot_origin.has_value())
            result.record_table_member_origin =
                source.record_table_slot_origin;
        else if (source.record_table_member_origin.has_value())
            result.record_table_member_origin =
                source.record_table_member_origin;
    }
    return result;
}

[[nodiscard]] bool immediate_field_displacement(const CallbackValue& value) {
    // MOV/EXTU/ADD commonly assemble a fixed object-field offset in R0.
    // A surviving positive constant is insufficient after a join with an
    // unknown index. Retain the indexed-table proof for literal addresses,
    // incoming selectors and memory-derived values; this lane accepts only
    // complete, immediate scalar offsets in the signed/unsigned 16-bit range.
    if (!value.immediate_scalar_origin || !value.constants_complete || value.constants_truncated ||
        value.constants.size() != 1u || value.input_mask != 0u ||
        value.may_be_stack || value.memory_derived_receiver ||
        value.pc_literal_identity.has_value() || value.affine_input.has_value() ||
        value.scaled_index_origin.has_value())
        return false;
    const auto scalar = *value.constants.begin();
    return scalar <= 0xFFFFu || scalar >= 0xFFFF8000u;
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
        state.registers[destination].immediate_scalar_origin = true;
        state.registers[destination].source_register_constant = true;
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
        {
        const auto index_origin = normalized_index_origin(
            state.registers[destination].scaled_index_origin, line.address);
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value & 0xFFu; });
        state.registers[destination].scaled_index_origin = index_origin;
        }
        return;
    case K::ExtendUnsignedWord:
        state.registers[destination] = state.registers[source];
        {
        const auto index_origin = normalized_index_origin(
            state.registers[destination].scaled_index_origin, line.address);
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value & 0xFFFFu; });
        state.registers[destination].scaled_index_origin = index_origin;
        }
        return;
    case K::ExtendSignedByte:
        state.registers[destination] = state.registers[source];
        {
        const auto index_origin = normalized_index_origin(
            state.registers[destination].scaled_index_origin, line.address);
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) {
                return static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(value)));
            });
        state.registers[destination].scaled_index_origin = index_origin;
        }
        return;
    case K::ExtendSignedWord:
        state.registers[destination] = state.registers[source];
        {
        const auto index_origin = normalized_index_origin(
            state.registers[destination].scaled_index_origin, line.address);
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) {
                return static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int16_t>(value)));
            });
        state.registers[destination].scaled_index_origin = index_origin;
        }
        return;
    case K::ShiftLogicalLeftOne:
    case K::ShiftArithmeticLeftOne: {
        const auto affine = state.registers[destination].affine_input;
        const auto index_origin =
            state.registers[destination].scaled_index_origin;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 1u; });
        state.registers[destination].affine_input = affine;
        state.registers[destination].scaled_index_origin = index_origin;
        scale_affine_input(state.registers[destination], 2u);
        scale_index_origin(state.registers[destination], 2u);
        scale_minimum_alignment(state.registers[destination], 2u);
        return;
    }
    case K::ShiftLogicalLeftTwo: {
        const auto affine = state.registers[destination].affine_input;
        const auto index_origin =
            state.registers[destination].scaled_index_origin;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 2u; });
        state.registers[destination].affine_input = affine;
        state.registers[destination].scaled_index_origin = index_origin;
        scale_affine_input(state.registers[destination], 4u);
        scale_index_origin(state.registers[destination], 4u);
        scale_minimum_alignment(state.registers[destination], 4u);
        return;
    }
    case K::ShiftLogicalLeftEight: {
        const auto affine = state.registers[destination].affine_input;
        const auto index_origin =
            state.registers[destination].scaled_index_origin;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 8u; });
        state.registers[destination].affine_input = affine;
        state.registers[destination].scaled_index_origin = index_origin;
        scale_affine_input(state.registers[destination], 256u);
        scale_index_origin(state.registers[destination], 256u);
        scale_minimum_alignment(state.registers[destination], 256u);
        return;
    }
    case K::ShiftLogicalLeftSixteen: {
        const auto affine = state.registers[destination].affine_input;
        const auto index_origin =
            state.registers[destination].scaled_index_origin;
        transform_scalar_value(
            state.registers[destination], image, native_entry_shapes,
            [](const std::uint32_t value) { return value << 16u; });
        state.registers[destination].affine_input = affine;
        state.registers[destination].scaled_index_origin = index_origin;
        scale_affine_input(state.registers[destination], 65'536u);
        scale_index_origin(state.registers[destination], 65'536u);
        scale_minimum_alignment(state.registers[destination], 65'536u);
        return;
    }
    case K::AndImmediate: {
        const auto index_origin = normalized_index_origin(
            state.registers[destination].scaled_index_origin, line.address);
        set_unknown(state.registers[destination]);
        state.registers[destination].scaled_index_origin = index_origin;
        return;
    }
    case K::AndRegister: {
        const auto left = state.registers[destination];
        set_unknown(state.registers[destination]);
        // A mask changes the selector domain, but every surviving selector
        // is still expanded by the subsequently proven byte stride. The
        // origin is structural rather than a claim that the scalar transform
        // remained affine.
        state.registers[destination].scaled_index_origin =
            normalized_index_origin(left.scaled_index_origin, line.address);
        return;
    }
    case K::AddRegister: {
        const auto left = state.registers[destination];
        const auto right = state.registers[source];
        std::optional<CallbackScaledIndexOrigin> index_origin;
        if (left.scaled_index_origin.has_value() &&
            right.scaled_index_origin.has_value() &&
            left.scaled_index_origin->load_instruction_address ==
                right.scaled_index_origin->load_instruction_address &&
            left.scaled_index_origin->selector_expression_address ==
                right.scaled_index_origin->selector_expression_address &&
            left.scaled_index_origin->scale <=
                maximum_static_code_pointer_table_stride -
                    right.scaled_index_origin->scale) {
            index_origin = left.scaled_index_origin;
            index_origin->scale += right.scaled_index_origin->scale;
        }

        std::optional<CallbackRecordPointerOrigin> record_origin;
        auto record_table_slot_origin =
            exact_record_table_slot_origin(left, right);
        if (!record_table_slot_origin.has_value())
            record_table_slot_origin =
                exact_record_table_slot_origin(right, left);
        const auto combine_record =
            [&](const CallbackValue& table,
                const CallbackValue& index) {
                if (!table.table_pointer_origin.has_value() ||
                    !index.scaled_index_origin.has_value() ||
                    index.scaled_index_origin->scale <
                        sizeof(std::uint32_t) ||
                    index.scaled_index_origin->scale >
                        maximum_static_code_pointer_table_stride ||
                    (index.scaled_index_origin->scale & 3u) != 0u)
                    return;
                record_origin = CallbackRecordPointerOrigin{
                    table.table_pointer_origin
                        ->header_table_pointer_displacement,
                    index.scaled_index_origin->scale};
            };
        combine_record(left, right);
        if (!record_origin.has_value()) combine_record(right, left);

        std::optional<GlobalVectorOrigin> global_origin;
        const auto combine_global_record = [&](const CallbackValue& table,
                                                const CallbackValue& index) {
            if (!table.global_vector_origin.has_value() ||
                (table.global_vector_origin->kind != GlobalVectorOriginKind::PointerCell &&
                 table.global_vector_origin->kind != GlobalVectorOriginKind::CallbackWord) ||
                table.may_be_stack || index.may_be_stack ||
                !index.scaled_index_origin.has_value()) return;
            const auto stride = index.scaled_index_origin->scale;
            if (stride < 4u || stride > maximum_static_code_pointer_table_stride ||
                (stride & 3u) != 0u) return;
            global_origin = GlobalVectorOrigin{GlobalVectorOriginKind::IndexedRecord,
                table.global_vector_origin->address, stride};
            if (table.global_vector_origin->kind == GlobalVectorOriginKind::CallbackWord) {
                global_origin->kind = GlobalVectorOriginKind::IndexedHeaderRecord;
                global_origin->header_displacement = table.global_vector_origin->displacement;
            }
        };
        combine_global_record(left, right);
        if (!global_origin.has_value()) combine_global_record(right, left);
        if (!global_origin.has_value()) global_origin = resident_table_origin(left, right);
        if (!global_origin.has_value()) global_origin = resident_table_origin(right, left);

        set_unknown(state.registers[destination]);
        state.registers[destination].scaled_index_origin = index_origin;
        state.registers[destination].record_pointer_origin = record_origin;
        state.registers[destination].record_table_slot_origin =
            record_table_slot_origin;
        state.registers[destination].global_vector_origin = global_origin;
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
        state.registers[0u].source_register_constant = true;
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
            state.registers[destination].source_register_constant = true;
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
                    line.address, width, native_entry_shapes,
                    !immediate_field_displacement(state.registers[0u]));
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
        // A proven stack spill already supplies the value. An absent static
        // image load is not an alternative path and must not erase its exact
        // argument/scalar provenance at a synthetic join.
        if (width == 4u && !has_exact_stack_value(state, base, 0, width)) {
            const auto loaded = load_static_image_values(
                image, base, 0, native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, loaded, line.address, native_entry_shapes);
            static_cast<void>(join_value(loaded_value, loaded,
                                         native_entry_shapes));
            attach_callback_field_origin(loaded_value, base, 0u, width,
                                         line.address);
            attach_record_load_origin(loaded_value, base, 0, width,
                                      line.address);
        }
        loaded_value.scaled_index_origin =
            CallbackScaledIndexOrigin{line.address, 1u};
        state.registers[destination] = std::move(loaded_value);
        return;
    }
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement: {
        const auto base = state.registers[source];
        auto loaded_value =
            load_value(state, base, instruction.displacement, width);
        if (width == 4u && !has_exact_stack_value(
                state, base, instruction.displacement, width)) {
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
            attach_record_load_origin(loaded_value, base,
                                      instruction.displacement, width,
                                      line.address);
        }
        loaded_value.scaled_index_origin =
            CallbackScaledIndexOrigin{line.address, 1u};
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
        if (index.constants_complete && index.constants.size() == 1u &&
            !index.constants_truncated)
            loaded_value = load_value(
                state, base,
                static_cast<std::int32_t>(
                    *index.constants.begin()),
                width);
        else
            set_unknown(loaded_value);
        const bool exact_stack_load =
            index.constants_complete && index.constants.size() == 1u &&
            !index.constants_truncated && has_exact_stack_value(
                state, base, static_cast<std::int32_t>(
                    *index.constants.begin()), width);
        if (width == 4u && !exact_stack_load) {
            const auto loaded = load_static_indexed_value(
                image, index, base, native_entry_shapes);
            static_cast<void>(join_value(loaded_value, loaded,
                                         native_entry_shapes));
            auto record_table_origin =
                exact_record_table_slot_origin(index, base);
            if (!record_table_origin.has_value())
                record_table_origin =
                    exact_record_table_slot_origin(base, index);
            if (record_table_origin.has_value())
                loaded_value.record_table_member_origin =
                    record_table_origin;
            auto resident = resident_table_origin(base, index);
            if (!resident.has_value()) resident = resident_table_origin(index, base);
            if (resident.has_value()) {
                resident->kind = GlobalVectorOriginKind::ResidentCallback;
                resident->load_instruction_address = line.address;
                loaded_value.global_vector_origin = resident;
            }
            if (index.constants_complete && index.constants.size() == 1u &&
                !index.constants_truncated) {
                const auto displacement = static_cast<std::int32_t>(
                    *index.constants.begin());
                attach_callback_field_origin(
                    loaded_value, base, displacement, width, line.address);
                attach_record_load_origin(
                    loaded_value, base, displacement, width, line.address);
            }
        }
        loaded_value.scaled_index_origin =
            CallbackScaledIndexOrigin{line.address, 1u};
        state.registers[destination] = std::move(loaded_value);
        return;
    }
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement: {
        const auto base = state.registers[source];
        auto loaded = load_value(state, base, 0, width);
        if (width == 4u && !has_exact_stack_value(state, base, 0, width)) {
            const auto static_loaded = load_static_image_values(
                image, base, 0, native_entry_shapes);
            observe_loaded_static_descriptor_table(
                model, image, static_loaded, line.address,
                native_entry_shapes);
            static_cast<void>(join_value(loaded, static_loaded,
                                         native_entry_shapes));
            attach_callback_field_origin(loaded, base, 0u, width,
                                         line.address);
            attach_record_load_origin(loaded, base, 0, width,
                                      line.address);
        }
        loaded.scaled_index_origin =
            CallbackScaledIndexOrigin{line.address, 1u};
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
    const CallbackValue& branch_before_delay) {
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
        if (resolved.has_value()) result.push_back(*resolved);
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
    // A callee may write through a passed spill address. Without a proven
    // memory-effect contract, only ABI-preserved register copies survive.
    for (auto& [slot, value] : state.stack_values) {
        static_cast<void>(slot);
        value.copied_field_identity.reset();
    }
}

[[nodiscard]] CallbackFunctionModel build_function_model(
    const katana::io::ExecutableImage& image,
    const FunctionInfo& function,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
    GuardedNativeEntryShapeCache& native_entry_shapes,
    std::size_t* const limited_evaluations,
    std::map<std::uint32_t, std::optional<StaticSourceConstantCall>>* source_calls = nullptr) {
    CallbackFunctionModel model;
    model.entry = function.entry_address;
    const auto entry = blocks.find(function.entry_address);
    if (entry == blocks.end()) return model;
    const auto structural_field_sinks =
        discover_structural_callback_field_sinks(function, blocks);

    CallbackState initial;
    for (std::uint8_t index = 0u; index < 4u; ++index) {
        initial.registers[4u + index].input_mask =
            static_cast<std::uint8_t>(1u << index);
        initial.registers[4u + index].direct_input_mask =
            static_cast<std::uint8_t>(1u << index);
        initial.registers[4u + index].record_family_input_mask =
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
    bool return_summary_supported = true;
    struct ReturnDomain final {
        std::set<std::uint32_t> constants;
        bool complete = false;
    };
    std::map<std::uint32_t, ReturnDomain> returns;

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        if (++evaluations > evaluation_budget) {
            ++*limited_evaluations;
            return_summary_supported = false;
            break;
        }
        const auto block = blocks.find(address);
        const auto state_it = ingress.find(address);
        if (block == blocks.end() || state_it == ingress.end()) continue;
        CallbackState state = state_it->second;
        const auto& lines = block->second->lines;

        const katana::sh4::DisassemblyLine* control = nullptr;
        const katana::sh4::DisassemblyLine* return_instruction = nullptr;
        CallbackValue branch_before_delay;
        std::vector<std::uint32_t> targets;
        for (const auto& line : lines) {
            if (source_calls != nullptr &&
                (line.instruction.kind == katana::sh4::InstructionKind::Unknown ||
                 line.instruction.control_flow == katana::sh4::ControlFlowKind::Trap ||
                 line.instruction.control_flow == katana::sh4::ControlFlowKind::ExceptionReturn ||
                 line.instruction.control_flow == katana::sh4::ControlFlowKind::Halt)) {
                ++*limited_evaluations;
                source_calls->clear();
                return model;
            }
            if (line.instruction.kind == katana::sh4::InstructionKind::Rts)
                return_instruction = &line;
            const auto flow = line.instruction.control_flow;
            const bool call = flow == katana::sh4::ControlFlowKind::Call ||
                              flow == katana::sh4::ControlFlowKind::IndirectCall;
            const bool tail =
                flow == katana::sh4::ControlFlowKind::UnconditionalBranch ||
                flow == katana::sh4::ControlFlowKind::IndirectBranch;
            if (call || tail) {
                if (call || flow == katana::sh4::ControlFlowKind::IndirectBranch)
                    return_summary_supported = false;
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
                targets = call_targets(image, line, branch_before_delay);
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
                auto origin = branch.global_vector_origin;
                if (origin.has_value() && origin->kind != GlobalVectorOriginKind::CallbackWord)
                    origin.reset();
                const auto [global_sink, inserted] = model.global_vector_sinks.emplace(
                    control->address, origin);
                if (!inserted && global_sink->second != origin) global_sink->second.reset();
                model.local_sink_mask = static_cast<std::uint8_t>(
                    model.local_sink_mask |
                    branch.direct_input_mask);
                if (branch.field_origins_truncated)
                    model.field_sinks_truncated = true;
                for (const auto& field : branch.field_origins) {
                    std::uint8_t exact_receiver_argument_mask = 0u;
                    if (field.receiver_record_table_origin.has_value()) {
                        for (std::uint8_t argument = 0u; argument < 4u;
                             ++argument) {
                            const auto& value =
                                state.registers[4u + argument];
                            if (value.record_table_member_origin ==
                                field.receiver_record_table_origin) {
                                exact_receiver_argument_mask =
                                    static_cast<std::uint8_t>(
                                        exact_receiver_argument_mask |
                                        static_cast<std::uint8_t>(
                                            1u << argument));
                            }
                        }
                    }
                    const auto structural = std::find_if(
                        structural_field_sinks.begin(),
                        structural_field_sinks.end(),
                        [&](const auto& candidate) {
                            return candidate.call_instruction_address ==
                                       control->address &&
                                   candidate.load_instruction_address ==
                                       field.load_instruction_address &&
                                   candidate.displacement ==
                                       field.displacement &&
                                   candidate.width == field.width &&
                                   candidate.call ==
                                       (control->instruction.control_flow ==
                                        katana::sh4::ControlFlowKind::
                                            IndirectCall);
                        });
                    CallbackFieldSink sink{
                        model.entry,
                        control->address,
                        field,
                        control->instruction.control_flow ==
                            katana::sh4::ControlFlowKind::IndirectCall,
                        static_cast<std::uint8_t>(
                            exact_receiver_argument_mask |
                            (structural == structural_field_sinks.end()
                                 ? 0u
                                 : structural->receiver_argument_mask))};
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
                        existing->receiver_argument_mask =
                            static_cast<std::uint8_t>(
                                existing->receiver_argument_mask |
                                sink.receiver_argument_mask);
                    } else {
                        model.field_sinks.push_back(std::move(sink));
                    }
                }
            }
        }

        if (source_calls != nullptr && control != nullptr &&
            (control->instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
             control->instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall)) {
            std::optional<StaticSourceConstantCall> observation;
            // Validate this block's delay context, not the global physical
            // address: an instruction may also have a normal-entry context.
            const bool valid_slot = lines.size() >= 2u &&
                &lines[lines.size() - 2u] == control &&
                control->instruction.has_delay_slot && !control->is_delay_slot &&
                lines.back().address == control->address + 2u &&
                lines.back().is_delay_slot &&
                lines.back().instruction.kind != katana::sh4::InstructionKind::Unknown &&
                lines.back().instruction.control_flow == katana::sh4::ControlFlowKind::None;
            const bool exact_target = control->instruction.kind == katana::sh4::InstructionKind::Bsr ||
                (control->instruction.kind == katana::sh4::InstructionKind::Jsr &&
                 branch_before_delay.source_register_constant &&
                 branch_before_delay.constants_complete &&
                 !branch_before_delay.constants_truncated && branch_before_delay.constants.size() == 1u);
            if (valid_slot && exact_target && targets.size() == 1u) {
                observation = StaticSourceConstantCall{model.entry, control->address, targets.front(), {}};
                for (std::size_t i = 0u; i < 4u; ++i) {
                    const auto& argument = state.registers[4u + i];
                    if (argument.source_register_constant && argument.constants_complete &&
                        !argument.constants_truncated && argument.constants.size() == 1u)
                        observation->arguments[i] = *argument.constants.begin();
                }
            }
            const auto [found, inserted] = source_calls->emplace(control->address, observation);
            if (!inserted) {
                if (!found->second || !observation ||
                    found->second->callee_address != observation->callee_address) {
                    found->second.reset();
                } else {
                    for (std::size_t i = 0u; i < 4u; ++i)
                        if (found->second->arguments[i] != observation->arguments[i])
                            found->second->arguments[i].reset();
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
                 katana::sh4::ControlFlowKind::IndirectCall)) {
            clobber_call_volatile_registers(state);
            if (targets.size() == 1u &&
                (control->instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                 (branch_before_delay.code_constants_complete &&
                  !branch_before_delay.code_constants_truncated &&
                  branch_before_delay.code_constants.size() == 1u)))
                state.registers[0u].global_vector_origin = GlobalVectorOrigin{
                    GlobalVectorOriginKind::CallResult, targets.front()};
        }

        if (return_instruction != nullptr) {
            const auto& value = state.registers[0u];
            returns[return_instruction->address] = ReturnDomain{
                value.constants, value.constants_complete && !value.constants_truncated &&
                                 !value.constants.empty()};
        } else {
            if (block->second->successors.empty()) return_summary_supported = false;
            for (const auto successor : block->second->successors)
                if (!owned.contains(successor)) return_summary_supported = false;
        }

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
    if (return_summary_supported && !returns.empty() && pending.empty()) {
        model.finite_return_complete = true;
        for (const auto& [site, domain] : returns) {
            static_cast<void>(site);
            model.finite_return_complete = model.finite_return_complete && domain.complete;
            model.finite_return_constants.insert(domain.constants.begin(), domain.constants.end());
            if (model.finite_return_constants.size() > maximum_scalar_constants) {
                model.finite_return_complete = false;
                break;
            }
        }
        if (!model.finite_return_complete) model.finite_return_constants.clear();
    }
    return model;
}

[[nodiscard]] std::optional<std::uint32_t> global_vector_ram_address(
    const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    const auto raw = static_cast<std::uint32_t>(value);
    const auto region = raw & 0xE0000000u;
    const auto physical = raw & 0x1FFFFFFFu;
    if ((region != 0u && region != 0x80000000u && region != 0xA0000000u) ||
        physical < 0x0C000000u || physical >= 0x10000000u || (physical & 3u) != 0u)
        return std::nullopt;
    return physical | 0x80000000u;
}

template <typename Models>
std::vector<StaticCallbackRecordTableContract> global_vector_table_contracts(
    const katana::io::ExecutableImage& image, const Models& models) {
    // Join only exact producer/consumer cells in this source-image generation.
    // The executable producer/getter literals still need immutable evidence.
    // Record bytes may also come from a committed, named file source: their
    // current values only seed guarded AOT Candidates, never scalar constants,
    // complete target sets, or a global-memory fixed point. In particular, a
    // bootstrap snapshot does not make the surrounding RAM immutable.
    std::map<std::uint32_t, std::set<std::uint32_t>> table_bases;
    std::map<std::uint32_t, std::vector<GlobalVectorOrigin>> published_vectors;
    for (const auto& [entry, state] : models) {
        static_cast<void>(entry);
        if (state.model->persistent_stores_truncated) continue;
        for (const auto& store : state.model->persistent_stores) {
            if (!store.source.global_vector_origin.has_value() ||
                !store.receiver.pc_literal_identity.has_value() ||
                store.receiver.may_be_stack || store.indexed_addressing ||
                store.width != 4u || store.displacement < 0) continue;
            const auto cell = global_vector_ram_address(
                static_cast<std::uint64_t>(store.receiver.pc_literal_identity->value) +
                static_cast<std::uint32_t>(store.displacement));
            if (!cell.has_value()) continue;
            const auto& origin = *store.source.global_vector_origin;
            if (origin.kind == GlobalVectorOriginKind::CallResult) {
                const auto getter = models.find(origin.address);
                if (getter == models.end() || !getter->second.model->finite_return_complete)
                    continue;
                auto& bases = table_bases[*cell];
                for (const auto raw : getter->second.model->finite_return_constants)
                    if (const auto base = global_vector_ram_address(raw); base.has_value())
                        bases.insert(*base);
            } else if (origin.kind == GlobalVectorOriginKind::RecordVector) {
                auto& origins = published_vectors[*cell];
                if (std::find(origins.begin(), origins.end(), origin) == origins.end())
                    origins.push_back(origin);
            }
        }
    }
    std::vector<StaticCallbackRecordTableContract> result;
    std::size_t bounded_scans = 0u;
    for (const auto& [entry, state] : models) {
        for (const auto& [call_site, sink] : state.model->global_vector_sinks) {
            if (!sink.has_value()) continue;
            const auto carrier = published_vectors.find(sink->address);
            if (carrier == published_vectors.end()) continue;
            for (const auto& descriptor : carrier->second) {
                const auto tables = table_bases.find(descriptor.address);
                if (tables == table_bases.end() || descriptor.stride < 4u ||
                    descriptor.stride > maximum_static_code_pointer_table_stride ||
                    descriptor.displacement < 0 ||
                    static_cast<std::uint32_t>(descriptor.displacement) + 4u > descriptor.stride)
                    continue;
                for (auto base = tables->second.begin(); base != tables->second.end(); ++base) {
                    const auto resolved = image.resolve_segment_address(*base, descriptor.stride);
                    if (!resolved.has_value()) continue;
                    const auto* segment = image.find_segment(*resolved, descriptor.stride);
                    const auto* bound = image.find_immutable_range(*resolved, descriptor.stride);
                    if (segment == nullptr || !segment->permissions.readable)
                        continue;
                    const bool immutable = bound != nullptr && !bound->identity.empty() &&
                        bound->generation == image.immutable_generation();
                    const bool committed_file_source = !segment->local_source_name.empty() &&
                        (segment->source_kind == katana::io::ImageSourceKind::RawBinary ||
                         segment->source_kind == katana::io::ImageSourceKind::ElfLoadSegment ||
                         segment->source_kind == katana::io::ImageSourceKind::DiscBootFile ||
                         segment->source_kind == katana::io::ImageSourceKind::DiscModule);
                    if (!immutable && !committed_file_source)
                        continue;
                    const auto next_base = std::next(base);
                    const auto end = next_base == tables->second.end()
                        ? std::numeric_limits<std::uint64_t>::max()
                        : static_cast<std::uint64_t>(*next_base);
                    bool terminated = false;
                    for (std::size_t index = 0u; index < maximum_static_code_pointer_table_entries; ++index) {
                        const auto address = static_cast<std::uint64_t>(*base) + index * descriptor.stride;
                        if (address + descriptor.stride > end ||
                            address + descriptor.stride > std::numeric_limits<std::uint32_t>::max()) break;
                        const auto record = image.resolve_segment_address(
                            static_cast<std::uint32_t>(address), descriptor.stride);
                        if (!record.has_value() ||
                            image.find_segment(*record, descriptor.stride) != segment ||
                            (immutable && image.find_immutable_range(*record, descriptor.stride) != bound)) break;
                        const auto offset = segment->byte_offset(*record);
                        if (!offset.has_value() || *offset > segment->bytes.size() ||
                            descriptor.stride > segment->bytes.size() - *offset) break;
                        const auto raw = read_image_u32(image,
                            *record + static_cast<std::uint32_t>(descriptor.displacement));
                        const auto vector = raw.has_value() ? global_vector_ram_address(*raw) : std::nullopt;
                        if (!vector.has_value()) { terminated = true; break; }
                        result.push_back({entry, call_site, sink->load_instruction_address,
                            entry, 0, static_cast<std::uint32_t>(sink->displacement) + 4u,
                            sink->displacement, 0u, 4u,
                            CallbackRecordTableSource::StaticVectorAddress, 0u, *vector});
                        if (result.size() >= maximum_inventory_candidates)
                            throw std::runtime_error("Global callback-vector inventory exceeds its bounded budget");
                    }
                    if (!terminated) ++bounded_scans;
                }
            }
        }
    }
    if (bounded_scans != 0u)
        std::clog << "KATANA_STATIC_VECTOR_SCAN bounded_scans=" << bounded_scans
                  << " table_completeness=unknown candidates=" << result.size() << '\n';
    return result;
}

void append_resident_callback_cells(
    const katana::io::ExecutableImage& image, const std::uint32_t function,
    const CallbackCall& call, const std::uint8_t argument,
    std::vector<StaticCallbackRecordTableContract>& result) {
    const auto& value = call.arguments[argument];
    if (!value.global_vector_origin.has_value()) return;
    const auto& origin = *value.global_vector_origin;
    if (origin.kind != GlobalVectorOriginKind::ResidentCallback ||
        origin.resident_index_count != 1u) return;
    const auto table = global_vector_ram_address(origin.address);
    if (!table.has_value() || origin.stride < 4u || origin.stride > 256u ||
        (origin.stride & 3u) != 0u) return;
    const auto resolved = image.resolve_segment_address(*table, 4u);
    if (!resolved.has_value()) return;
    const auto* segment = image.find_segment(*resolved, 4u);
    const auto* bound = image.find_immutable_range(*resolved, 4u);
    if (segment == nullptr || !segment->permissions.readable) return;
    const bool immutable = bound != nullptr && !bound->identity.empty() &&
        bound->generation == image.immutable_generation();
    const bool committed_source = !segment->local_source_name.empty() &&
        (segment->source_kind == katana::io::ImageSourceKind::RawBinary ||
         segment->source_kind == katana::io::ImageSourceKind::ElfLoadSegment ||
         segment->source_kind == katana::io::ImageSourceKind::DiscBootFile ||
         segment->source_kind == katana::io::ImageSourceKind::DiscModule);
    if (!immutable && !committed_source) return;
    // The runtime selector is not locally bounded. Keep only positive cells,
    // including entries after null holes; neither the terminator nor this cap
    // proves an exhaustive table. Foreign values are not local code constants.
    for (std::size_t index = 0u; index < maximum_static_code_pointer_table_entries; ++index) {
        const auto cell64 = static_cast<std::uint64_t>(*table) + index * origin.stride;
        if (cell64 > 0x8ffffffcu) break;
        const auto cell = static_cast<std::uint32_t>(cell64);
        const auto source = image.resolve_segment_address(cell, 4u);
        if (!source.has_value() || image.find_segment(*source, 4u) != segment ||
            (immutable && image.find_immutable_range(*source, 4u) != bound)) break;
        const auto raw = read_image_u32(image, *source);
        if (!raw.has_value()) break;
        if (*raw == 0u) continue;
        const auto region = *raw & 0xe0000000u;
        const auto physical = *raw & 0x1fffffffu;
        if ((region != 0u && region != 0x80000000u && region != 0xa0000000u) ||
            physical < 0x0c000000u || physical >= 0x10000000u || (physical & 1u) != 0u) break;
        if (result.size() >= maximum_inventory_candidates)
            throw std::runtime_error("Resident callback-cell inventory exceeds its bounded budget");
        result.push_back({function, call.instruction_address, origin.load_instruction_address,
            call.callee, 0, origin.stride, 0, argument, 4u,
            CallbackRecordTableSource::ResidentCallbackCell, 0u, *table,
            cell, physical | 0x80000000u});
    }
}

using PublishedHeaders = std::map<std::uint32_t,
    std::set<std::pair<std::uint32_t, std::uint32_t>>>;

template <typename Models>
PublishedHeaders published_resident_headers(
    const katana::io::ExecutableImage& image, const Models& models) {
    PublishedHeaders result;
    std::size_t candidates = 0u;
    for (const auto& [entry, state] : models) {
        static_cast<void>(entry);
        if (state.model->persistent_stores_truncated) continue;
        for (const auto& store : state.model->persistent_stores) {
            if (!store.source.global_vector_origin.has_value() ||
                !store.receiver.pc_literal_identity.has_value() ||
                store.receiver.may_be_stack || store.indexed_addressing ||
                store.width != 4u || store.displacement < 0) continue;
            const auto& origin = *store.source.global_vector_origin;
            if (origin.kind != GlobalVectorOriginKind::ResidentCallback ||
                origin.resident_index_count == 0u || origin.resident_index_count > 2u) continue;
            const auto published = global_vector_ram_address(
                static_cast<std::uint64_t>(store.receiver.pc_literal_identity->value) +
                static_cast<std::uint32_t>(store.displacement));
            const auto table = global_vector_ram_address(origin.address);
            if (!published.has_value() || !table.has_value()) continue;
            const auto base = image.resolve_segment_address(*table, 4u);
            if (!base.has_value()) continue;
            const auto* segment = image.find_segment(*base, 4u);
            const auto* bound = image.find_immutable_range(*base, 4u);
            if (segment == nullptr || !segment->permissions.readable) continue;
            const bool immutable = bound != nullptr && !bound->identity.empty() &&
                bound->generation == image.immutable_generation();
            const bool committed = !segment->local_source_name.empty() &&
                (segment->source_kind == katana::io::ImageSourceKind::RawBinary ||
                 segment->source_kind == katana::io::ImageSourceKind::ElfLoadSegment ||
                 segment->source_kind == katana::io::ImageSourceKind::DiscBootFile ||
                 segment->source_kind == katana::io::ImageSourceKind::DiscModule);
            if (!immutable && !committed) continue;
            const auto first = origin.resident_indices[0u].scale;
            const auto second = origin.resident_index_count == 2u
                ? origin.resident_indices[1u].scale : first;
            const auto row_stride = std::max(first, second);
            const auto slot_stride = std::min(first, second);
            if (slot_stride < 4u || row_stride > maximum_static_code_pointer_table_stride ||
                (slot_stride & 3u) != 0u || row_stride % slot_stride != 0u) continue;
            // Enumerate bounded nonnegative representatives of both selectors.
            // Signed/unknown domains remain unknown: no count, reachability or
            // completeness follows from these source-bound positive cells.
            bool stopped = false;
            for (std::size_t row = 0u;
                 row < maximum_static_code_pointer_table_entries && !stopped; ++row) {
                for (std::uint32_t slot = 0u; slot < row_stride / slot_stride; ++slot) {
                    const auto cell64 = static_cast<std::uint64_t>(*table) +
                        row * row_stride + slot * slot_stride;
                    if (cell64 > 0x8ffffffcu) { stopped = true; break; }
                    const auto cell = static_cast<std::uint32_t>(cell64);
                    const auto source = image.resolve_segment_address(cell, 4u);
                    if (!source.has_value() || image.find_segment(*source, 4u) != segment ||
                        (immutable && image.find_immutable_range(*source, 4u) != bound)) {
                        stopped = true; break;
                    }
                    const auto offset = segment->byte_offset(*source);
                    if (!offset.has_value() || *offset > segment->bytes.size() ||
                        4u > segment->bytes.size() - *offset) { stopped = true; break; }
                    const auto raw = read_image_u32(image, *source);
                    if (!raw.has_value()) { stopped = true; break; }
                    if (*raw == 0u) continue;
                    const auto header = global_vector_ram_address(*raw);
                    if (!header.has_value()) { stopped = true; break; }
                    if (result[*published].emplace(cell, *header).second &&
                        ++candidates > maximum_inventory_candidates)
                        throw std::runtime_error("Published header inventory exceeds its bounded budget");
                }
            }
        }
    }
    return result;
}

void append_published_header_records(
    const PublishedHeaders& headers, const std::uint32_t function,
    const CallbackCall& call, const std::uint8_t argument,
    std::vector<StaticCallbackRecordTableContract>& result) {
    const auto& value = call.arguments[argument];
    if (!value.global_vector_origin.has_value()) return;
    const auto& origin = *value.global_vector_origin;
    if (origin.kind != GlobalVectorOriginKind::HeaderCallback) return;
    const auto found = headers.find(origin.address);
    if (found == headers.end()) return;
    for (const auto& [cell, header] : found->second) {
        if (result.size() >= maximum_inventory_candidates)
            throw std::runtime_error("Published callback-record inventory exceeds its bounded budget");
        result.push_back({function, call.instruction_address, origin.load_instruction_address,
            call.callee, origin.header_displacement, origin.stride, origin.displacement,
            argument, 4u, CallbackRecordTableSource::PublishedHeaderRecords, 0u,
            origin.address, cell, header});
    }
}

[[nodiscard]] std::optional<StaticCallbackRecordTableContract>
direct_sentinel_record_table_contract(
    const FunctionInfo& function,
    const CallbackCall& call,
    const std::uint8_t callback_argument,
    const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks) {
    using K = katana::sh4::InstructionKind;
    using C = katana::sh4::ControlFlowKind;
    if (callback_argument >= 4u || function.block_addresses.size() > 256u)
        return std::nullopt;
    std::map<std::uint32_t, const katana::sh4::DisassemblyLine*> lines;
    const BasicBlock* call_block = nullptr;
    for (const auto address : function.block_addresses) {
        const auto found = blocks.find(address);
        if (found == blocks.end()) return std::nullopt;
        for (const auto& line : found->second->lines) {
            lines.emplace(line.address, &line);
            if (line.address == call.instruction_address) call_block = found->second;
        }
    }
    if (call_block == nullptr || lines.size() > 4096u) return std::nullopt;
    const auto at = [&](const std::uint32_t pc) {
        const auto found = lines.find(pc);
        return found == lines.end() ? nullptr : found->second;
    };
    struct Field {
        std::uint32_t load;
        std::uint8_t base;
        std::int32_t displacement;
    };
    // Resolve the outgoing argument after the current call's delay slot,
    // while earlier calls clobber volatile values after their own slots.
    std::array<std::optional<Field>, 16> fields{};
    bool clobber_after_slot = false;
    bool reached_call = false;
    for (const auto& line : call_block->lines) {
        const auto& instruction = line.instruction;
        std::optional<Field> value;
        if (instruction.kind == K::MovRegister)
            value = fields[instruction.source_register];
        else if (instruction.kind == K::MovLongLoad ||
                 instruction.kind == K::MovLongLoadDisplacement)
            value = Field{line.address, instruction.source_register,
                          instruction.kind == K::MovLongLoad ? 0 : instruction.displacement};
        const auto writes = general_register_write_mask(instruction);
        for (unsigned reg = 0u; reg < 16u; ++reg)
            if ((writes & (1u << reg)) != 0u) fields[reg].reset();
        if (value.has_value()) fields[instruction.destination_register] = value;
        if (line.is_delay_slot && clobber_after_slot) {
            for (unsigned reg = 0u; reg < 8u; ++reg) fields[reg].reset();
            clobber_after_slot = false;
        }
        if (line.address == call.instruction_address) reached_call = true;
        else if (reached_call && line.address == call.instruction_address + 2u && line.is_delay_slot)
            break;
        else if (instruction.control_flow == C::Call || instruction.control_flow == C::IndirectCall)
            clobber_after_slot = true;
    }
    const auto field = fields[4u + callback_argument];
    if (!reached_call || !field.has_value() || field->base < 8u || field->base >= 15u ||
        field->displacement < 0 || (field->displacement & 3) != 0)
        return std::nullopt;
    const auto is_field_load = [&](const katana::sh4::DisassemblyLine* line) {
        return line != nullptr && line->instruction.source_register == field->base &&
            ((line->instruction.kind == K::MovLongLoad && field->displacement == 0) ||
             (line->instruction.kind == K::MovLongLoadDisplacement &&
              line->instruction.displacement == field->displacement));
    };
    const auto is_zero_test = [&](const katana::sh4::DisassemblyLine* load,
                                  const katana::sh4::DisassemblyLine* test) {
        return is_field_load(load) && test != nullptr && test->instruction.kind == K::TestRegister &&
            test->instruction.source_register == load->instruction.destination_register &&
            test->instruction.destination_register == load->instruction.destination_register;
    };
    for (const auto& [pc, latch] : lines) {
        if (latch->instruction.kind != K::Bf || !latch->target_address.has_value() || pc < 6u)
            continue;
        const auto begin = *latch->target_address;
        if (begin < 6u || begin >= call.instruction_address || call.instruction_address >= pc - 6u)
            continue;
        const auto step = at(pc - 6u);
        const auto guard = at(begin - 2u);
        if (step == nullptr || step->instruction.kind != K::AddImmediate ||
            step->instruction.destination_register != field->base ||
            step->instruction.immediate < 4 || step->instruction.immediate > 256 ||
            (step->instruction.immediate & 3) != 0 ||
            field->displacement + 4 > step->instruction.immediate ||
            !is_zero_test(at(pc - 4u), at(pc - 2u)) ||
            !is_zero_test(at(begin - 6u), at(begin - 4u)) ||
            guard == nullptr || guard->instruction.kind != K::Bt ||
            guard->target_address != std::optional<std::uint32_t>{pc + 2u})
            continue;
        bool valid = true;
        // Both the initial and latch null guards precede a reload for the
        // registrar argument. Prove that this short prefix cannot modify
        // memory or call out; otherwise equal addresses do not establish
        // equal guarded values. Unknown instruction effects remain rejected.
        for (std::uint64_t cursor = begin; cursor < field->load; cursor += 2u) {
            const auto line = at(static_cast<std::uint32_t>(cursor));
            if (line == nullptr) { valid = false; break; }
            switch (line->instruction.kind) {
            case K::Nop: case K::MovRegister: case K::MovImmediate:
            case K::MovLongLoadPcRelative: case K::MovWordLoadPcRelative:
            case K::MovByteLoad: case K::MovWordLoad: case K::MovLongLoad:
            case K::MovByteLoadDisplacement: case K::MovWordLoadDisplacement:
            case K::MovLongLoadDisplacement:
                break;
            default: valid = false; break;
            }
            if (!valid) break;
        }
        if (!valid || field->load < begin || field->load >= call.instruction_address)
            continue;
        // The loop has one induction update, no other receiver definition,
        // no alternative entry and no exit bypassing the checked sentinel.
        for (std::uint64_t cursor = begin; cursor <= pc; cursor += 2u) {
            const auto line = at(static_cast<std::uint32_t>(cursor));
            if (line == nullptr ||
                ((general_register_write_mask(line->instruction) & (1u << field->base)) != 0u &&
                 line != step)) { valid = false; break; }
        }
        std::uint32_t guard_block = 0u;
        for (const auto address : function.block_addresses) {
            const auto& block = *blocks.at(address);
            if (block.start_address <= guard->address && block.end_address >= guard->address)
                guard_block = address;
            const bool inside = address >= begin && address <= pc;
            if (inside && block.successors.empty()) valid = false;
            for (const auto successor : block.successors) {
                if (inside && (successor < begin || successor > pc) &&
                    !(block.end_address == pc && successor == pc + 2u)) valid = false;
                if (!inside && successor >= begin && successor <= pc &&
                    !(successor == begin && block.end_address == begin - 2u)) valid = false;
            }
        }
        if (!valid || guard_block == 0u) continue;

        // Must-alias analysis of the prefix: intersection at every join.
        // This proves which incoming argument reaches the induction register,
        // without treating broad arithmetic/data influence as pointer identity.
        using Aliases = std::array<std::uint8_t, 16>;
        Aliases initial{};
        for (unsigned argument = 0u; argument < 4u; ++argument)
            initial[4u + argument] = static_cast<std::uint8_t>(1u << argument);
        std::map<std::uint32_t, Aliases> inputs{{function.entry_address, initial}};
        std::deque<std::uint32_t> pending{function.entry_address};
        std::size_t work = 0u;
        while (!pending.empty() && work++ < 16384u) {
            const auto address = pending.front(); pending.pop_front();
            if (address >= begin) continue;
            const auto found = blocks.find(address);
            if (found == blocks.end()) { valid = false; break; }
            auto state = inputs.at(address);
            bool clobber = false;
            for (const auto& line : found->second->lines) {
                const auto& instruction = line.instruction;
                const auto moved = instruction.kind == K::MovRegister
                    ? state[instruction.source_register] : std::uint8_t{0u};
                const auto writes = general_register_write_mask(instruction);
                for (unsigned reg = 0u; reg < 16u; ++reg)
                    if ((writes & (1u << reg)) != 0u) state[reg] = 0u;
                if (instruction.kind == K::MovRegister) state[instruction.destination_register] = moved;
                if (line.is_delay_slot && clobber) {
                    for (unsigned reg = 0u; reg < 8u; ++reg) state[reg] = 0u;
                    clobber = false;
                }
                if (instruction.control_flow == C::Call || instruction.control_flow == C::IndirectCall)
                    clobber = true;
            }
            for (const auto successor : found->second->successors) {
                if (successor > begin) continue;
                const auto [next, inserted] = inputs.emplace(successor, state);
                bool changed = inserted;
                if (!inserted) for (unsigned reg = 0u; reg < 16u; ++reg) {
                    const auto joined = static_cast<std::uint8_t>(next->second[reg] & state[reg]);
                    changed |= joined != next->second[reg]; next->second[reg] = joined;
                }
                if (changed) pending.push_back(successor);
            }
        }
        if (!valid || !pending.empty() || !inputs.contains(begin)) continue;
        const auto mask = inputs.at(begin)[field->base];
        if (mask == 0u || (mask & (mask - 1u)) != 0u) continue;
        std::uint8_t table_argument = 0u;
        while ((mask & (1u << table_argument)) == 0u) ++table_argument;
        return StaticCallbackRecordTableContract{
            function.entry_address, call.instruction_address, field->load, call.callee,
            0, static_cast<std::uint32_t>(step->instruction.immediate), field->displacement,
            callback_argument, 4u, CallbackRecordTableSource::DirectSentinelArgument, table_argument};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<StaticCallbackRecordTableContract>
callback_record_table_contract(
    const FunctionInfo& function,
    const CallbackCall& call,
    const std::uint8_t callback_argument) {
    if (callback_argument >= 4u) return std::nullopt;
    const auto& argument = call.arguments[callback_argument];
    if (!argument.record_load_origin.has_value()) return std::nullopt;
    const auto& origin = *argument.record_load_origin;
    if (origin.header_table_pointer_displacement <
            static_cast<std::int32_t>(sizeof(std::uint32_t)) ||
        (origin.header_table_pointer_displacement & 3) != 0 ||
        origin.record_stride < sizeof(std::uint32_t) ||
        origin.record_stride > maximum_static_code_pointer_table_stride ||
        (origin.record_stride & 3u) != 0u ||
        origin.record_displacement < 0 ||
        (origin.record_displacement & 3) != 0 ||
        static_cast<std::uint64_t>(origin.record_displacement) +
                sizeof(std::uint32_t) >
            origin.record_stride)
        return std::nullopt;
    return StaticCallbackRecordTableContract{
        function.entry_address,
        call.instruction_address,
        origin.load_instruction_address,
        call.callee,
        origin.header_table_pointer_displacement,
        origin.record_stride,
        origin.record_displacement,
        callback_argument,
        static_cast<std::uint8_t>(sizeof(std::uint32_t))};
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

StaticCodePointerVectorInventory discover_anchored_static_code_pointer_vectors(
    const katana::io::ExecutableImage& image,
    const katana::io::ImageSegment& active_source,
    const std::span<const std::uint32_t> anchors,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    if (anchors.empty()) return {};
    if (!std::is_sorted(anchors.begin(), anchors.end()) ||
        std::adjacent_find(anchors.begin(), anchors.end()) != anchors.end())
        throw std::invalid_argument("Code-vector anchors are not canonical");
    const std::array components{&active_source};
    return discover_static_code_pointer_vectors(
        image, components, native_entry_shapes, anchors);
}

struct StaticCallbackInventorySession::Impl final {
    struct CachedModel final {
        CallbackModelInputBinding binding;
        std::shared_ptr<const CallbackFunctionModel> model;
        std::size_t limited_evaluations = 0u;
        std::size_t retained_bytes = 0u;
    };

    bool image_bound = false;
    std::uint64_t image_identity = 0u;
    std::uint64_t immutable_generation = 0u;
    std::vector<katana::io::ImageImmutableRange> immutable_ranges;
    std::map<std::uint32_t, CachedModel> models;
    std::size_t retained_model_bytes = 0u;

    void bind(const katana::io::ExecutableImage& image) {
        const auto ranges = image.immutable_ranges();
        const bool same_image =
            image_bound &&
            image_identity == image.analysis_instance_identity() &&
            immutable_generation == image.immutable_generation() &&
            immutable_ranges.size() == ranges.size() &&
            std::equal(immutable_ranges.begin(), immutable_ranges.end(),
                       ranges.begin(), ranges.end());
        if (!same_image) {
            models.clear();
            retained_model_bytes = 0u;
        }
        image_bound = true;
        image_identity = image.analysis_instance_identity();
        immutable_generation = image.immutable_generation();
        immutable_ranges.assign(ranges.begin(), ranges.end());
    }

    void clear() noexcept {
        image_bound = false;
        image_identity = 0u;
        immutable_generation = 0u;
        immutable_ranges.clear();
        models.clear();
        retained_model_bytes = 0u;
    }
};

StaticCallbackInventorySession::StaticCallbackInventorySession()
    : impl_(std::make_unique<Impl>()) {}

StaticCallbackInventorySession::~StaticCallbackInventorySession() = default;

StaticCallbackInventorySession::StaticCallbackInventorySession(
    StaticCallbackInventorySession&&) noexcept = default;

StaticCallbackInventorySession& StaticCallbackInventorySession::operator=(
    StaticCallbackInventorySession&&) noexcept = default;

void StaticCallbackInventorySession::clear() noexcept { impl_->clear(); }

std::vector<StaticSourceConstantCall> discover_source_constant_calls(
    const katana::io::ExecutableImage& image,
    const std::span<const BasicBlock> blocks,
    const std::span<const FunctionInfo> functions,
    const std::span<const std::uint32_t> recognized_callees) {
    std::vector<StaticSourceConstantCall> result;
    if (recognized_callees.empty()) return result;
    const std::set<std::uint32_t> callees(recognized_callees.begin(), recognized_callees.end());
    std::unordered_map<std::uint32_t, const BasicBlock*> by_address;
    for (const auto& block : blocks) by_address.emplace(block.start_address, &block);
    GuardedNativeEntryShapeCache shapes(image);
    for (const auto& function : functions) {
        // Only model owners containing a direct call or a literal reference
        // to a recognized callee. No full interprocedural callback fixpoint.
        bool relevant = false;
        for (const auto address : function.block_addresses) {
            const auto block = by_address.find(address);
            if (block == by_address.end()) continue;
            for (const auto& line : block->second->lines) {
                auto target = line.target_address;
                if (line.instruction.kind == katana::sh4::InstructionKind::MovLongLoadPcRelative)
                    target = read_image_u32(image, ((line.address + 4u) & ~3u) +
                        static_cast<std::uint32_t>(line.instruction.displacement));
                if (target) {
                    const auto resolved = executable_constant(image, *target);
                    relevant = relevant || (resolved && callees.contains(*resolved));
                }
            }
        }
        if (!relevant) continue;
        std::size_t limited = 0u;
        std::map<std::uint32_t, std::optional<StaticSourceConstantCall>> calls;
        static_cast<void>(build_function_model(image, function, by_address, shapes, &limited, &calls));
        if (limited != 0u) continue;
        for (const auto& [site, call] : calls) {
            static_cast<void>(site);
            if (call && callees.contains(call->callee_address)) result.push_back(*call);
        }
    }
    return result;
}

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
        callback_field_sink_contracts,
    std::vector<StaticCallbackRecordTableContract>* const
        callback_record_table_contracts,
    StaticCallbackInventorySession* const session,
    std::vector<PersistentFieldCopyContract>* const persistent_field_copies) {
    if (callback_sink_contracts != nullptr)
        callback_sink_contracts->clear();
    if (persistent_pointer_sink_contracts != nullptr)
        persistent_pointer_sink_contracts->clear();
    if (callback_field_sink_contracts != nullptr)
        callback_field_sink_contracts->clear();
    if (callback_record_table_contracts != nullptr)
        callback_record_table_contracts->clear();
    if (persistent_field_copies != nullptr)
        persistent_field_copies->clear();
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
    std::size_t limited_evaluations = 0u;
    bool candidate_values_truncated = false;
    bool forwarding_truncated = false;
    std::size_t receiver_may_alias_stores = 0u;
    std::map<std::uint32_t, CallbackFunctionAnalysisState> models;
    std::size_t model_cache_hits = 0u;
    std::size_t model_cache_misses = 0u;
    std::size_t model_cache_budget_skips = 0u;
    std::size_t model_cache_derived = 0u;
    std::set<std::uint32_t> current_model_entries;
    if (session != nullptr) session->impl_->bind(image);
    for (const auto& function : functions) {
        current_model_entries.insert(function.entry_address);
        const auto binding = callback_model_input_binding(function,
                                                          block_index);
        if (session != nullptr && binding.has_value()) {
            const auto cached = session->impl_->models.find(
                function.entry_address);
            if (cached != session->impl_->models.end() &&
                cached->second.binding == *binding) {
                limited_evaluations +=
                    cached->second.limited_evaluations;
                models.emplace(
                    function.entry_address,
                    make_analysis_state(cached->second.model));
                ++model_cache_hits;
                continue;
            }
        }

        std::size_t model_limited_evaluations = 0u;
        auto model = build_function_model(image, function, block_index,
                                          native_entry_shapes,
                                          &model_limited_evaluations);
        limited_evaluations += model_limited_evaluations;
        auto shared_model =
            std::make_shared<CallbackFunctionModel>(std::move(model));
        if (session != nullptr && binding.has_value()) {
            auto& cache = *session->impl_;
            const auto cached = cache.models.find(function.entry_address);
            if (cached != cache.models.end()) {
                cache.retained_model_bytes -= cached->second.retained_bytes;
                cache.models.erase(cached);
            }
            const auto retained_bytes = saturated_add(
                estimate_callback_model_bytes(*shared_model),
                estimate_callback_binding_bytes(*binding));
            const bool fits_entry_budget =
                cache.models.size() < maximum_model_cache_entries;
            const bool fits_byte_budget =
                retained_bytes <= maximum_model_cache_bytes &&
                retained_bytes <=
                    maximum_model_cache_bytes - cache.retained_model_bytes;
            if (fits_entry_budget && fits_byte_budget) {
                cache.models.emplace(
                    function.entry_address,
                    StaticCallbackInventorySession::Impl::CachedModel{
                        *binding, shared_model, model_limited_evaluations,
                        retained_bytes});
                cache.retained_model_bytes += retained_bytes;
            } else {
                ++model_cache_budget_skips;
            }
        }
        models.insert_or_assign(
            function.entry_address,
            make_analysis_state(std::move(shared_model)));
        ++model_cache_misses;
    }
    if (session != nullptr) {
        for (auto cached = session->impl_->models.begin();
             cached != session->impl_->models.end();) {
            if (!current_model_entries.contains(cached->first)) {
                session->impl_->retained_model_bytes -=
                    cached->second.retained_bytes;
                cached = session->impl_->models.erase(cached);
            } else {
                ++cached;
            }
        }
    }

    // A receiver lane is allowed to become wide, but a lost receiver relation
    // makes the positive callback inventory incomplete.  Emit only the first
    // few exact loss sites so a contract failure identifies the responsible
    // owner/field instead of inviting a blind global budget increase.
    std::size_t receiver_loss_diagnostics = 0u;
    constexpr std::size_t maximum_receiver_loss_diagnostics = 12u;
    for (const auto& [entry, state] : models) {
        for (const auto& sink : state.model->field_sinks) {
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
        for (const auto& store : state.model->persistent_stores) {
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
    using CallbackRecordTableKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<CallbackFieldShape, std::vector<const CallbackFieldSink*>>
        field_sinks_by_shape;
    std::map<CallbackRecordTableKey,
             std::vector<const CallbackFieldSink*>>
        field_sinks_by_record_table;
    for (const auto& [entry, state] : models) {
        static_cast<void>(entry);
        candidate_values_truncated =
            candidate_values_truncated ||
            state.model->field_sinks_truncated ||
            state.model->persistent_stores_truncated ||
            state.model->field_copy_stores_truncated;
        for (const auto& sink : state.model->field_sinks) {
            field_sinks_by_shape[{sink.field.displacement,
                                  sink.field.width}]
                .push_back(&sink);
            if (sink.field.receiver_record_table_origin.has_value() &&
                (sink.receiver_argument_mask & 0x01u) != 0u) {
                const auto& table =
                    *sink.field.receiver_record_table_origin;
                field_sinks_by_record_table[
                    {table.table_address, table.stride}]
                    .push_back(&sink);
            }
        }
    }

    // Recover the ABI of record families independently from the identity of
    // any one heap/list instance.  A field sink proves such an ABI only when
    // the loaded field receiver is passed back to the callback in r4.  Direct
    // wrappers may preserve that record through any incoming argument, so
    // propagate the proof backwards through exact call targets and byte-
    // identical argument aliases.  This is deliberately stronger than a
    // displacement match: it binds the field receiver, callback argument and
    // complete direct-call chain before a constructor default can type an
    // alternative external callback as record-consuming.
    std::map<std::uint32_t, std::uint8_t> record_family_input_masks;
    for (const auto& [entry, state] : models) {
        auto& mask = record_family_input_masks[entry];
        for (const auto& sink : state.model->field_sinks) {
            if ((sink.receiver_argument_mask & 0x01u) == 0u) continue;
            mask = static_cast<std::uint8_t>(
                mask |
                (sink.field.receiver_record_family_input_mask & 0x0fu));
        }
    }
    {
        bool changed = true;
        std::size_t steps = 0u;
        const auto budget = std::max<std::size_t>(
            64u, models.size() * 8u);
        while (changed && steps++ < budget) {
            changed = false;
            for (const auto& [entry, state] : models) {
                auto& caller_mask = record_family_input_masks[entry];
                const auto before = caller_mask;
                for (const auto& call : state.model->calls) {
                    const auto callee = record_family_input_masks.find(
                        call.callee);
                    if (callee == record_family_input_masks.end()) continue;
                    for (std::size_t argument = 0u;
                         argument < call.arguments.size(); ++argument) {
                        if ((callee->second & static_cast<std::uint8_t>(
                                                  1u << argument)) == 0u)
                            continue;
                        caller_mask = static_cast<std::uint8_t>(
                            caller_mask |
                            call.arguments[argument].direct_input_mask);
                    }
                }
                changed = changed || caller_mask != before;
            }
        }
        if (changed) forwarding_truncated = true;
    }

    for (auto& [entry, state] : models) {
        const auto& model = *state.model;
        if (model.persistent_stores.empty()) continue;
        ++model_cache_derived;
        for (const auto& store : model.persistent_stores) {
            state.local_persistent_pointer_mask =
                static_cast<std::uint8_t>(
                    state.local_persistent_pointer_mask |
                    exact_incoming_argument_mask(store.source));
            // Dynamic R0-indexed stores use a separate caller-domain/address proof.
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
            bool locally_typed_record_callback = false;
            const bool locally_typed_callback =
                store.source.direct_input_mask != 0u &&
                std::any_of(
                    model.persistent_stores.begin(),
                    model.persistent_stores.end(),
                    [&](const auto& witness) {
                        const bool matched =
                            witness.displacement == store.displacement &&
                            witness.width == store.width &&
                            exact_executable_default_store(witness) &&
                            same_local_record_receiver(store.receiver,
                                                       witness.receiver);
                        if (!matched) return false;
                        locally_typed_record_callback = std::all_of(
                            witness.source.code_constants.begin(),
                            witness.source.code_constants.end(),
                            [&](const std::uint32_t target) {
                                const auto family =
                                    record_family_input_masks.find(target);
                                return family !=
                                           record_family_input_masks.end() &&
                                       (family->second & 0x01u) != 0u;
                            });
                        return true;
                    });
            const bool record_table_typed_callback =
                store.source.direct_input_mask != 0u &&
                std::any_of(
                    model.record_table_publications.begin(),
                    model.record_table_publications.end(),
                    [&](const auto& publication) {
                        const bool same_direct_receiver =
                            (store.receiver.direct_input_mask &
                             publication.record.direct_input_mask) != 0u;
                        if (!same_direct_receiver &&
                            !same_local_record_receiver(
                                store.receiver, publication.record))
                            return false;
                        const auto sinks =
                            field_sinks_by_record_table.find(
                                {publication.table.table_address,
                                 publication.table.stride});
                        if (sinks ==
                            field_sinks_by_record_table.end())
                            return false;
                        return std::any_of(
                            sinks->second.begin(), sinks->second.end(),
                            [&](const auto* const sink) {
                                return sink->field.displacement ==
                                           store.displacement &&
                                       sink->field.width == store.width;
                            });
                    });
            if (locally_typed_callback ||
                record_table_typed_callback) {
                observe_persistent_store(state, image, store.source,
                                         store.instruction_address,
                                         store.width, true,
                                         locally_typed_record_callback ||
                                             record_table_typed_callback);
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
                observe_persistent_store(state, image, store.source,
                                         store.instruction_address,
                                         store.width, true,
                                         (sink->receiver_argument_mask &
                                          0x01u) != 0u);
                break;
            }
            if (!matched && receiver_may_alias) {
                // Receiver Top is a may-alias state, not lost executable
                // evidence. Conservatively relate a callback-valued store to
                // a same-shaped indirect-load sink. This can retain extra
                // identity-checked guarded roots, but cannot omit a callback
                // merely because a record loop exceeded the exact receiver
                // inventory.
                observe_persistent_store(state, image, store.source,
                                         store.instruction_address,
                                         store.width, false, false);
                ++receiver_may_alias_stores;
            }
        }
    }
    if (session != nullptr) {
        std::cerr << "KATANA_STATIC_CALLBACK_MODEL_CACHE hits="
                  << model_cache_hits << " misses=" << model_cache_misses
                  << " retained=" << session->impl_->models.size()
                  << " retained_bytes="
                  << session->impl_->retained_model_bytes
                  << " budget_skips=" << model_cache_budget_skips
                  << " derived=" << model_cache_derived << '\n';
    }

    std::map<std::uint32_t, std::uint8_t> sink_masks;
    std::map<std::uint32_t, std::uint8_t> record_sink_masks;
    std::map<std::uint32_t, std::uint8_t> persistent_pointer_masks;
    std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::size_t>>>
        callers_by_callee;
    std::deque<std::uint32_t> pending;
    std::set<std::uint32_t> queued;
    for (const auto& [entry, state] : models) {
        sink_masks[entry] = state.local_sink_mask;
        record_sink_masks[entry] = state.local_record_sink_mask;
        persistent_pointer_masks[entry] =
            state.local_persistent_pointer_mask;
        if (state.local_sink_mask != 0u && queued.insert(entry).second)
            pending.push_back(entry);
        for (std::size_t index = 0u;
             index < state.model->calls.size(); ++index)
            callers_by_callee[state.model->calls[index].callee].push_back(
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
                        call_index >= model->second.model->calls.size())
                        continue;
                    std::uint8_t propagated = 0u;
                    const auto& call =
                        model->second.model->calls[call_index];
                    for (std::size_t argument = 0u; argument < 4u;
                         ++argument) {
                        if ((mask & static_cast<std::uint8_t>(
                                        1u << argument)) == 0u)
                            continue;
                        propagated = static_cast<std::uint8_t>(
                            propagated |
                            exact_incoming_argument_mask(call.arguments[argument]));
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
            if (model == models.end() ||
                call_index >= model->second.model->calls.size())
                continue;
            std::uint8_t propagated = 0u;
            const auto& call = model->second.model->calls[call_index];
            for (std::size_t argument = 0u; argument < 4u; ++argument) {
                if ((sink & static_cast<std::uint8_t>(1u << argument)) == 0u)
                    continue;
                propagated = static_cast<std::uint8_t>(
                    propagated |
                    call.arguments[argument].direct_input_mask);
            }
            auto& caller_sink = sink_masks[caller];
            const auto next = static_cast<std::uint8_t>(caller_sink |
                                                        propagated);
            if (next == caller_sink) continue;
            caller_sink = next;
            if (queued.insert(caller).second) pending.push_back(caller);
        }
    }

    const auto propagate_direct_masks =
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
                        call_index >= model->second.model->calls.size())
                        continue;
                    const auto& call =
                        model->second.model->calls[call_index];
                    std::uint8_t propagated = 0u;
                    for (std::size_t argument = 0u; argument < 4u;
                         ++argument) {
                        if ((mask & static_cast<std::uint8_t>(
                                        1u << argument)) == 0u)
                            continue;
                        propagated = static_cast<std::uint8_t>(
                            propagated |
                            call.arguments[argument].direct_input_mask);
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
    propagate_direct_masks(record_sink_masks);

    if (callback_sink_contracts != nullptr) {
        callback_sink_contracts->reserve(sink_masks.size());
        for (const auto [function_address, argument_mask] : sink_masks) {
            if (argument_mask == 0u) continue;
            const auto record = record_sink_masks.find(function_address);
            const auto record_argument_mask = static_cast<std::uint8_t>(
                record == record_sink_masks.end()
                    ? 0u
                    : record->second & argument_mask);
            callback_sink_contracts->push_back(
                {function_address, argument_mask,
                 record_argument_mask});
        }
    }

    if (callback_record_table_contracts != nullptr) {
        const auto global_vectors = global_vector_table_contracts(image, models);
        const auto published_headers = published_resident_headers(image, models);
        callback_record_table_contracts->insert(callback_record_table_contracts->end(),
                                                global_vectors.begin(), global_vectors.end());
        std::map<std::uint32_t, const FunctionInfo*> functions_by_entry;
        for (const auto& function : functions)
            functions_by_entry.emplace(function.entry_address, &function);
        for (const auto& [entry, state] : models) {
            const auto function = functions_by_entry.find(entry);
            if (function == functions_by_entry.end()) continue;
            for (const auto& call : state.model->calls) {
                const auto sink = sink_masks.find(call.callee);
                if (sink == sink_masks.end()) continue;
                for (std::uint8_t argument = 0u; argument < 4u;
                     ++argument) {
                    if ((sink->second & static_cast<std::uint8_t>(
                                            1u << argument)) == 0u)
                        continue;
                    append_resident_callback_cells(image, entry, call, argument,
                                                   *callback_record_table_contracts);
                    append_published_header_records(published_headers, entry, call, argument,
                                                    *callback_record_table_contracts);
                    auto contract = callback_record_table_contract(
                        *function->second, call, argument);
                    if (!contract.has_value())
                        contract = direct_sentinel_record_table_contract(
                            *function->second, call, argument, block_index);
                    if (contract.has_value())
                        callback_record_table_contracts->push_back(
                            *contract);
                }
            }
        }
        std::sort(
            callback_record_table_contracts->begin(),
            callback_record_table_contracts->end(),
            [](const auto& left, const auto& right) {
                return std::tie(
                           left.function_address,
                           left.call_instruction_address,
                           left.callback_load_instruction_address,
                           left.callback_sink_address,
                           left.header_table_pointer_displacement,
                           left.record_stride,
                           left.callback_displacement,
                           left.callback_argument,
                           left.width,
                           left.source_kind,
                           left.table_argument,
                           left.vector_address, left.resident_cell_address,
                           left.resident_target_address) <
                       std::tie(
                           right.function_address,
                           right.call_instruction_address,
                           right.callback_load_instruction_address,
                           right.callback_sink_address,
                           right.header_table_pointer_displacement,
                           right.record_stride,
                           right.callback_displacement,
                           right.callback_argument,
                           right.width,
                           right.source_kind,
                           right.table_argument,
                           right.vector_address, right.resident_cell_address,
                           right.resident_target_address);
            });
        callback_record_table_contracts->erase(
            std::unique(callback_record_table_contracts->begin(),
                        callback_record_table_contracts->end()),
            callback_record_table_contracts->end());
    }


    std::vector<PersistentFieldCopyContract> field_copy_contracts;
    for (const auto& [entry, state] : models) {
        if (state.model->field_copy_stores_truncated) continue;
        for (const auto& [store, origin] : state.model->field_copy_stores) {
            if (!origin) continue;
            field_copy_contracts.push_back({entry, origin->load_instruction_address,
                store, origin->displacement, origin->argument, 4u});
        }
    }
    std::sort(field_copy_contracts.begin(), field_copy_contracts.end());
    field_copy_contracts.erase(std::unique(field_copy_contracts.begin(), field_copy_contracts.end()),
                              field_copy_contracts.end());
    if (persistent_field_copies != nullptr) *persistent_field_copies = field_copy_contracts;

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
        for (const auto& [entry, state] : models) {
            static_cast<void>(entry);
            for (const auto& sink : state.model->field_sinks) {
                callback_field_sink_contracts->push_back(
                    {sink.function_address,
                     sink.call_instruction_address,
                     sink.field.load_instruction_address,
                     sink.field.displacement,
                     sink.field.width,
                     sink.call,
                     sink.receiver_argument_mask,
                     sink.field.receiver_base_argument_mask});
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
        std::vector<StaticCallbackFieldSinkContract> merged;
        merged.reserve(callback_field_sink_contracts->size());
        const auto same_shape = [](const auto& left, const auto& right) {
            return std::tie(left.function_address,
                            left.call_instruction_address,
                            left.load_instruction_address,
                            left.displacement,
                            left.width,
                            left.call) ==
                   std::tie(right.function_address,
                            right.call_instruction_address,
                            right.load_instruction_address,
                            right.displacement,
                            right.width,
                            right.call);
        };
        for (const auto& sink : *callback_field_sink_contracts) {
            if (!merged.empty() && same_shape(merged.back(), sink)) {
                merged.back().receiver_argument_mask =
                    static_cast<std::uint8_t>(
                        merged.back().receiver_argument_mask |
                        sink.receiver_argument_mask);
                merged.back().base_argument_mask = static_cast<std::uint8_t>(
                    merged.back().base_argument_mask | sink.base_argument_mask);
            } else {
                merged.push_back(sink);
            }
        }
        *callback_field_sink_contracts = std::move(merged);
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
    // Resolve only the concrete field whose unchanged publication was proven
    // in the callee. Ordinary field contents are still just data: the common
    // final standalone-entry gate below validates every potential code word.
    for (const auto& copy : field_copy_contracts) {
        const auto callers = callers_by_callee.find(copy.function_address);
        if (callers == callers_by_callee.end()) continue;
        for (const auto& [caller, call_index] : callers->second) {
            const auto owner = models.find(caller);
            if (owner == models.end() || call_index >= owner->second.model->calls.size()) continue;
            const auto& call = owner->second.model->calls[call_index];
            const auto& argument = call.arguments[copy.argument];
            if (!argument.constants_complete || argument.constants_truncated ||
                argument.input_mask != 0u || argument.constants.size() != 1u) continue;
            const auto base = *argument.constants.begin();
            const auto cell = std::uint64_t(base) + std::uint32_t(copy.displacement);
            if ((base & 3u) != 0u || cell > UINT32_MAX) continue;
            const auto raw = read_image_u32(image, static_cast<std::uint32_t>(cell));
            if (!raw) continue;
            const auto target = executable_constant(image, *raw);
            if (!target) continue;
            StoredCodeAddressCandidate candidate;
            candidate.target_address = *target;
            candidate.store_instruction_addresses = {copy.store_instruction_address};
            candidate.evidence_call_sites = {call.instruction_address};
            candidate.evidence_callees = {copy.function_address};
            merge_candidate(candidate);
        }
    }
    std::vector<const katana::io::ImageSegment*>
        analyzed_source_components;
    analyzed_source_components.reserve(functions.size());
    for (const auto& function : functions) {
        const auto* segment = image.find_segment(
            function.entry_address, sizeof(std::uint16_t));
        if (segment == nullptr) continue;
        if (std::none_of(
                analyzed_source_components.begin(),
                analyzed_source_components.end(),
                [&](const auto* active) {
                    return active != nullptr &&
                           same_static_vector_source_component(
                               *segment, *active);
                }))
            analyzed_source_components.push_back(segment);
    }
    const auto static_vectors = discover_static_code_pointer_vectors(
        image, analyzed_source_components, native_entry_shapes);
    for (const auto& candidate : static_vectors.candidates)
        merge_candidate(candidate);
    candidate_values_truncated =
        candidate_values_truncated || static_vectors.truncated;
    for (const auto& [entry, state] : models) {
        for (const auto& candidate : state.local_candidates)
            merge_candidate(candidate);
        // Some bootstrap registrars store code literals into a global vector
        // through an incoming selector. Admit those literals only when the
        // callee preserves an affine ABI-argument offset, the base retains its
        // exact PC-literal identity, and every statically known caller supplies
        // a complete finite argument domain. A plausible RAM address or an
        // alignment-only index is deliberately insufficient.
        for (const auto& [store_site, store] :
             state.model->indexed_persistent_stores) {
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
                    call_index >= owner->second.model->calls.size() ||
                    store.byte_offset->argument >= 4u) {
                    caller_domain_complete = false;
                    break;
                }
                const auto& call =
                    owner->second.model->calls[call_index];
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
            state.local_candidates_truncated;
        for (const auto& call : state.model->calls) {
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
        const auto shape = independently_bound
                               ? native_entry_shapes
                                     .classify_independent_normal_entry(
                                         candidate->first)
                               : native_entry_shapes.classify(
                                     candidate->first);
        if (shape != GuardedNativeEntryShapeStatus::Valid) {
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
        const auto count_base_models = [&](const auto selector) {
            return std::count_if(
                models.begin(), models.end(),
                [&](const auto& item) {
                    return selector(*item.second.model);
                });
        };
        std::size_t truncated_forwarded_arguments = 0u;
        for (const auto& [entry, state] : models) {
            static_cast<void>(entry);
            for (const auto& call : state.model->calls)
                truncated_forwarded_arguments += std::count_if(
                    call.arguments.begin(), call.arguments.end(),
                    [](const auto& argument) {
                        return argument.code_constants_truncated;
                    });
        }
        std::cerr
            << "KATANA_STATIC_CALLBACK_INVENTORY_LOSS "
            << "field_sink_functions="
            << count_base_models([](const auto& model) {
                   return model.field_sinks_truncated;
               })
            << " persistent_store_functions="
            << count_base_models([](const auto& model) {
                   return model.persistent_stores_truncated;
               })
            << " local_candidate_functions="
            << std::count_if(
                   models.begin(), models.end(),
                   [](const auto& item) {
                       return item.second.local_candidates_truncated;
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

std::vector<StaticExternalLiteralTransferCandidate>
discover_external_literal_transfer_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const StaticExternalLiteralTransferBlock> blocks,
    const std::uint32_t primary_begin, const std::uint64_t primary_size) {
    constexpr std::size_t maximum_transfers = 4096u;
    constexpr std::size_t maximum_writer_distance = 32u;
    if (image.address_model() != katana::io::ImageAddressModel::Sh4DirectMapped)
        return {};
    const auto primary_end = static_cast<std::uint64_t>(primary_begin) + primary_size;
    if (primary_size == 0u || primary_end > (std::uint64_t{1u} << 32u))
        throw std::invalid_argument("Invalid resident literal-transfer source extent");
    const auto canonical = [](const std::uint32_t address)
        -> std::optional<std::uint32_t> {
        const auto region = address & 0xE0000000u;
        if (region != 0u && region != 0x80000000u && region != 0xA0000000u)
            return std::nullopt;
        return (address & 0x1FFFFFFFu) | 0x80000000u;
    };
    std::vector<std::pair<std::uint32_t, std::uint64_t>> block_ranges;
    block_ranges.reserve(blocks.size());
    for (const auto& block : blocks) {
        const auto start = canonical(block.address);
        const auto end = start ? static_cast<std::uint64_t>(*start) + block.byte_size : 0u;
        if (!start || (*start & 1u) != 0u || block.byte_size == 0u ||
            (block.byte_size & 1u) != 0u || *start < primary_begin || end > primary_end)
            continue;
        block_ranges.emplace_back(*start, end);
    }
    std::sort(block_ranges.begin(), block_ranges.end());
    if (block_ranges.empty()) return {};
    const auto source_word = [&](const std::uint32_t address,
                                 const std::uint32_t width)
        -> std::optional<std::uint32_t> {
        const auto source = canonical(address);
        if (!source || *source < primary_begin ||
            static_cast<std::uint64_t>(*source) + width > primary_end)
            return std::nullopt;
        const auto resolved = image.resolve_segment_address(address, width);
        const auto* segment = resolved ? image.find_segment(*resolved, width) : nullptr;
        if (segment == nullptr || !segment->permissions.readable) return std::nullopt;
        const auto offset = segment->byte_offset(*resolved);
        if (!offset || *offset > segment->bytes.size() ||
            width > segment->bytes.size() - *offset) return std::nullopt;
        const bool file_source = !segment->local_source_name.empty() &&
            (segment->source_kind == katana::io::ImageSourceKind::RawBinary ||
             segment->source_kind == katana::io::ImageSourceKind::ElfLoadSegment ||
             segment->source_kind == katana::io::ImageSourceKind::DiscBootFile ||
             segment->source_kind == katana::io::ImageSourceKind::DiscModule);
        if (!file_source && image.find_immutable_range(*resolved, width) == nullptr)
            return std::nullopt;
        std::uint32_t value = 0u;
        for (std::uint32_t byte = 0u; byte < width; ++byte)
            value |= static_cast<std::uint32_t>(segment->bytes[*offset + byte]) << (byte * 8u);
        return value;
    };
    // Several contextual CFG views can own the same instruction. Exact source
    // bytes bind every view; disagreement is never resolved by insertion order.
    std::unordered_map<std::uint32_t, const katana::sh4::DisassemblyLine*> instructions;
    instructions.reserve(lines.size());
    std::set<std::uint32_t> ambiguous;
    for (const auto& line : lines) {
        const auto address = canonical(line.address);
        if (!address || *address < primary_begin || *address >= primary_end) continue;
        const auto [found, inserted] = instructions.emplace(*address, &line);
        if (!inserted && (found->second->opcode != line.opcode ||
                         found->second->is_delay_slot != line.is_delay_slot))
            ambiguous.insert(*address);
    }
    const auto bound_instruction = [&](const std::uint32_t address)
        -> const katana::sh4::DisassemblyLine* {
        const auto found = instructions.find(address);
        if (found == instructions.end() || ambiguous.contains(address)) return nullptr;
        const auto source = source_word(address, 2u);
        if (!source || *source != found->second->opcode) return nullptr;
        return found->second;
    };
    std::vector<StaticExternalLiteralTransferCandidate> result;
    for (const auto& [address, view] : instructions) {
        const auto kind = view->instruction.kind;
        if (kind != katana::sh4::InstructionKind::Jsr &&
            kind != katana::sh4::InstructionKind::Jmp) continue;
        const auto block_after = std::upper_bound(block_ranges.begin(), block_ranges.end(), address,
            [](const auto value, const auto& block) { return value < block.first; });
        if (block_after == block_ranges.begin()) continue;
        const auto& block = *std::prev(block_after);
        if (static_cast<std::uint64_t>(address) + 4u > block.second) continue;
        const auto* transfer = bound_instruction(address);
        const auto* slot = address <= UINT32_MAX - 2u ? bound_instruction(address + 2u) : nullptr;
        if (transfer == nullptr || transfer->is_delay_slot || slot == nullptr ||
            !slot->instruction.is_known() || slot->instruction.changes_control_flow()) continue;
        auto tracked = transfer->instruction.branch_register;
        if (tracked >= 16u) continue;
        auto cursor = address;
        for (std::size_t distance = 0u; distance < maximum_writer_distance && cursor > block.first;
             ++distance) {
            cursor -= 2u;
            const auto* writer = bound_instruction(cursor);
            if (writer == nullptr || writer->is_delay_slot ||
                !writer->instruction.is_known() || writer->instruction.changes_control_flow()) break;
            const auto writes = general_register_write_mask(writer->instruction);
            if ((writes & (std::uint16_t{1u} << tracked)) == 0u) continue;
            if (writer->instruction.destination_register != tracked) break;
            if (writer->instruction.kind == katana::sh4::InstructionKind::MovRegister) {
                tracked = writer->instruction.source_register;
                if (tracked >= 16u) break;
                continue;
            }
            if (writer->instruction.kind != katana::sh4::InstructionKind::MovLongLoadPcRelative) break;
            const auto literal64 = (static_cast<std::uint64_t>(cursor) + 4u) / 4u * 4u +
                                   writer->instruction.displacement;
            if (literal64 > UINT32_MAX) break;
            const auto literal = static_cast<std::uint32_t>(literal64);
            const auto raw = source_word(literal, 4u);
            const auto target = raw ? canonical(*raw) : std::nullopt;
            if (!target || (*target & 1u) != 0u ||
                (*target >= primary_begin && *target < primary_end)) break;
            result.push_back({address, literal, *target,
                              kind == katana::sh4::InstructionKind::Jsr});
            if (result.size() > maximum_transfers)
                throw std::runtime_error("Resident literal-transfer candidate budget exceeded");
            break;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.call_instruction_address, left.literal_address, left.target_address, left.call) <
               std::tie(right.call_instruction_address, right.literal_address, right.target_address, right.call);
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace katana::analysis::detail
