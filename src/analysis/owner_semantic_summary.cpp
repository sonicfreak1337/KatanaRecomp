#include "katana/analysis/owner_semantic_summary.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/ir/register_liveness.hpp"

#include <algorithm>
#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace katana::analysis {
namespace {

using BlockIndex = std::size_t;

void append_reason(OwnerSemanticSummary& summary,
                   const OwnerSemanticSummaryOptions& options,
                   const std::string_view reason) {
    summary.has_contract_gaps = true;
    if (std::find(summary.incomplete_reasons.begin(),
                  summary.incomplete_reasons.end(),
                  reason) != summary.incomplete_reasons.end())
        return;
    if (summary.incomplete_reasons.size() < options.maximum_reasons)
        summary.incomplete_reasons.emplace_back(reason);
}

[[nodiscard]] std::uint8_t width_bytes(const ir::OperandWidth width) noexcept {
    switch (width) {
    case ir::OperandWidth::Bit1:
    case ir::OperandWidth::Bits4:
        return 0u;
    case ir::OperandWidth::Bits8:
        return 1u;
    case ir::OperandWidth::Bits12:
    case ir::OperandWidth::Bits16:
        return 2u;
    case ir::OperandWidth::Bits32:
        return 4u;
    case ir::OperandWidth::Bits64:
        return 8u;
    case ir::OperandWidth::None:
        return 0u;
    }
    return 0u;
}

[[nodiscard]] bool is_closed_dynamic_target(const ir::Instruction& instruction) noexcept {
    return !instruction.resolved_targets.empty() &&
           (instruction.dynamic_target_class == ir::DynamicTargetClass::GuardedComplete ||
            instruction.dynamic_target_class == ir::DynamicTargetClass::ExactGuarded);
}

[[nodiscard]] std::string hex_token(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::uint32_t width_mask(const std::uint8_t width_bytes) noexcept {
    switch (width_bytes) {
    case 1u:
        return 0x000000FFu;
    case 2u:
        return 0x0000FFFFu;
    case 4u:
        return 0xFFFFFFFFu;
    default:
        return 0u;
    }
}

[[nodiscard]] std::uint32_t special_register_mask(
    const ir::SpecialRegister register_value) noexcept {
    const auto index = static_cast<unsigned>(register_value);
    return index < 32u ? (1u << index) : 0u;
}

[[nodiscard]] bool memory_operation_writes_gpr(ir::Operation operation) noexcept;

[[nodiscard]] std::string gpr_expression(const std::uint8_t index) {
    return "gpr:r" + std::to_string(index);
}

[[nodiscard]] std::string displacement_expression(const std::int32_t displacement) {
    return "disp:" + std::to_string(displacement);
}

[[nodiscard]] std::optional<std::string> symbolic_memory_address(
    const ir::Instruction& instruction,
    const std::uint8_t transfer_width) {
    const auto source = gpr_expression(instruction.source_register);
    const auto destination = gpr_expression(instruction.destination_register);
    switch (instruction.operation) {
    case ir::Operation::LoadByteSigned:
    case ir::Operation::LoadWordSigned:
    case ir::Operation::LoadLong:
    case ir::Operation::LoadByteSignedPostIncrement:
    case ir::Operation::LoadWordSignedPostIncrement:
    case ir::Operation::LoadLongPostIncrement:
    case ir::Operation::FmovLoad:
    case ir::Operation::FmovLoadPostIncrement:
    case ir::Operation::LoadSpecialRegisterPostIncrement:
        return source;

    case ir::Operation::StoreByte:
    case ir::Operation::StoreWord:
    case ir::Operation::StoreLong:
    case ir::Operation::FmovStore:
    case ir::Operation::MovcaLong:
        return destination;

    case ir::Operation::StoreBytePreDecrement:
    case ir::Operation::StoreWordPreDecrement:
    case ir::Operation::StoreLongPreDecrement:
    case ir::Operation::FmovStorePreDecrement:
    case ir::Operation::StoreSpecialRegisterPreDecrement:
        return destination + "-predec:" + std::to_string(transfer_width);

    case ir::Operation::LoadByteSignedDisplacement:
    case ir::Operation::LoadWordSignedDisplacement:
    case ir::Operation::LoadLongDisplacement:
        return source + "+" + displacement_expression(instruction.displacement);

    case ir::Operation::StoreByteDisplacement:
    case ir::Operation::StoreWordDisplacement:
    case ir::Operation::StoreLongDisplacement:
        return destination + "+" + displacement_expression(instruction.displacement);

    case ir::Operation::LoadByteSignedR0Indexed:
    case ir::Operation::LoadWordSignedR0Indexed:
    case ir::Operation::LoadLongR0Indexed:
    case ir::Operation::FmovLoadR0Indexed:
        return "gpr:r0+" + source;

    case ir::Operation::StoreByteR0Indexed:
    case ir::Operation::StoreWordR0Indexed:
    case ir::Operation::StoreLongR0Indexed:
    case ir::Operation::FmovStoreR0Indexed:
        return "gpr:r0+" + destination;

    case ir::Operation::LoadByteSignedGbrDisplacement:
    case ir::Operation::LoadWordSignedGbrDisplacement:
    case ir::Operation::LoadLongGbrDisplacement:
    case ir::Operation::StoreByteGbrDisplacement:
    case ir::Operation::StoreWordGbrDisplacement:
    case ir::Operation::StoreLongGbrDisplacement:
        return "gbr+" + displacement_expression(instruction.displacement);

    case ir::Operation::TestByteImmediate:
    case ir::Operation::AndByteImmediate:
    case ir::Operation::XorByteImmediate:
    case ir::Operation::OrByteImmediate:
        return "gbr+gpr:r0";

    case ir::Operation::TestAndSetByte:
        return source;

    case ir::Operation::LoadWordSignedPcRelative:
    case ir::Operation::LoadLongPcRelative:
        // Lowering must publish the effective address for PC-relative loads.
        // Reconstructing it here would hide malformed or stale IR.
        return std::nullopt;

    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> memory_write_value_expression(
    const ir::Instruction& instruction) {
    switch (instruction.operation) {
    case ir::Operation::StoreByte:
    case ir::Operation::StoreWord:
    case ir::Operation::StoreLong:
    case ir::Operation::StoreBytePreDecrement:
    case ir::Operation::StoreWordPreDecrement:
    case ir::Operation::StoreLongPreDecrement:
    case ir::Operation::StoreByteDisplacement:
    case ir::Operation::StoreWordDisplacement:
    case ir::Operation::StoreLongDisplacement:
    case ir::Operation::StoreByteR0Indexed:
    case ir::Operation::StoreWordR0Indexed:
    case ir::Operation::StoreLongR0Indexed:
        return gpr_expression(instruction.source_register);
    case ir::Operation::StoreByteGbrDisplacement:
    case ir::Operation::StoreWordGbrDisplacement:
    case ir::Operation::StoreLongGbrDisplacement:
    case ir::Operation::MovcaLong:
        return "gpr:r0";
    case ir::Operation::StoreSpecialRegisterPreDecrement:
        return "special:" +
               std::to_string(static_cast<unsigned>(instruction.special_register));
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> memory_read_result_expression(
    const ir::Instruction& instruction) {
    if (memory_operation_writes_gpr(instruction.operation))
        return gpr_expression(instruction.destination_register);
    switch (instruction.operation) {
    case ir::Operation::TestByteImmediate:
        return "status:t";
    case ir::Operation::LoadSpecialRegisterPostIncrement:
        return "special:" +
               std::to_string(static_cast<unsigned>(instruction.special_register));
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool is_fpu_operation(const ir::Operation operation) noexcept {
    return operation >= ir::Operation::FmovRegister &&
           operation <= ir::Operation::Fschg;
}

void absorb_scalar_result_defs(OwnerSemanticResultProjection& result,
                               const ir::Instruction& instruction) noexcept {
    const auto defs = ir::instruction_register_use_def(instruction).defs;
    result.gpr_write_mask |= ir::general_register_subset(defs);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::T))
        result.status_write_mask |= static_cast<std::uint8_t>(ir::StatusRegisterBit::T);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::Pr))
        result.special_write_mask |= special_register_mask(ir::SpecialRegister::Pr);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::Gbr))
        result.special_write_mask |= special_register_mask(ir::SpecialRegister::Gbr);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::Mach))
        result.special_write_mask |= special_register_mask(ir::SpecialRegister::Mach);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::Macl))
        result.special_write_mask |= special_register_mask(ir::SpecialRegister::Macl);
    if (ir::register_mask_contains(defs, ir::TrackedRegister::Fpul))
        result.special_write_mask |= special_register_mask(ir::SpecialRegister::Fpul);
    if (instruction.operation == ir::Operation::LoadSpecialRegister ||
        instruction.operation == ir::Operation::LoadSpecialRegisterPostIncrement)
        result.special_write_mask |=
            special_register_mask(instruction.special_register);
    result.status_write_mask |=
        static_cast<std::uint8_t>(instruction.status_effects.writes);
}

[[nodiscard]] bool memory_operation_writes_gpr(const ir::Operation operation) noexcept {
    switch (operation) {
    case ir::Operation::LoadByteSigned:
    case ir::Operation::LoadWordSigned:
    case ir::Operation::LoadLong:
    case ir::Operation::LoadByteSignedPostIncrement:
    case ir::Operation::LoadWordSignedPostIncrement:
    case ir::Operation::LoadLongPostIncrement:
    case ir::Operation::LoadByteSignedDisplacement:
    case ir::Operation::LoadWordSignedDisplacement:
    case ir::Operation::LoadLongDisplacement:
    case ir::Operation::LoadByteSignedR0Indexed:
    case ir::Operation::LoadWordSignedR0Indexed:
    case ir::Operation::LoadLongR0Indexed:
    case ir::Operation::LoadByteSignedGbrDisplacement:
    case ir::Operation::LoadWordSignedGbrDisplacement:
    case ir::Operation::LoadLongGbrDisplacement:
    case ir::Operation::LoadWordSignedPcRelative:
    case ir::Operation::LoadLongPcRelative:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_stateless_loop_instruction(const ir::Instruction& instruction) noexcept {
    if (instruction.memory_effects.access != ir::MemoryAccessKind::None ||
        instruction.status_effects.reads != ir::StatusRegisterBit::None ||
        instruction.status_effects.writes != ir::StatusRegisterBit::None ||
        instruction.accumulator_effects.reads_if_s_clear != ir::AccumulatorRegister::None ||
        instruction.accumulator_effects.reads_if_s_set != ir::AccumulatorRegister::None ||
        instruction.accumulator_effects.writes_if_s_clear != ir::AccumulatorRegister::None ||
        instruction.accumulator_effects.writes_if_s_set != ir::AccumulatorRegister::None ||
        instruction.special_register != ir::SpecialRegister::None)
        return false;
    return instruction.operation == ir::Operation::Nop ||
           instruction.operation == ir::Operation::Branch;
}

void append_u64(std::string& output, const std::uint64_t value) {
    for (std::size_t index = 0u; index < sizeof(value); ++index)
        output.push_back(static_cast<char>((value >> (index * 8u)) & 0xFFu));
}

void append_text(std::string& output,
                 const std::string_view value,
                 const std::size_t maximum_bytes) {
    const auto length = std::min(value.size(), maximum_bytes);
    append_u64(output, length);
    if (length != 0u) output.append(value.data(), length);
}

[[nodiscard]] std::optional<HardwareAccessReference> bounded_hardware_reference(
    const HardwareAccessReference* reference,
    const OwnerSemanticSummaryOptions& options) {
    if (reference == nullptr) return std::nullopt;
    auto copy = *reference;
    if (copy.support_reason.size() > options.maximum_text_bytes)
        copy.support_reason.resize(options.maximum_text_bytes);
    if (copy.register_name.size() > options.maximum_text_bytes)
        copy.register_name.resize(options.maximum_text_bytes);
    return copy;
}

} // namespace

const char* owner_semantic_summary_status_name(
    const OwnerSemanticSummaryStatus status) noexcept {
    switch (status) {
    case OwnerSemanticSummaryStatus::Incomplete:
        return "incomplete";
    case OwnerSemanticSummaryStatus::Complete:
        return "complete";
    case OwnerSemanticSummaryStatus::Truncated:
        return "truncated";
    }
    return "incomplete";
}

const char* owner_semantic_authority_name(
    const OwnerSemanticAuthority authority) noexcept {
    switch (authority) {
    case OwnerSemanticAuthority::Unbound:
        return "unbound";
    case OwnerSemanticAuthority::IdentityBound:
        return "identity-bound";
    case OwnerSemanticAuthority::Invalidated:
        return "invalidated";
    }
    return "invalidated";
}

const char* owner_semantic_effect_kind_name(
    const OwnerSemanticEffectKind kind) noexcept {
    switch (kind) {
    case OwnerSemanticEffectKind::MemoryRead:
        return "memory-read";
    case OwnerSemanticEffectKind::MemoryWrite:
        return "memory-write";
    case OwnerSemanticEffectKind::HardwareRead:
        return "hardware-read";
    case OwnerSemanticEffectKind::HardwareWrite:
        return "hardware-write";
    case OwnerSemanticEffectKind::HardwarePrefetch:
        return "hardware-prefetch";
    case OwnerSemanticEffectKind::CpuStatusRead:
        return "cpu-status-read";
    case OwnerSemanticEffectKind::CpuStatusWrite:
        return "cpu-status-write";
    case OwnerSemanticEffectKind::CpuAccumulatorRead:
        return "cpu-accumulator-read";
    case OwnerSemanticEffectKind::CpuAccumulatorWrite:
        return "cpu-accumulator-write";
    case OwnerSemanticEffectKind::CpuSpecialRegisterRead:
        return "cpu-special-read";
    case OwnerSemanticEffectKind::CpuSpecialRegisterWrite:
        return "cpu-special-write";
    case OwnerSemanticEffectKind::DirectCall:
        return "direct-call";
    case OwnerSemanticEffectKind::Return:
        return "return";
    }
    return "cpu-status-read";
}

OwnerSemanticSummary summarize_owner_semantics(
    const ir::Function& function,
    OwnerSemanticBoundary boundary,
    const std::span<const HardwareAccessReference> hardware_references,
    const OwnerSemanticSummaryOptions options) {
    OwnerSemanticSummary summary;
    summary.boundary = std::move(boundary);
    summary.authority = summary.boundary.exact && summary.boundary.identity_bound
                            ? OwnerSemanticAuthority::IdentityBound
                            : OwnerSemanticAuthority::Unbound;

    const auto invalid_boundary = [&] {
        summary.authority = OwnerSemanticAuthority::Invalidated;
        append_reason(summary, options, "invalid-exact-owner-boundary");
    };
    const auto malformed_boundary =
        summary.boundary.entry_address != function.entry_address ||
        summary.boundary.size == 0u ||
        static_cast<std::uint64_t>(summary.boundary.entry_address) +
                summary.boundary.size >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    if (malformed_boundary) {
        invalid_boundary();
    } else if (!summary.boundary.exact || !summary.boundary.identity_bound ||
               summary.boundary.identity.empty() ||
               summary.boundary.identity.size() > options.maximum_text_bytes) {
        summary.authority = OwnerSemanticAuthority::Unbound;
        append_reason(summary, options, "owner-boundary-not-identity-bound");
    } else {
        summary.boundary_valid = true;
        summary.result.cpu_state_expression =
            "owner-state-v1:" + summary.boundary.identity;
    }

    const auto append_effect = [&](const BlockIndex block_index,
                                   OwnerSemanticEffect effect) {
        if (summary.effects.size() >= options.maximum_effects) {
            summary.effects_truncated = true;
            ++summary.truncated_effect_count;
            append_reason(summary, options, "effect-budget-exceeded");
            return;
        }
        effect.order = static_cast<std::uint16_t>(
            std::min<std::size_t>(summary.effects.size(),
                                  std::numeric_limits<std::uint16_t>::max()));
        if (effect.path_identity.empty())
            effect.path_identity =
                "block:" + hex_token(summary.blocks[block_index].start_address);
        summary.blocks[block_index].effect_indices.push_back(summary.effects.size());
        summary.effects.push_back(std::move(effect));
    };
    const auto append_guard = [&](const BlockIndex block_index,
                                  const std::uint32_t target_address) {
        if (summary.guards.size() >= options.maximum_guards) {
            summary.guards_truncated = true;
            ++summary.truncated_guard_count;
            append_reason(summary, options, "guard-budget-exceeded");
            return;
        }
        OwnerSemanticGuard guard;
        guard.order = static_cast<std::uint16_t>(
            std::min<std::size_t>(summary.guards.size(),
                                  std::numeric_limits<std::uint16_t>::max()));
        guard.expression = "successor:" + hex_token(target_address);
        guard.path_identity =
            "block:" + hex_token(summary.blocks[block_index].start_address);
        summary.guards.push_back(std::move(guard));
    };

    // Index the global audit once.  Owner summaries are built repeatedly for
    // hardware hooks; scanning every audit reference for every instruction
    // would turn that path into OwnerInstructions x GlobalHardwareSites.
    std::unordered_map<
        std::uint32_t,
        std::vector<const HardwareAccessReference*>>
        hardware_by_instruction;
    hardware_by_instruction.reserve(hardware_references.size());
    for (const auto& reference : hardware_references)
        hardware_by_instruction[reference.instruction_address].push_back(
            &reference);
    for (auto& [instruction_address, references] :
         hardware_by_instruction) {
        static_cast<void>(instruction_address);
        std::sort(references.begin(), references.end(),
                  [](const auto* left, const auto* right) {
                      return std::tuple{
                                 static_cast<std::uint8_t>(left->kind),
                                 left->width,
                                 left->canonical_address,
                                 static_cast<std::uint8_t>(left->region),
                                 left->register_name} <
                             std::tuple{
                                 static_cast<std::uint8_t>(right->kind),
                                 right->width,
                                 right->canonical_address,
                                 static_cast<std::uint8_t>(right->region),
                                 right->register_name};
                  });
    }
    const std::vector<const HardwareAccessReference*> no_hardware_references;

    const auto block_end = [&](const std::uint32_t start) {
        return static_cast<std::uint64_t>(start) + 2u;
    };
    const auto in_boundary = [&](const std::uint32_t address) {
        if (!summary.boundary_valid) return true;
        const auto begin = static_cast<std::uint64_t>(summary.boundary.entry_address);
        const auto end = begin + summary.boundary.size;
        return static_cast<std::uint64_t>(address) >= begin &&
               block_end(address) <= end;
    };

    summary.blocks.reserve(function.blocks.size());
    std::unordered_map<std::uint32_t, BlockIndex> block_by_address;
    block_by_address.reserve(function.blocks.size());
    for (const auto& block : function.blocks) {
        const auto index = summary.blocks.size();
        summary.blocks.push_back(OwnerSemanticBlockSummary{});
        summary.blocks.back().start_address = block.start_address;
        if (!in_boundary(block.start_address))
            append_reason(summary, options, "block-outside-owner-boundary");
        if (!block_by_address.emplace(block.start_address, index).second)
            append_reason(summary, options, "duplicate-basic-block-address");
    }

    const auto block_count = function.blocks.size();
    if (block_count == 0u)
        append_reason(summary, options, "owner-has-no-basic-blocks");
    std::vector<std::vector<BlockIndex>> successors(block_count);
    std::vector<std::vector<BlockIndex>> predecessors(block_count);
    for (BlockIndex index = 0u; index < block_count; ++index) {
        const auto& source = function.blocks[index];
        auto& output = summary.blocks[index];
        output.has_indirect_successor = source.has_indirect_successor;
        if (source.has_indirect_successor) {
            output.open_edge = true;
            summary.has_open_edges = true;
            append_reason(summary, options, "indirect-successor-unresolved");
        }
        output.successor_addresses = source.successors;
        for (const auto target_address : source.successors) {
            const auto target = block_by_address.find(target_address);
            if (target == block_by_address.end()) {
                output.open_edge = true;
                summary.has_open_edges = true;
                append_reason(summary, options, "successor-outside-owner-graph");
                continue;
            }
            successors[index].push_back(target->second);
            predecessors[target->second].push_back(index);
        }
    }

    for (BlockIndex index = 0u; index < block_count; ++index) {
        auto& output = summary.blocks[index];
        for (const auto predecessor : predecessors[index])
            output.predecessor_addresses.push_back(
                summary.blocks[predecessor].start_address);
        summary.predecessor_edge_count += predecessors[index].size();
    }

    std::vector<bool> reachable(block_count, false);
    const auto entry = block_by_address.find(function.entry_address);
    if (entry == block_by_address.end()) {
        append_reason(summary, options, "owner-entry-block-missing");
    } else {
        std::deque<BlockIndex> pending{entry->second};
        reachable[entry->second] = true;
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop_front();
            for (const auto successor : successors[current]) {
                if (!reachable[successor]) {
                    reachable[successor] = true;
                    pending.push_back(successor);
                }
            }
        }
    }
    summary.all_blocks_reachable = block_count != 0u &&
                                   std::all_of(reachable.begin(),
                                               reachable.end(),
                                               [](const bool value) { return value; });
    if (!summary.all_blocks_reachable)
        append_reason(summary, options, "owner-graph-has-unreachable-block");
    for (BlockIndex index = 0u; index < block_count; ++index)
        summary.blocks[index].reachable = reachable[index];

    std::vector<bool> block_has_return(block_count, false);
    bool saw_return = false;
    bool saw_direct_call = false;
    bool saw_unrepresentable_terminal = false;
    bool saw_fpu_state = false;

    // Validate instruction placement once while collecting the ordered effect
    // stream.  The graph indexes above are independent of effect extraction.
    for (BlockIndex block_index = 0u; block_index < block_count; ++block_index) {
        const auto& block = function.blocks[block_index];
        auto& output = summary.blocks[block_index];
        if (block.instructions.empty())
            append_reason(summary, options, "empty-basic-block");
        std::optional<std::uint32_t> previous_address;
        for (const auto& instruction : block.instructions) {
            ++summary.instruction_count;
            if (!in_boundary(instruction.source_address))
                append_reason(summary, options, "instruction-outside-owner-boundary");
            if (previous_address &&
                (static_cast<std::uint64_t>(*previous_address) + 2u !=
                 instruction.source_address))
                append_reason(summary, options, "non-contiguous-basic-block");
            previous_address = instruction.source_address;

            absorb_scalar_result_defs(summary.result, instruction);
            if (is_fpu_operation(instruction.operation)) {
                saw_fpu_state = true;
                append_reason(summary, options, "fpu-result-projection-unavailable");
            }

            const auto hardware = hardware_by_instruction.find(
                instruction.source_address);
            const auto& matching_hardware =
                hardware != hardware_by_instruction.end()
                    ? hardware->second
                    : no_hardware_references;
            if (!matching_hardware.empty())
                summary.provider_contract_required = true;
            if (instruction.operation == ir::Operation::Prefetch &&
                matching_hardware.empty())
                append_reason(summary, options,
                              "hardware-prefetch-address-unresolved");

            if (instruction.operation == ir::Operation::Unknown) {
                summary.has_unknown_operations = true;
                append_reason(summary, options, "unknown-ir-operation");
            }

            const auto memory_access = instruction.memory_effects.access;
            if (memory_access != ir::MemoryAccessKind::None) {
                if (instruction.memory_effects.access_count != 1u)
                    append_reason(summary, options,
                                  "memory-access-cardinality-unrepresentable");
                if (matching_hardware.size() > 1u)
                    append_reason(summary, options,
                                  "hardware-access-correlation-ambiguous");
                const auto append_memory_effect =
                    [&](const HardwareAccessReference* hardware) {
                        OwnerSemanticEffect effect;
                        effect.instruction_address = instruction.source_address;
                        effect.width_bytes = width_bytes(instruction.memory_effects.width);
                        effect.destination_register = instruction.destination_register;
                        effect.source_register = instruction.source_register;
                        effect.exact_address = instruction.effective_address;
                        effect.hardware_reference =
                            bounded_hardware_reference(hardware, options);
                        if (effect.width_bytes == 0u && hardware != nullptr)
                            effect.width_bytes = hardware->width;
                        if (effect.width_bytes == 0u)
                            append_reason(summary, options, "memory-width-unresolved");
                        if (hardware != nullptr) {
                            effect.canonical_address_known = true;
                            effect.canonical_address = hardware->canonical_address;
                            effect.region = dreamcast_hardware_region_name(hardware->region);
                            effect.register_name = hardware->register_name.substr(
                                0u, options.maximum_text_bytes);
                            effect.resource = effect.register_name.empty()
                                                  ? effect.region
                                                  : effect.register_name;
                            effect.address_expression =
                                "canonical:" + hex_token(hardware->canonical_address);
                            switch (hardware->kind) {
                            case HardwareAccessKind::Read:
                                effect.kind = OwnerSemanticEffectKind::HardwareRead;
                                effect.provider_operation =
                                    runtime::NativePortProviderOperation::Read;
                                break;
                            case HardwareAccessKind::Write:
                                effect.kind = OwnerSemanticEffectKind::HardwareWrite;
                                effect.provider_operation =
                                    runtime::NativePortProviderOperation::Write;
                                break;
                            case HardwareAccessKind::Prefetch:
                                effect.kind = OwnerSemanticEffectKind::HardwarePrefetch;
                                effect.provider_operation =
                                    runtime::NativePortProviderOperation::Prefetch;
                                effect.provider_resource_kind =
                                    runtime::NativePortProviderResourceKind::Queue;
                                break;
                            }
                            if (hardware->kind != HardwareAccessKind::Prefetch)
                                effect.provider_resource_kind =
                                    runtime::NativePortProviderResourceKind::HardwareRegister;
                        } else {
                            effect.kind = memory_access == ir::MemoryAccessKind::Write
                                              ? OwnerSemanticEffectKind::MemoryWrite
                                              : OwnerSemanticEffectKind::MemoryRead;
                            effect.provider_operation =
                                memory_access == ir::MemoryAccessKind::Write
                                    ? runtime::NativePortProviderOperation::Write
                                    : runtime::NativePortProviderOperation::Read;
                            effect.provider_resource_kind =
                                runtime::NativePortProviderResourceKind::Memory;
                            effect.region = "guest-memory";
                            if (effect.exact_address) {
                                effect.canonical_address_known = true;
                                effect.canonical_address = *effect.exact_address;
                                effect.address_expression =
                                    "guest:" + hex_token(*effect.exact_address);
                            } else {
                                const auto expression = symbolic_memory_address(
                                    instruction, effect.width_bytes);
                                if (expression)
                                    effect.address_expression = *expression;
                                else {
                                    effect.address_expression = "unknown";
                                    append_reason(summary, options,
                                                  "memory-address-expression-unresolved");
                                }
                            }
                            effect.resource = "memory";
                        }
                        if (memory_access == ir::MemoryAccessKind::Write) {
                            const auto expression =
                                memory_write_value_expression(instruction);
                            if (expression)
                                effect.value_expression = *expression;
                            else {
                                effect.value_expression = "unknown";
                                append_reason(summary, options,
                                              "memory-write-value-unrepresentable");
                            }
                            effect.write_mask = width_mask(effect.width_bytes);
                        } else {
                            const auto expression =
                                memory_read_result_expression(instruction);
                            if (expression)
                                effect.result_expression = *expression;
                            else {
                                effect.result_expression = "unknown";
                                append_reason(summary, options,
                                              "memory-read-result-unrepresentable");
                            }
                        }
                        append_effect(block_index, std::move(effect));
                    };
                if (matching_hardware.empty()) append_memory_effect(nullptr);
                else
                    for (const auto* reference : matching_hardware)
                        append_memory_effect(reference);
            } else {
                // Hardware audit references are authoritative observations of
                // an access even when an older IR shard did not retain a
                // MemoryEffects record.  Preserve every matching reference;
                // never collapse two widths/resources into one wildcard.
                if (matching_hardware.size() > 1u)
                    append_reason(summary, options,
                                  "hardware-access-correlation-ambiguous");
                for (const auto* reference : matching_hardware) {
                    OwnerSemanticEffect effect;
                    effect.instruction_address = instruction.source_address;
                    effect.width_bytes = reference->width;
                    effect.canonical_address_known = true;
                    effect.canonical_address = reference->canonical_address;
                    effect.hardware_reference =
                        bounded_hardware_reference(reference, options);
                    effect.region = dreamcast_hardware_region_name(reference->region);
                    effect.register_name = reference->register_name.substr(
                        0u, options.maximum_text_bytes);
                    effect.resource = effect.register_name.empty()
                                          ? effect.region
                                          : effect.register_name;
                    effect.address_expression =
                        "canonical:" + hex_token(reference->canonical_address);
                    effect.provider_resource_kind =
                        runtime::NativePortProviderResourceKind::HardwareRegister;
                    switch (reference->kind) {
                    case HardwareAccessKind::Read:
                        effect.kind = OwnerSemanticEffectKind::HardwareRead;
                        effect.provider_operation =
                            runtime::NativePortProviderOperation::Read;
                        if (const auto expression =
                                memory_read_result_expression(instruction))
                            effect.result_expression = *expression;
                        else {
                            effect.result_expression = "unknown";
                            append_reason(summary, options,
                                          "hardware-read-result-unrepresentable");
                        }
                        break;
                    case HardwareAccessKind::Write:
                        effect.kind = OwnerSemanticEffectKind::HardwareWrite;
                        effect.provider_operation =
                            runtime::NativePortProviderOperation::Write;
                        if (const auto expression =
                                memory_write_value_expression(instruction))
                            effect.value_expression = *expression;
                        else {
                            effect.value_expression = "unknown";
                            append_reason(summary, options,
                                          "hardware-write-value-unrepresentable");
                        }
                        effect.write_mask = width_mask(effect.width_bytes);
                        break;
                    case HardwareAccessKind::Prefetch:
                        effect.kind = OwnerSemanticEffectKind::HardwarePrefetch;
                        effect.provider_operation =
                            runtime::NativePortProviderOperation::Prefetch;
                        effect.provider_resource_kind =
                            runtime::NativePortProviderResourceKind::Queue;
                        break;
                    }
                    append_effect(block_index, std::move(effect));
                }
            }

            // CPU register, status, accumulator and special-register changes
            // are terminal architectural state, not provider resources.  They
            // are retained in result masks by absorb_scalar_result_defs().
            // Keeping them out of the ordered resource-effect stream prevents
            // a status comparison or RTS from masquerading as a device read.

            if (instruction.operation == ir::Operation::Call) {
                saw_direct_call = true;
                const auto exact_call_target =
                    instruction.target_address
                        ? instruction.target_address
                        : instruction.resolved_targets.size() == 1u
                              ? std::optional<std::uint32_t>{instruction.resolved_targets.front()}
                              : std::nullopt;
                if (!exact_call_target) {
                    output.open_edge = true;
                    summary.has_open_edges = true;
                    append_reason(summary, options, "direct-call-target-unresolved");
                } else
                    append_reason(summary, options,
                                  "direct-call-effect-not-composed");
            } else if (instruction.operation == ir::Operation::Return) {
                saw_return = true;
                block_has_return[block_index] = true;
                summary.result.action =
                    runtime::NativePortProviderReturnAction::Return;
            } else if (instruction.operation == ir::Operation::TrapAlways ||
                       instruction.operation == ir::Operation::ReturnFromException ||
                       instruction.operation == ir::Operation::Sleep) {
                saw_unrepresentable_terminal = true;
                append_reason(summary, options,
                              "non-return-terminal-result-unrepresentable");
            }

            if ((instruction.operation == ir::Operation::JumpRegister ||
                 instruction.operation == ir::Operation::CallRegister) &&
                !is_closed_dynamic_target(instruction)) {
                output.open_edge = true;
                summary.has_open_edges = true;
                append_reason(summary, options, "dynamic-target-unresolved");
            }
            if (instruction.operation == ir::Operation::Branch ||
                instruction.operation == ir::Operation::BranchIfTrue ||
                instruction.operation == ir::Operation::BranchIfFalse) {
                for (const auto target_address : output.successor_addresses)
                    append_guard(block_index, target_address);
            }
        }
    }

    // Iterative Kosaraju keeps the summary usable for large owners without
    // consuming the host call stack.  Missing successors were already marked
    // open above and are intentionally absent from this graph.
    std::vector<bool> visited(block_count, false);
    std::vector<BlockIndex> finish_order;
    finish_order.reserve(block_count);
    const auto visit_forward = [&](const BlockIndex root) {
        std::vector<std::pair<BlockIndex, std::size_t>> stack;
        stack.emplace_back(root, 0u);
        visited[root] = true;
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.second < successors[frame.first].size()) {
                const auto next = successors[frame.first][frame.second++];
                if (!visited[next]) {
                    visited[next] = true;
                    stack.emplace_back(next, 0u);
                }
            } else {
                finish_order.push_back(frame.first);
                stack.pop_back();
            }
        }
    };
    for (BlockIndex index = 0u; index < block_count; ++index)
        if (!visited[index]) visit_forward(index);

    std::vector<std::size_t> old_component(block_count, std::numeric_limits<std::size_t>::max());
    std::vector<std::vector<BlockIndex>> old_components;
    for (auto iterator = finish_order.rbegin(); iterator != finish_order.rend(); ++iterator) {
        if (old_component[*iterator] != std::numeric_limits<std::size_t>::max()) continue;
        const auto component = old_components.size();
        old_components.emplace_back();
        std::vector<BlockIndex> stack{*iterator};
        old_component[*iterator] = component;
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            old_components.back().push_back(current);
            for (const auto predecessor : predecessors[current]) {
                if (old_component[predecessor] == std::numeric_limits<std::size_t>::max()) {
                    old_component[predecessor] = component;
                    stack.push_back(predecessor);
                }
            }
        }
    }

    std::vector<std::size_t> component_order(old_components.size());
    for (std::size_t index = 0u; index < component_order.size(); ++index)
        component_order[index] = index;
    const auto component_min_address = [&](const std::size_t component) {
        std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
        for (const auto block_index : old_components[component])
            minimum = std::min(minimum, summary.blocks[block_index].start_address);
        return minimum;
    };
    std::sort(component_order.begin(), component_order.end(), [&](const auto left, const auto right) {
        return component_min_address(left) < component_min_address(right);
    });
    std::vector<std::size_t> normalized_component(old_components.size());
    for (std::size_t index = 0u; index < component_order.size(); ++index)
        normalized_component[component_order[index]] = index;

    summary.sccs.resize(old_components.size());
    for (std::size_t old_index = 0u; old_index < old_components.size(); ++old_index) {
        const auto new_index = normalized_component[old_index];
        auto& scc = summary.sccs[new_index];
        scc.block_indices = old_components[old_index];
        std::sort(scc.block_indices.begin(), scc.block_indices.end(), [&](const auto a, const auto b) {
            return summary.blocks[a].start_address < summary.blocks[b].start_address;
        });
        for (const auto block_index : scc.block_indices)
            summary.blocks[block_index].scc_index = new_index;
        scc.cyclic = scc.block_indices.size() > 1u;
        for (const auto block_index : scc.block_indices) {
            for (const auto successor : successors[block_index]) {
                if (old_component[successor] == old_index) {
                    if (successor == block_index) scc.cyclic = true;
                } else {
                    scc.successor_scc_indices.push_back(
                        normalized_component[old_component[successor]]);
                }
            }
            scc.open_edge |= summary.blocks[block_index].open_edge;
            scc.effect_indices.insert(scc.effect_indices.end(),
                                      summary.blocks[block_index].effect_indices.begin(),
                                      summary.blocks[block_index].effect_indices.end());
        }
        std::sort(scc.successor_scc_indices.begin(), scc.successor_scc_indices.end());
        scc.successor_scc_indices.erase(
            std::unique(scc.successor_scc_indices.begin(), scc.successor_scc_indices.end()),
            scc.successor_scc_indices.end());
        scc.structurally_stable =
            scc.cyclic && std::all_of(scc.block_indices.begin(), scc.block_indices.end(),
                                      [&](const auto block_index) {
                                          return std::all_of(
                                              function.blocks[block_index].instructions.begin(),
                                              function.blocks[block_index].instructions.end(),
                                              is_stateless_loop_instruction);
                                      });
        if (scc.cyclic) {
            if (!scc.structurally_stable) {
                // No abstract value transfer is performed by this structural
                // layer, so an effectful SCC has no completed iteration to
                // report.  It remains explicitly fail-closed rather than
                // pretending that a budget-sized probe was a fixpoint.
                scc.fixed_point_iterations = 0u;
                summary.has_unstable_loops = true;
                append_reason(summary, options, "unstable-loop-scc");
            } else
                scc.fixed_point_iterations = 1u;
        } else {
            scc.fixed_point_iterations = 1u;
        }
    }

    bool every_terminal_path_returns = saw_return;
    for (BlockIndex block_index = 0u; block_index < block_count; ++block_index) {
        if (!reachable[block_index] || !successors[block_index].empty() ||
            summary.blocks[block_index].has_indirect_successor)
            continue;
        if (!block_has_return[block_index]) {
            every_terminal_path_returns = false;
            append_reason(summary, options, "terminal-block-without-return");
        }
    }
    if (!saw_return)
        append_reason(summary, options, "owner-return-not-proven");

    summary.control_flow_closed = !summary.has_open_edges &&
                                  !summary.has_unknown_operations &&
                                  !summary.has_unstable_loops;
    if (summary.effects_truncated || summary.guards_truncated)
        summary.status = OwnerSemanticSummaryStatus::Truncated;
    else if (summary.boundary_valid && summary.authority == OwnerSemanticAuthority::IdentityBound &&
             summary.control_flow_closed && summary.all_blocks_reachable &&
             !summary.has_contract_gaps && !summary.effects_truncated &&
             !summary.guards_truncated)
        summary.status = OwnerSemanticSummaryStatus::Complete;
    else
        summary.status = OwnerSemanticSummaryStatus::Incomplete;

    // The state expression is exact because it is bound to the authenticated
    // owner bytes and the masks above enumerate every scalar architectural
    // definition.  Calls, FPU state and non-return terminals remain outside
    // this first compositional leaf contract and therefore fail closed.
    summary.result.complete =
        summary.status == OwnerSemanticSummaryStatus::Complete &&
        every_terminal_path_returns && !saw_direct_call &&
        !saw_unrepresentable_terminal && !saw_fpu_state;

    std::string digest_material;
    append_u64(digest_material, summary.boundary.entry_address);
    append_u64(digest_material, summary.boundary.size);
    append_u64(digest_material, summary.boundary.exact);
    append_u64(digest_material, summary.boundary.identity_bound);
    append_text(digest_material, summary.boundary.identity, options.maximum_text_bytes);
    append_u64(digest_material, static_cast<std::uint8_t>(summary.status));
    append_u64(digest_material, static_cast<std::uint8_t>(summary.authority));
    append_u64(digest_material, summary.boundary_valid);
    append_u64(digest_material, summary.control_flow_closed);
    append_u64(digest_material, summary.all_blocks_reachable);
    append_u64(digest_material, summary.has_unknown_operations);
    append_u64(digest_material, summary.has_open_edges);
    append_u64(digest_material, summary.has_unstable_loops);
    append_u64(digest_material, summary.provider_contract_required);
    append_u64(digest_material, summary.has_contract_gaps);
    append_u64(digest_material, summary.effects_truncated);
    append_u64(digest_material, summary.guards_truncated);
    append_u64(digest_material, summary.instruction_count);
    append_u64(digest_material, summary.predecessor_edge_count);
    append_u64(digest_material, summary.truncated_effect_count);
    append_u64(digest_material, summary.truncated_guard_count);
    append_u64(digest_material, summary.blocks.size());
    for (const auto& block : summary.blocks) {
        append_u64(digest_material, block.start_address);
        append_u64(digest_material, block.scc_index);
        append_u64(digest_material, block.reachable);
        append_u64(digest_material, block.has_indirect_successor);
        append_u64(digest_material, block.open_edge);
        append_u64(digest_material, block.successor_addresses.size());
        for (const auto address : block.successor_addresses) append_u64(digest_material, address);
        append_u64(digest_material, block.predecessor_addresses.size());
        for (const auto address : block.predecessor_addresses)
            append_u64(digest_material, address);
        append_u64(digest_material, block.effect_indices.size());
        for (const auto index : block.effect_indices) append_u64(digest_material, index);
    }
    append_u64(digest_material, summary.sccs.size());
    for (const auto& scc : summary.sccs) {
        append_u64(digest_material, scc.block_indices.size());
        for (const auto index : scc.block_indices) append_u64(digest_material, index);
        append_u64(digest_material, scc.successor_scc_indices.size());
        for (const auto index : scc.successor_scc_indices)
            append_u64(digest_material, index);
        append_u64(digest_material, scc.effect_indices.size());
        for (const auto index : scc.effect_indices) append_u64(digest_material, index);
        append_u64(digest_material, scc.fixed_point_iterations);
        append_u64(digest_material, scc.cyclic);
        append_u64(digest_material, scc.structurally_stable);
        append_u64(digest_material, scc.open_edge);
    }
    append_u64(digest_material, summary.guards.size());
    for (const auto& guard : summary.guards) {
        append_u64(digest_material, guard.order);
        append_text(digest_material, guard.expression, options.maximum_text_bytes);
        append_text(digest_material, guard.path_identity, options.maximum_text_bytes);
    }
    append_u64(digest_material, summary.effects.size());
    for (const auto& effect : summary.effects) {
        append_u64(digest_material, effect.order);
        append_u64(digest_material, effect.instruction_address);
        append_u64(digest_material, static_cast<std::uint8_t>(effect.kind));
        append_u64(digest_material,
                   static_cast<std::uint8_t>(effect.provider_operation));
        append_u64(digest_material,
                   static_cast<std::uint8_t>(effect.provider_resource_kind));
        append_u64(digest_material, effect.canonical_address_known);
        append_u64(digest_material, effect.canonical_address);
        append_u64(digest_material, effect.write_mask);
        append_u64(digest_material, effect.clear_mask);
        append_text(digest_material, effect.region, options.maximum_text_bytes);
        append_text(digest_material, effect.register_name, options.maximum_text_bytes);
        append_text(digest_material, effect.resource, options.maximum_text_bytes);
        append_text(digest_material, effect.address_expression, options.maximum_text_bytes);
        append_text(digest_material, effect.value_expression, options.maximum_text_bytes);
        append_text(digest_material, effect.result_expression, options.maximum_text_bytes);
        append_text(digest_material, effect.path_identity, options.maximum_text_bytes);
        append_u64(digest_material, effect.exact_address.has_value());
        append_u64(digest_material, effect.exact_address.value_or(0u));
        append_u64(digest_material, effect.width_bytes);
        append_u64(digest_material, effect.destination_register);
        append_u64(digest_material, effect.source_register);
        append_u64(digest_material, static_cast<std::uint8_t>(effect.special_register));
        append_u64(digest_material, effect.hardware_reference.has_value());
        if (effect.hardware_reference) {
            append_u64(digest_material, effect.hardware_reference->guest_address);
            append_u64(digest_material, effect.hardware_reference->canonical_address);
            append_u64(digest_material,
                       static_cast<std::uint8_t>(effect.hardware_reference->region));
            append_u64(digest_material,
                       static_cast<std::uint8_t>(effect.hardware_reference->kind));
            append_u64(digest_material, effect.hardware_reference->width);
            append_u64(digest_material, effect.hardware_reference->aperture_mapped);
            append_u64(digest_material,
                       static_cast<std::uint8_t>(
                           effect.hardware_reference->runtime_support));
            append_text(digest_material,
                        effect.hardware_reference->register_name,
                        options.maximum_text_bytes);
            append_text(digest_material,
                        effect.hardware_reference->support_reason,
                        options.maximum_text_bytes);
        }
    }
    append_u64(digest_material, static_cast<std::uint8_t>(summary.result.action));
    append_u64(digest_material, summary.result.gpr_write_mask);
    append_u64(digest_material, summary.result.special_write_mask);
    append_u64(digest_material, summary.result.status_write_mask);
    append_text(digest_material, summary.result.target_expression,
                options.maximum_text_bytes);
    append_text(digest_material, summary.result.error_expression,
                options.maximum_text_bytes);
    append_text(digest_material, summary.result.cpu_state_expression,
                options.maximum_text_bytes);
    append_text(digest_material, summary.result.title_state_expression,
                options.maximum_text_bytes);
    append_u64(digest_material, summary.result.complete);
    append_u64(digest_material, summary.incomplete_reasons.size());
    for (const auto& reason : summary.incomplete_reasons)
        append_text(digest_material, reason, options.maximum_text_bytes);
    summary.digest = "sha256:" + katana::io::sha256_bytes(digest_material);
    return summary;
}

} // namespace katana::analysis
