#include "return_object_callback_inventory.hpp"

#include "guarded_native_entry_shape.hpp"
#include "katana/analysis/value_analysis.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis::detail {
namespace {

using Candidate = katana::codegen::LatentAotReturnObjectCallbackCandidate;

struct ReturnOrigin final {
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t callee_address = 0u;
};

struct LiteralOrigin final {
    std::uint32_t literal_address = 0u;
    std::uint32_t literal_value = 0u;
};

struct RegisterState final {
    std::array<std::optional<ReturnOrigin>, 16u> returned_objects{};
    std::array<std::optional<LiteralOrigin>, 16u> literals{};
};

// Keep this local to the diagnostic TU.  The product latent-AOT resolver has
// the same direct-mapped spelling rules, but its internal helper is deliberately
// not part of the analysis ABI.  A JSR literal is useful here only when it is
// an aligned P1/P2 (or physical P0) main-RAM code spelling; no runtime memory
// lookup or executable graph edge is created by this normalization.
std::optional<std::uint32_t> canonical_direct_code_address(
    const std::uint32_t value) noexcept {
    const auto segment = value >> 29u;
    if (segment == 4u || segment == 5u)
        return (value & 0x1fffffffu) | 0x80000000u;
    if (segment == 0u && value >= 0x08000000u && value < 0x10000000u)
        return value | 0x80000000u;
    return std::nullopt;
}

struct DecodedLineIndex final {
    std::unordered_map<std::uint32_t, const katana::sh4::DisassemblyLine*>
        lines;
    std::unordered_set<std::uint32_t> ambiguous;
};

const katana::sh4::DisassemblyLine* decoded_line_at(
    const DecodedLineIndex& index, const std::uint32_t address) noexcept {
    if (index.ambiguous.contains(address)) return nullptr;
    const auto iterator = index.lines.find(address);
    return iterator == index.lines.end() ? nullptr : iterator->second;
}

std::optional<std::uint32_t> read_identity_bound_literal(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) {
    const auto resolved = image.resolve_segment_address(address, 4u);
    if (!resolved.has_value() ||
        image.find_immutable_range(*resolved, 4u) == nullptr)
        return std::nullopt;
    const auto* segment = image.find_segment(*resolved, 4u);
    if (segment == nullptr || !segment->permissions.readable)
        return std::nullopt;
    const auto offset = segment->byte_offset(*resolved);
    if (!offset.has_value() || *offset > segment->bytes.size() ||
        segment->bytes.size() - *offset < 4u)
        return std::nullopt;
    return image.read_u32_le(*resolved);
}

void clear_register_mask(RegisterState& state,
                         const std::uint16_t write_mask) noexcept {
    for (std::uint8_t register_index = 0u; register_index < 16u;
         ++register_index) {
        if ((write_mask &
             static_cast<std::uint16_t>(1u << register_index)) == 0u)
            continue;
        state.returned_objects[register_index].reset();
        state.literals[register_index].reset();
    }
}

void copy_register_provenance(RegisterState& state,
                              const std::uint8_t source_register,
                              const std::uint8_t destination_register) {
    if (source_register >= 16u || destination_register >= 16u) return;
    const auto returned_object = state.returned_objects[source_register];
    const auto literal = state.literals[source_register];
    state.returned_objects[destination_register] = returned_object;
    state.literals[destination_register] = literal;
}

// Advance only the non-control-flow part of the bounded provenance.  The
// caller handles stores separately so it can observe the returned-object /
// callback pair before applying the instruction's normal GPR kill mask.
bool apply_register_provenance(
    RegisterState& state,
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& decoded,
    const katana::ir::Instruction& instruction) {
    if (!decoded.instruction.is_known() ||
        decoded.instruction.changes_control_flow())
        return false;

    if (instruction.operation == katana::ir::Operation::FmovRegister)
        return true; // FPU lanes do not clobber same-numbered GPRs.

    if (instruction.operation == katana::ir::Operation::MovRegister) {
        copy_register_provenance(
            state, instruction.source_register, instruction.destination_register);
        return true;
    }

    if (instruction.operation == katana::ir::Operation::
            LoadLongPcRelative) {
        const auto destination = instruction.destination_register;
        if (destination >= 16u) return false;
        state.returned_objects[destination].reset();
        state.literals[destination].reset();
        if (!instruction.effective_address.has_value()) return true;
        const auto value = read_identity_bound_literal(
            image, *instruction.effective_address);
        if (value.has_value())
            state.literals[destination] = LiteralOrigin{
                *instruction.effective_address, *value};
        return true;
    }

    // Stores do not write GPR values, but they are still a known, non-control
    // instruction.  Let the complete decoder-derived write mask below handle
    // unusual addressing forms (including stack updates).
    clear_register_mask(
        state,
        katana::analysis::general_register_write_mask(decoded.instruction));
    return true;
}

std::optional<std::uint32_t> resolve_call_target(
    const katana::ir::Instruction& call,
    const RegisterState& state) noexcept {
    if (call.operation == katana::ir::Operation::Call) {
        if (!call.target_address.has_value()) return std::nullopt;
        return call.target_address;
    }
    if (call.operation != katana::ir::Operation::CallRegister ||
        call.branch_register_relative || call.branch_register >= 16u)
        return std::nullopt;
    const auto& literal = state.literals[call.branch_register];
    if (!literal.has_value()) return std::nullopt;
    return canonical_direct_code_address(literal->literal_value);
}

std::optional<std::size_t> unique_block_index(
    const std::unordered_map<std::uint32_t, std::size_t>& block_indices,
    const std::uint32_t address) noexcept {
    const auto iterator = block_indices.find(address);
    if (iterator == block_indices.end()) return std::nullopt;
    return iterator->second;
}

bool contains_successor(const katana::ir::BasicBlock& block,
                        const std::uint32_t address) noexcept {
    return std::find(block.successors.begin(), block.successors.end(), address) !=
           block.successors.end();
}

bool is_terminal_return_guard_target(
    const katana::ir::BasicBlock& block,
    const DecodedLineIndex& decoded_index) {
    if (!block.successors.empty() || block.has_indirect_successor)
        return false;
    bool saw_return = false;
    for (const auto& instruction : block.instructions) {
        const auto* decoded = decoded_line_at(decoded_index,
                                               instruction.source_address);
        if (decoded == nullptr || !decoded->instruction.is_known())
            return false;
        if (instruction.operation == katana::ir::Operation::Return) {
            if (saw_return) return false;
            saw_return = true;
            continue;
        }
        if (saw_return) {
            if (!decoded->is_delay_slot) return false;
            continue;
        }
        if (decoded->is_delay_slot ||
            decoded->instruction.changes_control_flow() ||
            instruction.operation == katana::ir::Operation::
                StoreLongDisplacement)
            return false;
    }
    return saw_return;
}

std::optional<std::size_t> guarded_branch_fallthrough(
    const katana::ir::Function& function,
    const katana::ir::BasicBlock& block,
    const katana::ir::Instruction& branch,
    const std::unordered_map<std::uint32_t, std::size_t>& block_indices,
    const std::vector<std::size_t>& predecessor_counts,
    const std::unordered_map<std::uint32_t, const katana::ir::BasicBlock*>&
        blocks_by_address,
    const DecodedLineIndex& decoded_index) {
    if ((branch.operation != katana::ir::Operation::BranchIfTrue &&
         branch.operation != katana::ir::Operation::BranchIfFalse) ||
        !branch.target_address.has_value() ||
        block.instructions.empty() ||
        block.instructions.back().source_address != branch.source_address)
        return std::nullopt;
    const auto* decoded = decoded_line_at(decoded_index, branch.source_address);
    if (decoded == nullptr || decoded->is_delay_slot ||
        decoded->instruction.control_flow !=
            katana::sh4::ControlFlowKind::ConditionalBranch ||
        decoded->instruction.has_delay_slot || block.successors.size() != 2u)
        return std::nullopt;

    const auto target_index = unique_block_index(
        block_indices, *branch.target_address);
    // The cleanup/return block may also be reached by the ordinary
    // fallthrough path (the Dash producer joins there).  It is safe to use
    // as a guard sink because we never propagate provenance through it; the
    // candidate path itself remains the uniquely predecessor-owned
    // fallthrough below.
    if (!target_index.has_value() || *target_index >= function.blocks.size() ||
        predecessor_counts[*target_index] == 0u ||
        !is_terminal_return_guard_target(
            function.blocks[*target_index], decoded_index))
        return std::nullopt;

    std::optional<std::uint32_t> fallthrough_address;
    for (const auto successor : block.successors) {
        if (successor == *branch.target_address) continue;
        if (fallthrough_address.has_value()) return std::nullopt;
        fallthrough_address = successor;
    }
    if (!fallthrough_address.has_value()) return std::nullopt;
    const auto fallthrough_block = blocks_by_address.find(*fallthrough_address);
    if (fallthrough_block == blocks_by_address.end() ||
        fallthrough_block->second == nullptr)
        return std::nullopt;
    const auto fallthrough_index = unique_block_index(
        block_indices, *fallthrough_address);
    if (!fallthrough_index.has_value() ||
        predecessor_counts[*fallthrough_index] != 1u)
        return std::nullopt;
    return fallthrough_index;
}

std::optional<RegisterState> register_state_before_call(
    const katana::io::ExecutableImage& image,
    const katana::ir::Function& function,
    const katana::ir::BasicBlock& block,
    const std::size_t block_index,
    const std::size_t predecessor_count,
    const std::size_t call_index,
    const DecodedLineIndex& decoded_index) {
    // A zero-predecessor block is accepted only as the function entry.  Any
    // non-entry block must have exactly one direct predecessor; joins and
    // missing/indirect ingress are intentionally not guessed through.
    if (predecessor_count > 1u ||
        (predecessor_count == 0u &&
         (block_index != 0u || block.start_address != function.entry_address)))
        return std::nullopt;

    RegisterState state;
    std::optional<std::uint32_t> previous_address;
    for (std::size_t index = 0u; index < call_index; ++index) {
        const auto& instruction = block.instructions[index];
        if (previous_address.has_value() &&
            instruction.source_address != *previous_address + 2u)
            return std::nullopt;
        previous_address = instruction.source_address;
        const auto* decoded = decoded_line_at(decoded_index,
                                               instruction.source_address);
        if (decoded == nullptr || decoded->is_delay_slot ||
            !apply_register_provenance(state, image, *decoded, instruction))
            return std::nullopt;
    }
    return state;
}

using CandidateKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::int32_t, std::uint32_t>;

CandidateKey candidate_key(const Candidate& candidate) noexcept {
    return {candidate.function_address,
            candidate.block_address,
            candidate.call_instruction_address,
            candidate.callee_address,
            candidate.literal_address,
            candidate.literal_value,
            candidate.store_instruction_address,
            candidate.field_displacement,
            candidate.target_address};
}

void append_candidate(std::vector<Candidate>& output,
                      const katana::ir::Function& function,
                      const katana::ir::BasicBlock& block,
                      const katana::ir::Instruction& call,
                      const std::uint8_t object_register,
                      const LiteralOrigin& literal,
                      const katana::ir::Instruction& store,
                      GuardedNativeEntryShapeCache& native_entry_shapes) {
    // r15 is the architectural stack pointer.  A store through it is a real
    // stack write, not evidence of an object returned by the callee.
    if (object_register == 15u) return;

    if (!call.target_address.has_value()) return;
    const auto raw_target = literal.literal_value;
    if (native_entry_shapes.classify(raw_target) !=
        GuardedNativeEntryShapeStatus::Valid)
        return;
    const auto canonical_target = native_entry_shapes.canonical_address(raw_target);
    if (!canonical_target.has_value()) return;

    Candidate candidate;
    candidate.function_address = function.entry_address;
    candidate.block_address = block.start_address;
    candidate.call_instruction_address = call.source_address;
    candidate.callee_address = *call.target_address;
    candidate.return_register = 0u;
    candidate.object_register = object_register;
    candidate.literal_address = literal.literal_address;
    candidate.literal_value = literal.literal_value;
    candidate.store_instruction_address = store.source_address;
    candidate.field_displacement = store.displacement;
    candidate.target_address = *canonical_target;
    candidate.target_shape_valid = true;
    // A direct return value is not a proof of persistent allocation or of the
    // receiver ABI; these values are fixed false by contract.
    candidate.complete = false;
    candidate.strict_eligible = false;
    candidate.execution_eligible = false;
    candidate.reason = "return-object-alias-unproven";
    output.push_back(std::move(candidate));
}

bool scan_straight_line(
    std::vector<Candidate>& output,
    const katana::io::ExecutableImage& image,
    const katana::ir::Function& function,
    const std::vector<std::size_t>& block_order,
    const std::vector<std::size_t>& predecessor_counts,
    const std::unordered_map<std::uint32_t, std::size_t>& block_indices,
    const std::unordered_map<std::uint32_t, const katana::ir::BasicBlock*>&
        blocks_by_address,
    const DecodedLineIndex& decoded_index,
    const std::size_t initial_block_index,
    std::size_t instruction_index,
    RegisterState state,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    std::unordered_set<std::size_t> visited_blocks;
    std::size_t block_position = 0u;
    for (; block_position < block_order.size(); ++block_position) {
        if (block_order[block_position] == initial_block_index) break;
    }
    if (block_position == block_order.size()) return false;

    while (block_position < block_order.size()) {
        const auto block_index = block_order[block_position];
        if (!visited_blocks.insert(block_index).second) break;
        if (block_index >= function.blocks.size()) break;
        if (block_index != initial_block_index &&
            predecessor_counts[block_index] != 1u)
            break;
        const auto& block = function.blocks[block_index];
        if (instruction_index > block.instructions.size()) break;

        bool control_flow_seen = false;
        std::optional<std::size_t> guarded_fallthrough_index;
        for (std::size_t index = instruction_index;
             index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            const auto* decoded = decoded_line_at(decoded_index,
                                                   instruction.source_address);
            if (decoded == nullptr || !decoded->instruction.is_known())
                return false;
            if (decoded->is_delay_slot) return false;

            // A second call or any branch ends this straight-line proof.  In
            // particular, an indirect call cannot carry the direct r0 origin
            // through an unknown callee.
            if (instruction.operation == katana::ir::Operation::Call ||
                decoded->instruction.changes_control_flow()) {
                if (const auto fallthrough = guarded_branch_fallthrough(
                        function, block, instruction, block_indices,
                        predecessor_counts, blocks_by_address, decoded_index);
                    fallthrough.has_value()) {
                    guarded_fallthrough_index = fallthrough;
                    break;
                }
                control_flow_seen = true;
                break;
            }

            if (instruction.operation == katana::ir::Operation::FmovRegister)
                continue; // FPU lanes do not clobber same-numbered GPRs.

            if (instruction.operation == katana::ir::Operation::MovRegister) {
                copy_register_provenance(
                    state, instruction.source_register,
                    instruction.destination_register);
                continue;
            }

            if (instruction.operation == katana::ir::Operation::
                    LoadLongPcRelative) {
                const auto destination = instruction.destination_register;
                if (destination >= 16u) continue;
                state.returned_objects[destination].reset();
                state.literals[destination].reset();
                if (!instruction.effective_address.has_value()) continue;
                const auto value = read_identity_bound_literal(
                    image, *instruction.effective_address);
                if (value.has_value())
                    state.literals[destination] = LiteralOrigin{
                        *instruction.effective_address, *value};
                continue;
            }

            if (instruction.operation == katana::ir::Operation::
                    StoreLongDisplacement) {
                const auto object_register = instruction.destination_register;
                const auto literal_register = instruction.source_register;
                if (object_register < 16u && literal_register < 16u &&
                    state.returned_objects[object_register].has_value() &&
                    state.literals[literal_register].has_value()) {
                    const auto& returned =
                        *state.returned_objects[object_register];
                    const auto& literal = *state.literals[literal_register];
                    // The return origin is read before any later GPR writes;
                    // this store itself has no register writeback.
                    katana::ir::Instruction call;
                    call.source_address = returned.call_instruction_address;
                    call.target_address = returned.callee_address;
                    append_candidate(output, function, block, call,
                                     object_register, literal, instruction,
                                     native_entry_shapes);
                }
                continue;
            }

            clear_register_mask(
                state,
                katana::analysis::general_register_write_mask(
                    decoded->instruction));
        }

        if (guarded_fallthrough_index.has_value()) {
            std::size_t successor_position = block_order.size();
            for (std::size_t position = 0u; position < block_order.size();
                 ++position) {
                if (block_order[position] == *guarded_fallthrough_index) {
                    successor_position = position;
                    break;
                }
            }
            if (successor_position == block_order.size()) break;
            instruction_index = 0u;
            block_position = successor_position;
            continue;
        }
        if (control_flow_seen) break;
        if (block.successors.size() != 1u ||
            block.has_indirect_successor)
            break;
        const auto successor_address = block.successors.front();
        const auto successor_iterator = blocks_by_address.find(successor_address);
        if (successor_iterator == blocks_by_address.end()) break;
        const auto successor_block = successor_iterator->second;
        if (successor_block == nullptr) break;
        std::size_t successor_index = function.blocks.size();
        for (std::size_t index = 0u; index < function.blocks.size(); ++index) {
            if (&function.blocks[index] == successor_block) {
                successor_index = index;
                break;
            }
        }
        if (successor_index == function.blocks.size() ||
            predecessor_counts[successor_index] != 1u)
            break;
        instruction_index = 0u;
        std::size_t successor_position = block_order.size();
        for (std::size_t position = 0u; position < block_order.size();
             ++position) {
            if (block_order[position] == successor_index) {
                successor_position = position;
                break;
            }
        }
        if (successor_position == block_order.size()) break;
        block_position = successor_position;
    }
    return true;
}

} // namespace

std::vector<Candidate> discover_return_object_callback_candidates(
    const katana::io::ExecutableImage& image,
    const std::span<const katana::sh4::DisassemblyLine> decoded_lines,
    const std::span<const katana::ir::Function> program,
    GuardedNativeEntryShapeCache& native_entry_shapes) {
    // The index is built from CFA's already decoded lines; this detector never
    // decodes a second copy of the module.
    DecodedLineIndex index;
    index.lines.reserve(decoded_lines.size());
    for (const auto& line : decoded_lines) {
        const auto [iterator, inserted] = index.lines.emplace(line.address,
                                                                &line);
        if (!inserted &&
            (iterator->second->opcode != line.opcode ||
             iterator->second->instruction.kind != line.instruction.kind))
            index.ambiguous.insert(line.address);
    }

    std::vector<Candidate> candidates;
    for (const auto& function : program) {
        if (function.blocks.empty()) continue;
        std::unordered_map<std::uint32_t, std::size_t> block_indices;
        std::unordered_map<std::uint32_t,
                           const katana::ir::BasicBlock*> blocks_by_address;
        block_indices.reserve(function.blocks.size());
        blocks_by_address.reserve(function.blocks.size());
        bool duplicate_block = false;
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            const auto& block = function.blocks[block_index];
            if (!block_indices.emplace(block.start_address, block_index).second)
                duplicate_block = true;
            blocks_by_address.emplace(block.start_address, &block);
        }
        if (duplicate_block) continue;

        std::vector<std::size_t> predecessor_counts(function.blocks.size(), 0u);
        for (const auto& block : function.blocks) {
            for (const auto successor : block.successors) {
                const auto iterator = block_indices.find(successor);
                if (iterator != block_indices.end())
                    ++predecessor_counts[iterator->second];
            }
        }
        std::vector<std::size_t> block_order(function.blocks.size());
        for (std::size_t index_value = 0u;
             index_value < function.blocks.size(); ++index_value)
            block_order[index_value] = index_value;

        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            const auto& block = function.blocks[block_index];
            for (std::size_t call_index = 0u;
                 call_index < block.instructions.size(); ++call_index) {
                const auto& call = block.instructions[call_index];
                const bool direct_call =
                    call.operation == katana::ir::Operation::Call;
                const bool literal_call =
                    call.operation == katana::ir::Operation::CallRegister;
                if (!direct_call && !literal_call)
                    continue;
                const auto* decoded_call = decoded_line_at(
                    index, call.source_address);
                if (decoded_call == nullptr ||
                    !decoded_call->instruction.is_known() ||
                    !decoded_call->instruction.has_delay_slot ||
                    (direct_call &&
                     decoded_call->instruction.control_flow !=
                         katana::sh4::ControlFlowKind::Call) ||
                    (literal_call &&
                     (decoded_call->instruction.control_flow !=
                          katana::sh4::ControlFlowKind::IndirectCall ||
                      call.branch_register_relative ||
                      call.branch_register >= 16u)))
                    continue;

                // Resolve a literal JSR before its architectural delay slot
                // executes.  The direct BSR path keeps its existing IR target;
                // the CallRegister path is accepted only when the branch GPR
                // has one immutable PC-literal origin on this unique prefix.
                RegisterState state_before_call;
                if (literal_call) {
                    const auto state = register_state_before_call(
                        image, function, block, block_index,
                        predecessor_counts[block_index], call_index, index);
                    if (!state.has_value()) continue;
                    state_before_call = *state;
                }
                const auto callee_address = resolve_call_target(
                    call, state_before_call);
                if (!callee_address.has_value()) continue;

                std::size_t first_instruction = call_index + 1u;
                if (first_instruction >= block.instructions.size() ||
                    call.delay_slot.role != katana::ir::DelaySlotRole::Owner ||
                    block.instructions[first_instruction].source_address !=
                        call.source_address + 2u ||
                    !block.instructions[first_instruction]
                         .delay_slot.counterpart_address.has_value() ||
                    block.instructions[first_instruction].delay_slot.role !=
                        katana::ir::DelaySlotRole::Slot) {
                    // The lowerer normally keeps the owner and slot together;
                    // if that architectural ordering is absent, stop rather
                    // than treating the slot as post-return code.
                    continue;
                }

                // The delay slot executes architecturally before the callee
                // returns.  Validate it against the pre-call state only and
                // discard that state afterwards: an unknown callee may
                // freely replace every GPR, so delay-slot copies/literals
                // cannot become post-return evidence.
                RegisterState delay_validation_state = state_before_call;
                const auto* decoded_delay = decoded_line_at(
                    index, block.instructions[first_instruction].source_address);
                if (decoded_delay == nullptr || !decoded_delay->is_delay_slot ||
                    !apply_register_provenance(
                        delay_validation_state, image, *decoded_delay,
                        block.instructions[first_instruction]))
                    continue;
                ++first_instruction;

                // The direct SH-4 call ABI places the returned object in r0,
                // but this origin is created only after the delay slot.
                constexpr std::uint8_t return_register = 0u;
                RegisterState post_return_state;
                post_return_state.returned_objects[return_register] =
                    ReturnOrigin{call.source_address, *callee_address};
                const auto continuation_address = call.source_address + 4u;
                if (first_instruction < block.instructions.size()) {
                    scan_straight_line(
                        candidates, image, function, block_order,
                        predecessor_counts, block_indices,
                        blocks_by_address, index, block_index,
                        first_instruction, post_return_state,
                        native_entry_shapes);
                    continue;
                }
                if (!contains_successor(block, continuation_address)) continue;
                const auto continuation_index =
                    unique_block_index(block_indices, continuation_address);
                if (!continuation_index.has_value()) continue;
                if (predecessor_counts[*continuation_index] != 1u) continue;
                scan_straight_line(
                    candidates, image, function, block_order,
                    predecessor_counts, block_indices,
                    blocks_by_address, index, *continuation_index, 0u,
                    post_return_state, native_entry_shapes);
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return candidate_key(left) < candidate_key(right);
              });
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

} // namespace katana::analysis::detail
