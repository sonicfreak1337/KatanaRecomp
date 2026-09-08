#include "returned_receiver_diagnostic.hpp"

#include "katana/analysis/value_analysis.hpp"

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
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis::detail {
namespace {

using ReturnedReceiverOrigin = StaticReturnedReceiverOrigin;
using StaticCallbackFieldSinkContract =
    katana::analysis::StaticCallbackFieldSinkContract;

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

[[nodiscard]] bool contains_successor(
    const katana::ir::BasicBlock& block,
    const std::uint32_t address) noexcept {
    return std::find(block.successors.begin(), block.successors.end(), address) !=
           block.successors.end();
}


struct ReturnedReceiverLiteral final {
    std::uint32_t literal_address = 0u;
    std::uint32_t literal_value = 0u;

    bool operator==(const ReturnedReceiverLiteral&) const = default;
};

struct ReturnedReceiverValue final {
    bool unknown = true;
    std::optional<ReturnedReceiverOrigin> origin;
    bool may_be_null = false;

    bool operator==(const ReturnedReceiverValue&) const = default;
};

[[nodiscard]] std::optional<ReturnedReceiverLiteral>
join_returned_receiver_literals(
    const std::optional<ReturnedReceiverLiteral>& left,
    const std::optional<ReturnedReceiverLiteral>& right) noexcept {
    if (!left.has_value() || !right.has_value() || *left != *right)
        return std::nullopt;
    return left;
}

ReturnedReceiverValue returned_receiver_unknown() noexcept { return {}; }

ReturnedReceiverValue returned_receiver_null() noexcept {
    ReturnedReceiverValue value;
    value.unknown = false;
    value.may_be_null = true;
    return value;
}

ReturnedReceiverValue returned_receiver_origin(
    const ReturnedReceiverOrigin origin) {
    ReturnedReceiverValue value;
    value.unknown = false;
    value.origin = origin;
    return value;
}

[[nodiscard]] ReturnedReceiverValue join_returned_receiver_values(
    const ReturnedReceiverValue& left,
    const ReturnedReceiverValue& right) {
    if (left.unknown || right.unknown) return returned_receiver_unknown();
    if (left.origin.has_value() && right.origin.has_value() &&
        *left.origin != *right.origin)
        return returned_receiver_unknown();

    ReturnedReceiverValue joined;
    joined.unknown = false;
    joined.origin = left.origin.has_value() ? left.origin : right.origin;
    joined.may_be_null = left.may_be_null || right.may_be_null;
    if (!joined.origin.has_value() && !joined.may_be_null)
        return returned_receiver_unknown();
    return joined;
}

struct ReturnedReceiverState final {
    std::array<ReturnedReceiverValue, 16u> registers{};
    // This lane is independent from receiver identity and carries only one
    // exact immutable PC-relative literal into a non-relative JSR register.
    std::array<std::optional<ReturnedReceiverLiteral>, 16u>
        literal_registers{};
    std::map<std::int32_t, ReturnedReceiverValue> stack_values;
    bool stack_identity_valid = true;

    bool operator==(const ReturnedReceiverState&) const = default;

    void clear_stack() noexcept {
        stack_values.clear();
        stack_identity_valid = false;
    }
};

void clear_returned_receiver_register(ReturnedReceiverState& state,
                                      const std::uint8_t register_index) {
    if (register_index >= state.registers.size()) return;
    state.registers[register_index] = returned_receiver_unknown();
    state.literal_registers[register_index].reset();
}

[[nodiscard]] bool merge_returned_receiver_state(
    ReturnedReceiverState& destination,
    const ReturnedReceiverState& incoming) {
    ReturnedReceiverState merged = destination;
    for (std::size_t index = 0u; index < merged.registers.size(); ++index)
        merged.registers[index] = join_returned_receiver_values(
            destination.registers[index], incoming.registers[index]);
    for (std::size_t index = 0u; index < merged.literal_registers.size();
         ++index)
        merged.literal_registers[index] = join_returned_receiver_literals(
            destination.literal_registers[index],
            incoming.literal_registers[index]);

    if (!destination.stack_identity_valid || !incoming.stack_identity_valid) {
        merged.clear_stack();
    } else {
        std::set<std::int32_t> keys;
        for (const auto& entry : destination.stack_values)
            keys.insert(entry.first);
        for (const auto& entry : incoming.stack_values)
            keys.insert(entry.first);

        merged.stack_values.clear();
        for (const auto key : keys) {
            const auto left = destination.stack_values.find(key);
            const auto right = incoming.stack_values.find(key);
            const auto left_value = left == destination.stack_values.end()
                                        ? returned_receiver_unknown()
                                        : left->second;
            const auto right_value = right == incoming.stack_values.end()
                                         ? returned_receiver_unknown()
                                         : right->second;
            const auto joined =
                join_returned_receiver_values(left_value, right_value);
            if (!joined.unknown) merged.stack_values.emplace(key, joined);
        }
        merged.stack_identity_valid = true;
    }

    if (merged == destination) return false;
    destination = std::move(merged);
    return true;
}

struct ReturnedDecodedLineIndex final {
    std::map<std::uint32_t, const katana::sh4::DisassemblyLine*> lines;
    std::set<std::uint32_t> ambiguous;
};

[[nodiscard]] const katana::sh4::DisassemblyLine* returned_decoded_line_at(
    const ReturnedDecodedLineIndex& index,
    const std::uint32_t address) noexcept {
    if (index.ambiguous.contains(address)) return nullptr;
    const auto iterator = index.lines.find(address);
    return iterator == index.lines.end() ? nullptr : iterator->second;
}

[[nodiscard]] bool is_memory_operation(const katana::ir::Operation operation) {
    using Operation = katana::ir::Operation;
    switch (operation) {
    case Operation::LoadByteSigned:
    case Operation::LoadWordSigned:
    case Operation::LoadLong:
    case Operation::StoreByte:
    case Operation::StoreWord:
    case Operation::StoreLong:
    case Operation::StoreBytePreDecrement:
    case Operation::StoreWordPreDecrement:
    case Operation::StoreLongPreDecrement:
    case Operation::LoadByteSignedPostIncrement:
    case Operation::LoadWordSignedPostIncrement:
    case Operation::LoadLongPostIncrement:
    case Operation::StoreByteDisplacement:
    case Operation::StoreWordDisplacement:
    case Operation::StoreLongDisplacement:
    case Operation::LoadByteSignedDisplacement:
    case Operation::LoadWordSignedDisplacement:
    case Operation::LoadLongDisplacement:
    case Operation::StoreByteR0Indexed:
    case Operation::StoreWordR0Indexed:
    case Operation::StoreLongR0Indexed:
    case Operation::LoadByteSignedR0Indexed:
    case Operation::LoadWordSignedR0Indexed:
    case Operation::LoadLongR0Indexed:
    case Operation::StoreByteGbrDisplacement:
    case Operation::StoreWordGbrDisplacement:
    case Operation::StoreLongGbrDisplacement:
    case Operation::LoadByteSignedGbrDisplacement:
    case Operation::LoadWordSignedGbrDisplacement:
    case Operation::LoadLongGbrDisplacement:
    case Operation::LoadWordSignedPcRelative:
    case Operation::LoadLongPcRelative:
    case Operation::FmovLoad:
    case Operation::FmovLoadPostIncrement:
    case Operation::FmovLoadR0Indexed:
    case Operation::FmovStore:
    case Operation::FmovStorePreDecrement:
    case Operation::FmovStoreR0Indexed:
    case Operation::StoreSpecialRegister:
    case Operation::StoreSpecialRegisterPreDecrement:
    case Operation::LoadSpecialRegister:
    case Operation::LoadSpecialRegisterPostIncrement:
    case Operation::Prefetch:
    case Operation::Ocbi:
    case Operation::Ocbp:
    case Operation::Ocbwb:
    case Operation::MovcaLong:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::optional<std::uint32_t>
read_returned_receiver_pc_literal(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 4u);
    if (!resolved.has_value() ||
        image.find_immutable_range(*resolved, 4u) == nullptr)
        return std::nullopt;
    return read_image_u32(image, *resolved);
}

[[nodiscard]] bool apply_returned_receiver_noncontrol(
    ReturnedReceiverState& state,
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& decoded,
    const katana::ir::Instruction& instruction) {
    using Operation = katana::ir::Operation;
    using InstructionKind = katana::sh4::InstructionKind;

    if (!decoded.instruction.is_known() ||
        decoded.instruction.changes_control_flow() ||
        instruction.operation == Operation::Unknown)
        return false;

    if (instruction.operation == Operation::FmovRegister) {
        if (instruction.destination_register >= 16u ||
            instruction.source_register >= 16u)
            return false;
        // FMOV uses the FPU lane and does not kill an equal-numbered GPR.
        return true;
    }

    if (instruction.operation == Operation::MovRegister) {
        if (decoded.instruction.kind != InstructionKind::MovRegister ||
            instruction.destination_register >= 16u ||
            instruction.source_register >= 16u)
            return false;
        state.registers[instruction.destination_register] =
            state.registers[instruction.source_register];
        state.literal_registers[instruction.destination_register] =
            state.literal_registers[instruction.source_register];
        if (instruction.destination_register == 15u) state.clear_stack();
        return true;
    }

    if (instruction.operation == Operation::MovImmediate ||
        instruction.operation == Operation::Constant32) {
        if (instruction.destination_register >= 16u) return false;
        state.registers[instruction.destination_register] =
            instruction.immediate == 0 ? returned_receiver_null()
                                        : returned_receiver_unknown();
        state.literal_registers[instruction.destination_register].reset();
        if (instruction.destination_register == 15u) state.clear_stack();
        return true;
    }

    const auto long_store =
        instruction.operation == Operation::StoreLong ||
        instruction.operation == Operation::StoreLongDisplacement;
    if (long_store) {
        const auto expected_kind =
            instruction.operation == Operation::StoreLong
                ? InstructionKind::MovLongStore
                : InstructionKind::MovLongStoreDisplacement;
        if (decoded.instruction.kind != expected_kind ||
            instruction.destination_register >= 16u ||
            instruction.source_register >= 16u)
            return false;
        if (instruction.destination_register != 15u) {
            state.clear_stack();
            return true;
        }
        if (!state.stack_identity_valid) return true;
        const auto displacement = instruction.operation == Operation::StoreLong
                                      ? 0
                                      : instruction.displacement;
        const auto& value = state.registers[instruction.source_register];
        if (value.unknown)
            state.stack_values.erase(displacement);
        else
            state.stack_values[displacement] = value;
        return true;
    }

    const auto long_load =
        instruction.operation == Operation::LoadLong ||
        instruction.operation == Operation::LoadLongDisplacement;
    if (long_load) {
        const auto expected_kind =
            instruction.operation == Operation::LoadLong
                ? InstructionKind::MovLongLoad
                : InstructionKind::MovLongLoadDisplacement;
        if (decoded.instruction.kind != expected_kind ||
            instruction.destination_register >= 16u ||
            instruction.source_register >= 16u)
            return false;
        const auto displacement = instruction.operation == Operation::LoadLong
                                      ? 0
                                      : instruction.displacement;
        if (instruction.source_register == 15u &&
            state.stack_identity_valid) {
            const auto value = state.stack_values.find(displacement);
            state.registers[instruction.destination_register] =
                value == state.stack_values.end() ? returned_receiver_unknown()
                                                  : value->second;
            state.literal_registers[instruction.destination_register].reset();
        } else {
            clear_returned_receiver_register(
                state, instruction.destination_register);
            state.clear_stack();
        }
        if (instruction.destination_register == 15u) state.clear_stack();
        return true;
    }

    if (instruction.operation == Operation::LoadLongPcRelative) {
        if (decoded.instruction.kind != InstructionKind::MovLongLoadPcRelative ||
            instruction.destination_register >= 16u)
            return false;
        clear_returned_receiver_register(state,
                                         instruction.destination_register);
        if (instruction.effective_address.has_value()) {
            const auto literal = read_returned_receiver_pc_literal(
                image, *instruction.effective_address);
            if (literal.has_value())
                state.literal_registers[instruction.destination_register] =
                    ReturnedReceiverLiteral{*instruction.effective_address,
                                            *literal};
        }
        if (instruction.destination_register == 15u) state.clear_stack();
        return true;
    }

    const auto writes =
        katana::analysis::general_register_write_mask(decoded.instruction);
    for (std::uint8_t register_index = 0u; register_index < 16u;
         ++register_index) {
        if ((writes & static_cast<std::uint16_t>(1u << register_index)) != 0u)
            clear_returned_receiver_register(state, register_index);
    }
    if ((writes & static_cast<std::uint16_t>(1u << 15u)) != 0u ||
        is_memory_operation(instruction.operation))
        state.clear_stack();
    return true;
}


struct ReturnedReceiverWorkBudget final {
    std::size_t remaining = 0u;
    std::size_t consumed = 0u;
    bool exhausted = false;

    [[nodiscard]] bool charge(const std::size_t amount = 1u) noexcept {
        if (amount > remaining ||
            amount > std::numeric_limits<std::size_t>::max() - consumed) {
            exhausted = true;
            return false;
        }
        remaining -= amount;
        consumed += amount;
        return true;
    }
};

struct ReturnedReceiverDelayResult final {
    std::size_t next_instruction = 0u;
    ReturnedReceiverState state_after_delay;
};

[[nodiscard]] std::optional<std::uint32_t>
resolve_returned_receiver_call_target(
    const katana::sh4::DisassemblyLine& decoded,
    const katana::ir::Instruction& instruction,
    const ReturnedReceiverState& state) noexcept {
    using ControlFlow = katana::sh4::ControlFlowKind;
    using InstructionKind = katana::sh4::InstructionKind;
    using Operation = katana::ir::Operation;

    if (instruction.operation == Operation::Call) {
        if (decoded.instruction.kind != InstructionKind::Bsr ||
            decoded.instruction.control_flow != ControlFlow::Call ||
            !instruction.target_address.has_value() ||
            (*instruction.target_address & 1u) != 0u)
            return std::nullopt;
        return instruction.target_address;
    }
    if (instruction.operation != Operation::CallRegister ||
        decoded.instruction.kind != InstructionKind::Jsr ||
        decoded.instruction.control_flow != ControlFlow::IndirectCall ||
        instruction.branch_register_relative ||
        instruction.branch_register >= state.literal_registers.size())
        return std::nullopt;

    // The branch register is read before the delay slot executes.  The
    // literal lane was populated only by an identity-bound immutable
    // PC-relative load and is therefore safe to latch here.  The target may
    // be an external primary-image function; this diagnostic never
    // authorizes its execution.
    const auto& literal = state.literal_registers[instruction.branch_register];
    if (!literal.has_value() || (literal->literal_value & 1u) != 0u)
        return std::nullopt;
    return literal->literal_value;
}

[[nodiscard]] ReturnedReceiverState returned_receiver_state_after_call(
    const katana::io::ExecutableImage& image,
    const ReturnedReceiverState& state_after_delay,
    const std::uint32_t call_instruction_address,
    const std::optional<std::uint32_t> callee) {
    ReturnedReceiverState result;
    // Calls invalidate the caller's stack identity.  Preserve only the
    // callee-saved GPR lane covered by the authenticated SuperH ABI; an
    // unknown ABI must not turn an observed r14 into a guessed preservation.
    result.clear_stack();
    if (image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC) {
        for (std::uint8_t index = 8u; index <= 14u; ++index) {
            result.registers[index] = state_after_delay.registers[index];
            result.literal_registers[index] =
                state_after_delay.literal_registers[index];
        }
    }
    if (image.guest_call_abi() == katana::io::GuestCallAbi::SuperHC &&
        callee.has_value())
        result.registers[0u] = returned_receiver_origin(
            ReturnedReceiverOrigin{call_instruction_address, *callee});
    return result;
}

[[nodiscard]] std::optional<ReturnedReceiverDelayResult>
consume_returned_receiver_delay(
    const katana::ir::BasicBlock& block,
    const std::size_t instruction_index,
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& decoded,
    const katana::ir::Instruction& instruction,
    const ReturnedReceiverState& state,
    const ReturnedDecodedLineIndex& decoded_index,
    ReturnedReceiverWorkBudget& work_budget) {
    const bool has_delay = decoded.instruction.has_delay_slot ||
                           instruction.delay_slot.role ==
                               katana::ir::DelaySlotRole::Owner;
    if (!has_delay) {
        if (decoded.is_delay_slot ||
            instruction.delay_slot.role != katana::ir::DelaySlotRole::None)
            return std::nullopt;
        return ReturnedReceiverDelayResult{instruction_index + 1u, state};
    }

    if (instruction_index + 1u >= block.instructions.size())
        return std::nullopt;
    if (!work_budget.charge()) return std::nullopt;
    const auto& slot = block.instructions[instruction_index + 1u];
    const auto* decoded_slot =
        returned_decoded_line_at(decoded_index, slot.source_address);
    if (slot.source_address != instruction.source_address + 2u ||
        instruction.delay_slot.role != katana::ir::DelaySlotRole::Owner ||
        !instruction.delay_slot.counterpart_address.has_value() ||
        *instruction.delay_slot.counterpart_address != slot.source_address ||
        slot.delay_slot.role != katana::ir::DelaySlotRole::Slot ||
        !slot.delay_slot.counterpart_address.has_value() ||
        *slot.delay_slot.counterpart_address != instruction.source_address ||
        decoded_slot == nullptr || !decoded_slot->instruction.is_known() ||
        !decoded_slot->is_delay_slot ||
        decoded_slot->instruction.changes_control_flow())
        return std::nullopt;

    ReturnedReceiverState state_after_delay = state;
    if (!apply_returned_receiver_noncontrol(state_after_delay, image,
                                            *decoded_slot, slot))
        return std::nullopt;
    return ReturnedReceiverDelayResult{instruction_index + 2u,
                                      std::move(state_after_delay)};
}

struct ReturnedReceiverFieldObservation final {
    bool saw = false;
    bool missing = false;
    bool conflict = false;
    std::optional<ReturnedReceiverOrigin> origin;
    bool optional_null = false;
};

void observe_returned_receiver_field(
    ReturnedReceiverFieldObservation& observation,
    const StaticCallbackFieldSinkContract& field,
    const ReturnedReceiverState& state) {
    observation.saw = true;
    bool saw_origin = false;
    for (std::uint8_t bit = 0u; bit < 4u; ++bit) {
        if ((field.receiver_argument_mask &
             static_cast<std::uint8_t>(1u << bit)) == 0u)
            continue;
        const auto& value = state.registers[4u + bit];
        if (value.unknown || !value.origin.has_value()) continue;
        if (observation.origin.has_value() &&
            *observation.origin != *value.origin)
            observation.conflict = true;
        if (!observation.origin.has_value()) observation.origin = value.origin;
        observation.optional_null = observation.optional_null ||
                                    value.may_be_null;
        saw_origin = true;
    }
    if (!saw_origin) observation.missing = true;
}

[[nodiscard]] bool matches_returned_receiver_field(
    const StaticCallbackFieldSinkContract& field,
    const katana::ir::Function& function,
    const katana::ir::Instruction& instruction,
    const katana::sh4::DisassemblyLine& decoded) noexcept {
    if (field.function_address != function.entry_address ||
        field.call_instruction_address != instruction.source_address ||
        field.receiver_argument_mask == 0u ||
        (field.receiver_argument_mask & 0xf0u) != 0u)
        return false;
    if (field.call) {
        return instruction.operation == katana::ir::Operation::CallRegister &&
               decoded.instruction.control_flow ==
                   katana::sh4::ControlFlowKind::IndirectCall;
    }
    return instruction.operation == katana::ir::Operation::JumpRegister &&
           decoded.instruction.control_flow ==
               katana::sh4::ControlFlowKind::IndirectBranch;
}

struct ReturnedReceiverFunctionResult final {
    bool complete = true;
    bool truncated = false;
    std::size_t work_items = 0u;
    bool saw_return = false;
    ReturnedReceiverValue return_value;
    bool return_origin_conflict = false;
    bool saw_unresolved_call_target = false;
    bool saw_unknown_call_abi = false;
    std::uint32_t unresolved_call_block_address = 0u;
    std::uint32_t unresolved_call_instruction_address = 0u;
    std::uint32_t unknown_call_abi_block_address = 0u;
    std::uint32_t unknown_call_abi_instruction_address = 0u;
    std::uint32_t return_value_block_address = 0u;
    std::uint32_t return_value_instruction_address = 0u;
    StaticReturnedReceiverRejectionReason rejection_reason =
        StaticReturnedReceiverRejectionReason::None;
    std::uint32_t rejection_block_address = 0u;
    std::uint32_t rejection_instruction_address = 0u;
    std::vector<ReturnedReceiverFieldObservation> field_observations;
};

void note_returned_receiver_rejection(
    ReturnedReceiverFunctionResult& result,
    const StaticReturnedReceiverRejectionReason reason,
    const std::uint32_t block_address = 0u,
    const std::uint32_t instruction_address = 0u) noexcept {
    if (result.rejection_reason !=
        StaticReturnedReceiverRejectionReason::None)
        return;
    result.rejection_reason = reason;
    result.rejection_block_address = block_address;
    result.rejection_instruction_address = instruction_address;
}

[[nodiscard]] ReturnedReceiverFunctionResult analyze_returned_receiver_function(
    const katana::io::ExecutableImage& image,
    const katana::ir::Function& function,
    const ReturnedDecodedLineIndex& decoded_index,
    const std::span<const StaticCallbackFieldSinkContract> field_contracts,
    ReturnedReceiverWorkBudget& work_budget) {
    ReturnedReceiverFunctionResult result;
    const auto work_start = work_budget.consumed;
    const auto finish = [&]() {
        result.work_items = work_budget.consumed - work_start;
        return result;
    };
    result.field_observations.resize(field_contracts.size());

    std::unordered_map<std::uint32_t, std::size_t> block_indices;
    block_indices.reserve(function.blocks.size());
    for (std::size_t index = 0u; index < function.blocks.size(); ++index) {
        const auto [iterator, inserted] =
            block_indices.emplace(function.blocks[index].start_address, index);
        if (!inserted) {
            result.complete = false;
            note_returned_receiver_rejection(
                result, StaticReturnedReceiverRejectionReason::DuplicateBlock,
                function.blocks[index].start_address);
        }
        static_cast<void>(iterator);
    }
    const auto entry = block_indices.find(function.entry_address);
    if (entry == block_indices.end()) {
        result.complete = false;
        note_returned_receiver_rejection(
            result, StaticReturnedReceiverRejectionReason::MissingEntryBlock,
            function.entry_address);
        return finish();
    }

    for (const auto& block : function.blocks) {
        if (block.instructions.empty()) {
            result.complete = false;
            note_returned_receiver_rejection(
                result, StaticReturnedReceiverRejectionReason::EmptyBlock,
                block.start_address);
            continue;
        }
        if (block.instructions.front().source_address != block.start_address) {
            result.complete = false;
            note_returned_receiver_rejection(
                result,
                StaticReturnedReceiverRejectionReason::BlockStartMismatch,
                block.start_address, block.instructions.front().source_address);
        }
        std::optional<std::uint32_t> previous_address;
        for (const auto& instruction : block.instructions) {
            if (previous_address.has_value() &&
                instruction.source_address != *previous_address + 2u) {
                result.complete = false;
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::InstructionAddressGap,
                    block.start_address, instruction.source_address);
            }
            previous_address = instruction.source_address;
            const auto* decoded = returned_decoded_line_at(
                decoded_index, instruction.source_address);
            if (decoded == nullptr) {
                result.complete = false;
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::
                        DecodedInstructionMissing,
                    block.start_address, instruction.source_address);
            } else if (!decoded->instruction.is_known()) {
                result.complete = false;
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::
                        DecodedInstructionUnknown,
                    block.start_address, instruction.source_address);
            } else if (decoded->is_delay_slot !=
                       (instruction.delay_slot.role ==
                        katana::ir::DelaySlotRole::Slot)) {
                result.complete = false;
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::DelaySlotMismatch,
                    block.start_address, instruction.source_address);
            }
        }
        for (const auto successor : block.successors) {
            if (!block_indices.contains(successor)) {
                result.complete = false;
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::SuccessorMissing,
                    block.start_address);
            }
        }
    }
    if (!result.complete) return finish();

    std::vector<std::optional<ReturnedReceiverState>> incoming(
        function.blocks.size());
    std::deque<std::size_t> worklist;
    incoming[entry->second] = ReturnedReceiverState{};
    worklist.push_back(entry->second);
    std::vector<bool> reached(function.blocks.size(), false);
    const auto enqueue = [&](const std::uint32_t address,
                             const ReturnedReceiverState& state) -> bool {
        const auto iterator = block_indices.find(address);
        if (iterator == block_indices.end()) return false;
        auto& destination = incoming[iterator->second];
        if (!destination.has_value()) {
            destination = state;
            worklist.push_back(iterator->second);
            return true;
        }
        if (merge_returned_receiver_state(*destination, state)) {
            worklist.push_back(iterator->second);
        }
        return true;
    };

    while (!worklist.empty()) {
        if (!work_budget.charge()) {
            result.complete = false;
            result.truncated = true;
            note_returned_receiver_rejection(
                result, StaticReturnedReceiverRejectionReason::WorkBudget,
                function.blocks[worklist.front()].start_address);
            break;
        }
        const auto block_index = worklist.front();
        worklist.pop_front();
        if (!incoming[block_index].has_value()) {
            result.complete = false;
            note_returned_receiver_rejection(
                result,
                StaticReturnedReceiverRejectionReason::MissingIncomingState,
                function.blocks[block_index].start_address);
            continue;
        }
        reached[block_index] = true;
        const auto& block = function.blocks[block_index];
        ReturnedReceiverState state = *incoming[block_index];
        bool terminated = false;
        bool block_valid = true;

        for (std::size_t index = 0u; index < block.instructions.size();) {
            if (!work_budget.charge()) {
                result.complete = false;
                result.truncated = true;
                note_returned_receiver_rejection(
                    result, StaticReturnedReceiverRejectionReason::WorkBudget,
                    block.start_address, block.instructions[index].source_address);
                block_valid = false;
                break;
            }
            const auto& instruction = block.instructions[index];
            const auto* decoded = returned_decoded_line_at(
                decoded_index, instruction.source_address);
            if (decoded == nullptr) {
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::
                        DecodedInstructionMissing,
                    block.start_address, instruction.source_address);
                block_valid = false;
                break;
            }
            if (!decoded->instruction.is_known()) {
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::
                        DecodedInstructionUnknown,
                    block.start_address, instruction.source_address);
                block_valid = false;
                break;
            }
            if (decoded->is_delay_slot ||
                instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot) {
                note_returned_receiver_rejection(
                    result, StaticReturnedReceiverRejectionReason::
                                DelaySlotMismatch,
                    block.start_address, instruction.source_address);
                block_valid = false;
                break;
            }

            using Operation = katana::ir::Operation;
            using ControlFlow = katana::sh4::ControlFlowKind;
            if (instruction.operation == Operation::Call ||
                instruction.operation == Operation::CallRegister ||
                instruction.operation == Operation::JumpRegister) {
                const bool direct_call = instruction.operation == Operation::Call;
                const bool indirect_call =
                    instruction.operation == Operation::CallRegister;
                const bool literal_call =
                    indirect_call &&
                    decoded->instruction.kind ==
                        katana::sh4::InstructionKind::Jsr;
                if (direct_call) {
                    if (decoded->instruction.kind !=
                            katana::sh4::InstructionKind::Bsr ||
                        decoded->instruction.control_flow != ControlFlow::Call ||
                        !instruction.target_address.has_value()) {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::InvalidCall,
                            block.start_address, instruction.source_address);
                        block_valid = false;
                        break;
                    }
                } else if (indirect_call) {
                    if (!literal_call || decoded->instruction.control_flow !=
                                             ControlFlow::IndirectCall) {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::InvalidCall,
                            block.start_address, instruction.source_address);
                        block_valid = false;
                        break;
                    }
                } else if (decoded->instruction.control_flow !=
                           ControlFlow::IndirectBranch) {
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::InvalidCall,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }

                const auto callee =
                    direct_call || literal_call
                        ? resolve_returned_receiver_call_target(
                              *decoded, instruction, state)
                        : std::optional<std::uint32_t>{};

                if ((direct_call || literal_call) && !callee.has_value()) {
                    result.saw_unresolved_call_target = true;
                    if (result.unresolved_call_instruction_address == 0u) {
                        result.unresolved_call_block_address =
                            block.start_address;
                        result.unresolved_call_instruction_address =
                            instruction.source_address;
                    }
                } else if ((direct_call || literal_call) &&
                           image.guest_call_abi() !=
                               katana::io::GuestCallAbi::SuperHC) {
                    result.saw_unknown_call_abi = true;
                    if (result.unknown_call_abi_instruction_address == 0u) {
                        result.unknown_call_abi_block_address =
                            block.start_address;
                        result.unknown_call_abi_instruction_address =
                            instruction.source_address;
                    }
                }

                const auto delayed = consume_returned_receiver_delay(
                    block, index, image, *decoded, instruction, state,
                    decoded_index, work_budget);
                if (!delayed.has_value()) {
                    if (work_budget.exhausted) {
                        result.truncated = true;
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::WorkBudget,
                            block.start_address, instruction.source_address);
                    } else {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::
                                InvalidDelaySlot,
                            block.start_address, instruction.source_address);
                    }
                    block_valid = false;
                    break;
                }

                if (literal_call ||
                    instruction.operation == Operation::JumpRegister) {
                    for (std::size_t field_index = 0u;
                         field_index < field_contracts.size(); ++field_index) {
                        if (matches_returned_receiver_field(
                                field_contracts[field_index], function,
                                instruction, *decoded))
                            observe_returned_receiver_field(
                                result.field_observations[field_index],
                                field_contracts[field_index],
                                delayed->state_after_delay);
                    }
                }

                if (instruction.operation == Operation::JumpRegister) {
                    // The callback field can still be inventoried at this
                    // exact tailcall, but the unknown jump edge prevents a
                    // complete return summary.
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::IndirectTailcall,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }

                state = returned_receiver_state_after_call(
                    image, delayed->state_after_delay,
                    instruction.source_address, callee);
                index = delayed->next_instruction;
                continue;
            }

            if (instruction.operation == Operation::Return) {
                if (decoded->instruction.control_flow != ControlFlow::Return ||
                    !block.successors.empty() || block.has_indirect_successor) {
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::InvalidReturn,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }
                const auto delayed = consume_returned_receiver_delay(
                    block, index, image, *decoded, instruction, state,
                    decoded_index, work_budget);
                if (!delayed.has_value()) {
                    if (work_budget.exhausted) {
                        result.truncated = true;
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::WorkBudget,
                            block.start_address, instruction.source_address);
                    } else {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::
                                InvalidDelaySlot,
                            block.start_address, instruction.source_address);
                    }
                    block_valid = false;
                    break;
                }
                if (delayed->next_instruction != block.instructions.size()) {
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::InvalidReturn,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }
                result.return_value_block_address = block.start_address;
                result.return_value_instruction_address =
                    instruction.source_address;
                const auto returned_value =
                    delayed->state_after_delay.registers[0u];
                if (!result.saw_return) {
                    result.return_value = returned_value;
                } else {
                    if (result.return_value.origin.has_value() &&
                        returned_value.origin.has_value() &&
                        *result.return_value.origin !=
                            *returned_value.origin)
                        result.return_origin_conflict = true;
                    result.return_value =
                        join_returned_receiver_values(result.return_value,
                                                      returned_value);
                }
                result.saw_return = true;
                terminated = true;
                index = delayed->next_instruction;
                break;
            }

            const auto direct_branch =
                instruction.operation == Operation::Branch;
            const auto conditional_branch =
                instruction.operation == Operation::BranchIfTrue ||
                instruction.operation == Operation::BranchIfFalse;
            if (direct_branch || conditional_branch) {
                const auto expected_control =
                    conditional_branch ? ControlFlow::ConditionalBranch
                                        : ControlFlow::UnconditionalBranch;
                if (decoded->instruction.control_flow != expected_control ||
                    !instruction.target_address.has_value() ||
                    !contains_successor(block, *instruction.target_address) ||
                    (conditional_branch && block.successors.size() != 2u) ||
                    (direct_branch && block.successors.size() != 1u) ||
                    block.has_indirect_successor) {
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::InvalidBranch,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }
                const auto delayed = consume_returned_receiver_delay(
                    block, index, image, *decoded, instruction, state,
                    decoded_index, work_budget);
                if (!delayed.has_value()) {
                    if (work_budget.exhausted) {
                        result.truncated = true;
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::WorkBudget,
                            block.start_address, instruction.source_address);
                    } else {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::
                                InvalidDelaySlot,
                            block.start_address, instruction.source_address);
                    }
                    block_valid = false;
                    break;
                }
                if (delayed->next_instruction != block.instructions.size()) {
                    note_returned_receiver_rejection(
                        result,
                        StaticReturnedReceiverRejectionReason::InvalidBranch,
                        block.start_address, instruction.source_address);
                    block_valid = false;
                    break;
                }
                for (const auto successor : block.successors) {
                    if (!enqueue(successor, delayed->state_after_delay)) {
                        note_returned_receiver_rejection(
                            result,
                            StaticReturnedReceiverRejectionReason::
                                SuccessorMissing,
                            block.start_address, instruction.source_address);
                        block_valid = false;
                        break;
                    }
                }
                terminated = true;
                break;
            }

            if (!apply_returned_receiver_noncontrol(state, image, *decoded,
                                                    instruction)) {
                note_returned_receiver_rejection(
                    result,
                    StaticReturnedReceiverRejectionReason::
                        UnsupportedInstruction,
                    block.start_address, instruction.source_address);
                block_valid = false;
                break;
            }
            ++index;
        }

        if (!block_valid) {
            result.complete = false;
            continue;
        }
        if (terminated) continue;
        if (block.has_indirect_successor || block.successors.size() != 1u) {
            note_returned_receiver_rejection(
                result,
                StaticReturnedReceiverRejectionReason::InvalidContinuation,
                block.start_address);
            result.complete = false;
            continue;
        }
        if (!enqueue(block.successors.front(), state)) {
            note_returned_receiver_rejection(
                result, StaticReturnedReceiverRejectionReason::SuccessorMissing,
                block.start_address);
            result.complete = false;
        }
    }

    const auto unreached =
        std::find(reached.begin(), reached.end(), false);
    if (unreached != reached.end()) {
        result.complete = false;
        const auto block_index = static_cast<std::size_t>(
            unreached - reached.begin());
        note_returned_receiver_rejection(
            result, StaticReturnedReceiverRejectionReason::UnreachedBlock,
            function.blocks[block_index].start_address);
    }
    return finish();
}

[[nodiscard]] StaticReturnedReceiverFunctionDiagnostic
make_returned_receiver_function_diagnostic(
    const katana::ir::Function& function,
    const std::size_t instruction_count,
    const ReturnedReceiverFunctionResult& result) noexcept {
    using Outcome = StaticReturnedReceiverFunctionOutcome;
    using Reason = StaticReturnedReceiverRejectionReason;

    Outcome outcome = Outcome::Incomplete;
    if (result.truncated) {
        outcome = Outcome::Budget;
    } else if (!result.complete) {
        outcome = Outcome::Incomplete;
    } else if (result.saw_return && !result.return_value.unknown &&
               result.return_value.origin.has_value()) {
        outcome = Outcome::Recognized;
    } else {
        outcome = Outcome::NoReturnOrigin;
    }

    auto reason = result.rejection_reason;
    auto rejection_block_address = result.rejection_block_address;
    auto rejection_instruction_address =
        result.rejection_instruction_address;
    if (reason == Reason::None) {
        if (outcome == Outcome::Budget) {
            reason = Reason::WorkBudget;
        } else if (outcome == Outcome::Incomplete) {
            reason = Reason::IncompleteAnalysis;
        } else if (outcome == Outcome::NoReturnOrigin) {
            if (!result.saw_return) {
                reason = Reason::NoReturn;
                rejection_block_address = function.entry_address;
            } else if (result.return_origin_conflict) {
                reason = Reason::ReturnOriginConflict;
                rejection_block_address = result.return_value_block_address;
                rejection_instruction_address =
                    result.return_value_instruction_address;
            } else if (!result.return_value.unknown &&
                       !result.return_value.origin.has_value() &&
                       result.return_value.may_be_null) {
                reason = Reason::NullOnlyReturn;
                rejection_block_address = result.return_value_block_address;
                rejection_instruction_address =
                    result.return_value_instruction_address;
            } else if (result.saw_unresolved_call_target) {
                reason = Reason::UnresolvedCallTarget;
                rejection_block_address = result.unresolved_call_block_address;
                rejection_instruction_address =
                    result.unresolved_call_instruction_address;
            } else if (result.saw_unknown_call_abi) {
                reason = Reason::UnknownCallAbi;
                rejection_block_address = result.unknown_call_abi_block_address;
                rejection_instruction_address =
                    result.unknown_call_abi_instruction_address;
            } else {
                reason = Reason::ReturnValueUnknown;
                rejection_block_address = result.return_value_block_address;
                rejection_instruction_address =
                    result.return_value_instruction_address;
            }
        }
    }

    return StaticReturnedReceiverFunctionDiagnostic{
        function.entry_address,
        true,
        instruction_count,
        result.work_items,
        outcome,
        reason,
        rejection_block_address,
        rejection_instruction_address};
}

} // namespace

StaticReturnedReceiverInventory discover_static_returned_receiver_contracts(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::ir::Function> program,
    const std::span<const katana::sh4::DisassemblyLine> decoded_lines,
    const std::span<const StaticCallbackFieldSinkContract> field_sink_contracts,
    const std::size_t maximum_work_items) {
    StaticReturnedReceiverInventory inventory;
    inventory.work_budget = maximum_work_items;
    ReturnedReceiverWorkBudget work_budget{maximum_work_items};
    ReturnedDecodedLineIndex decoded_index;
    for (const auto& line : decoded_lines) {
        const auto [iterator, inserted] =
            decoded_index.lines.emplace(line.address, &line);
        if (!inserted) {
            decoded_index.ambiguous.insert(line.address);
            static_cast<void>(iterator);
        }
    }

    // Partition the optional field contracts once by owning function.  The
    // returned-value summary itself does not need field contracts, and a
    // function must never allocate or scan the whole global field set.
    std::vector<StaticCallbackFieldSinkContract> sorted_field_contracts(
        field_sink_contracts.begin(), field_sink_contracts.end());
    std::sort(sorted_field_contracts.begin(), sorted_field_contracts.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.function_address,
                                  left.call_instruction_address,
                                  left.load_instruction_address,
                                  left.displacement, left.width, left.call,
                                  left.receiver_argument_mask) <
                         std::tie(right.function_address,
                                  right.call_instruction_address,
                                  right.load_instruction_address,
                                  right.displacement, right.width, right.call,
                                  right.receiver_argument_mask);
              });

    std::set<std::uint32_t> seen_functions;
    for (const auto& function : program) {
        if (!seen_functions.insert(function.entry_address).second) continue;
        std::size_t function_instruction_count = 0u;
        bool function_instruction_count_overflow = false;
        for (const auto& block : function.blocks) {
            if (block.instructions.size() >
                std::numeric_limits<std::size_t>::max() -
                    function_instruction_count) {
                function_instruction_count_overflow = true;
                break;
            }
            function_instruction_count += block.instructions.size();
        }
        if (function_instruction_count_overflow ||
            function_instruction_count >
                std::numeric_limits<std::size_t>::max() -
                    inventory.instructions_examined) {
            inventory.truncated = true;
            ++inventory.incomplete_functions;
            break;
        }
        ++inventory.functions_examined;
        inventory.instructions_examined += function_instruction_count;
        const auto field_begin = std::lower_bound(
            sorted_field_contracts.begin(), sorted_field_contracts.end(),
            function.entry_address,
            [](const auto& field, const std::uint32_t address) {
                return field.function_address < address;
            });
        const auto field_end = std::upper_bound(
            field_begin, sorted_field_contracts.end(), function.entry_address,
            [](const std::uint32_t address, const auto& field) {
                return address < field.function_address;
            });
        const std::span<const StaticCallbackFieldSinkContract> function_fields(
            field_begin == sorted_field_contracts.end()
                ? nullptr
                : &*field_begin,
            static_cast<std::size_t>(field_end - field_begin));
        const auto result = analyze_returned_receiver_function(
            image, function, decoded_index, function_fields, work_budget);
        inventory.work_items = work_budget.consumed;
        inventory.truncated = inventory.truncated || result.truncated;
        if (!result.complete) ++inventory.incomplete_functions;
        inventory.function_diagnostics.push_back(
            make_returned_receiver_function_diagnostic(
                function, function_instruction_count, result));
        if (result.truncated) break;

        if (result.complete && result.saw_return &&
            !result.return_value.unknown &&
            result.return_value.origin.has_value()) {
            inventory.returned_receivers.push_back(
                StaticReturnedReceiverContract{
                    function.entry_address,
                    *result.return_value.origin,
                    result.return_value.may_be_null,
                    true,
                    true,
                    false,
                    false});
        }

        for (std::size_t field_index = 0u;
             field_index < function_fields.size(); ++field_index) {
            const auto& observation = result.field_observations[field_index];
            if (!observation.saw || observation.missing || observation.conflict ||
                !observation.origin.has_value())
                continue;
            inventory.field_candidates.push_back(
                StaticReturnedReceiverFieldCandidate{
                    function_fields[field_index],
                    *observation.origin,
                    observation.optional_null,
                    result.complete,
                    true,
                    false,
                    false});
        }
    }

    std::sort(inventory.returned_receivers.begin(),
              inventory.returned_receivers.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.function_address,
                                  left.origin.call_instruction_address,
                                  left.origin.callee_address,
                                  left.optional_null) <
                         std::tie(right.function_address,
                                  right.origin.call_instruction_address,
                                  right.origin.callee_address,
                                  right.optional_null);
              });
    inventory.returned_receivers.erase(
        std::unique(inventory.returned_receivers.begin(),
                    inventory.returned_receivers.end()),
        inventory.returned_receivers.end());

    std::sort(
        inventory.function_diagnostics.begin(),
        inventory.function_diagnostics.end(),
        [](const auto& left, const auto& right) {
            return left.function_address < right.function_address;
        });

    std::sort(inventory.field_candidates.begin(), inventory.field_candidates.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.field.function_address,
                                  left.field.call_instruction_address,
                                  left.field.load_instruction_address,
                                  left.field.displacement,
                                  left.origin.call_instruction_address,
                                  left.origin.callee_address,
                                  left.optional_null) <
                         std::tie(right.field.function_address,
                                  right.field.call_instruction_address,
                                  right.field.load_instruction_address,
                                  right.field.displacement,
                                  right.origin.call_instruction_address,
                                  right.origin.callee_address,
                                  right.optional_null);
              });
    inventory.field_candidates.erase(
        std::unique(inventory.field_candidates.begin(),
                    inventory.field_candidates.end()),
        inventory.field_candidates.end());
    return inventory;
}


} // namespace katana::analysis::detail
