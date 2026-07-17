#include "katana/analysis/function_value_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_summary_values = 8u;

struct AbstractValue {
    bool known = false;
    std::vector<std::uint32_t> values;
    std::set<std::uint32_t> call_sites;
    std::set<std::uint32_t> callees;
};

using AbstractState = std::array<AbstractValue, 16u>;

struct FunctionEvaluation {
    FunctionValueSummary summary;
    std::vector<InterproceduralTargetResolution> resolutions;
};

void normalize(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void make_unknown(AbstractValue& value) {
    value.known = false;
    value.values.clear();
    value.call_sites.clear();
    value.callees.clear();
}

void set_value(AbstractValue& value, const std::uint32_t constant) {
    value.known = true;
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
        destination.values.clear();
    } else {
        destination.values.insert(
            destination.values.end(), source.values.begin(), source.values.end());
        normalize(destination.values);
        if (destination.values.size() > maximum_summary_values) make_unknown(destination);
    }
    return destination.known != before.known || destination.values != before.values ||
           destination.call_sites != before.call_sites || destination.callees != before.callees;
}

bool merge_state(AbstractState& destination, const AbstractState& source) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.size(); ++index)
        changed = merge_value(destination[index], source[index]) || changed;
    return changed;
}

constexpr std::uint16_t register_bit(const std::uint8_t index) {
    return static_cast<std::uint16_t>(1u << index);
}

void clear_written(AbstractState& state, const katana::sh4::DecodedInstruction& instruction) {
    const auto mask = general_register_write_mask(instruction);
    for (std::uint8_t index = 0u; index < state.size(); ++index) {
        if ((mask & register_bit(index)) != 0u) make_unknown(state[index]);
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
            values.push_back(kind == katana::sh4::InstructionKind::AddRegister ? left + right
                                                                               : left - right);
            if (values.size() > maximum_summary_values) {
                make_unknown(destination);
                return;
            }
        }
    }
    normalize(values);
    destination.values = std::move(values);
    destination.call_sites.insert(source.call_sites.begin(), source.call_sites.end());
    destination.callees.insert(source.callees.begin(), source.callees.end());
}

void apply_transfer(AbstractState& state, const katana::sh4::DisassemblyLine& line) {
    const auto& instruction = line.instruction;
    switch (instruction.kind) {
    case katana::sh4::InstructionKind::Nop:
    case katana::sh4::InstructionKind::Rts:
        return;
    case katana::sh4::InstructionKind::MovImmediate:
        set_value(state[instruction.destination_register],
                  static_cast<std::uint32_t>(instruction.immediate));
        return;
    case katana::sh4::InstructionKind::MovRegister:
        state[instruction.destination_register] = state[instruction.source_register];
        return;
    case katana::sh4::InstructionKind::AddImmediate:
        if (!state[instruction.destination_register].known) return;
        for (auto& value : state[instruction.destination_register].values)
            value += static_cast<std::uint32_t>(instruction.immediate);
        normalize(state[instruction.destination_register].values);
        return;
    case katana::sh4::InstructionKind::AddRegister:
    case katana::sh4::InstructionKind::SubRegister:
        apply_binary(state[instruction.destination_register],
                     state[instruction.source_register],
                     instruction.kind);
        return;
    case katana::sh4::InstructionKind::MoveAddressPcRelative:
        set_value(state[0u],
                  ((line.address + 4u) & ~3u) +
                      static_cast<std::uint32_t>(instruction.displacement));
        return;
    default:
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
                const std::map<std::uint32_t, FunctionValueSummary>& summaries) {
    if (image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC) {
        for (auto& value : state)
            make_unknown(value);
        return;
    }
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        make_unknown(state[index]);
    make_unknown(state[15u]);
    if (!callee.has_value()) return;
    const auto summary = summaries.find(*callee);
    if (summary == summaries.end()) return;
    const auto* returned = register_summary(summary->second, 0u);
    if (returned == nullptr || !returned->complete || returned->values.empty()) return;
    state[0u].known = true;
    state[0u].values = returned->values;
    state[0u].call_sites = {call_site};
    state[0u].callees = {*callee};
    state[0u].callees.insert(returned->evidence_callees.begin(), returned->evidence_callees.end());
}

const katana::sh4::DisassemblyLine& controlling_line(const BasicBlock& block) {
    const auto last = block.lines.size() - 1u;
    return block.lines[last].is_delay_slot && last > 0u ? block.lines[last - 1u]
                                                        : block.lines[last];
}

std::vector<std::uint32_t> checked_targets(const katana::io::ExecutableImage& image,
                                           const katana::sh4::DisassemblyLine& line,
                                           const AbstractValue& value) {
    std::vector<std::uint32_t> targets;
    for (const auto candidate : value.values) {
        std::uint64_t target = candidate;
        if (line.instruction.kind == katana::sh4::InstructionKind::Braf ||
            line.instruction.kind == katana::sh4::InstructionKind::Bsrf) {
            target += static_cast<std::uint64_t>(line.address) + 4u;
        }
        if (target > 0xFFFFFFFFull) return {};
        const auto narrowed = static_cast<std::uint32_t>(target);
        if (!validate_committed_code_address(image, narrowed).valid()) return {};
        targets.push_back(narrowed);
    }
    normalize(targets);
    return targets;
}

FunctionEvaluation
evaluate_function(const katana::io::ExecutableImage& image,
                  const FunctionInfo& function,
                  const std::unordered_map<std::uint32_t, const BasicBlock*>& blocks,
                  const std::map<std::uint32_t, FunctionValueSummary>& summaries,
                  const bool collect_resolutions) {
    FunctionEvaluation evaluation;
    evaluation.summary.function_address = function.entry_address;
    std::unordered_set<std::uint32_t> members(function.block_addresses.begin(),
                                              function.block_addresses.end());
    std::unordered_map<std::uint32_t, AbstractState> inputs;
    std::deque<std::uint32_t> pending;
    inputs.emplace(function.entry_address, AbstractState{});
    pending.push_back(function.entry_address);
    std::vector<std::pair<std::uint32_t, AbstractState>> returns;

    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        const auto block = blocks.find(address);
        if (block == blocks.end()) continue;
        auto state = inputs[address];
        std::optional<std::pair<std::uint32_t, std::optional<std::uint32_t>>> delayed_call;
        for (const auto& line : block->second->lines) {
            const bool indirect = line.instruction.kind == katana::sh4::InstructionKind::Jmp ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Braf ||
                                  line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
            if (collect_resolutions && indirect) {
                const auto& value = state[line.instruction.branch_register];
                if (value.known && !value.values.empty() && !value.callees.empty()) {
                    auto targets = checked_targets(image, line, value);
                    if (!targets.empty()) {
                        InterproceduralTargetResolution resolution;
                        resolution.instruction_address = line.address;
                        resolution.register_index = line.instruction.branch_register;
                        resolution.call =
                            line.instruction.kind == katana::sh4::InstructionKind::Jsr ||
                            line.instruction.kind == katana::sh4::InstructionKind::Bsrf;
                        resolution.targets = std::move(targets);
                        resolution.call_sites.assign(value.call_sites.begin(),
                                                     value.call_sites.end());
                        resolution.callees.assign(value.callees.begin(), value.callees.end());
                        resolution.reason = resolution.targets.size() == 1u
                                                ? "interprocedural-return-constant"
                                                : "interprocedural-return-set";
                        evaluation.resolutions.push_back(std::move(resolution));
                    }
                }
            }

            const bool call =
                line.instruction.control_flow == katana::sh4::ControlFlowKind::Call ||
                line.instruction.control_flow == katana::sh4::ControlFlowKind::IndirectCall;
            if (!call) apply_transfer(state, line);
            if (delayed_call.has_value()) {
                apply_call(state, image, delayed_call->first, delayed_call->second, summaries);
                delayed_call.reset();
            }
            if (call) {
                const auto callee =
                    line.instruction.control_flow == katana::sh4::ControlFlowKind::Call
                        ? line.target_address
                        : std::nullopt;
                if (line.instruction.has_delay_slot)
                    delayed_call = std::pair{line.address, callee};
                else
                    apply_call(state, image, line.address, callee, summaries);
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
            if (!value.known || value.values.empty()) {
                complete = false;
                continue;
            }
            values.insert(value.values.begin(), value.values.end());
            evidence.insert(value.callees.begin(), value.callees.end());
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
    return evaluation;
}

} // namespace

FunctionValueAnalysisResult
analyze_function_values(const katana::io::ExecutableImage& image,
                        const std::span<const katana::sh4::DisassemblyLine> lines,
                        const std::span<const std::uint32_t> function_entries,
                        const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    FunctionValueAnalysisResult result;
    if (lines.empty() || function_entries.empty() ||
        image.guest_call_abi() != katana::io::GuestCallAbi::SuperHC)
        return result;
    const auto blocks = build_basic_blocks(lines, resolved_edges, function_entries);
    std::unordered_map<std::uint32_t, const BasicBlock*> block_index;
    for (const auto& block : blocks)
        block_index.emplace(block.start_address, &block);
    const auto functions = discover_functions_from_blocks(blocks, function_entries, resolved_edges);
    std::map<std::uint32_t, FunctionValueSummary> summaries;
    std::unordered_map<std::uint32_t, const FunctionInfo*> function_by_address;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> callers_by_callee;
    for (const auto& function : functions)
        summaries.emplace(function.entry_address, FunctionValueSummary{function.entry_address, {}});
    for (const auto& function : functions) {
        function_by_address.emplace(function.entry_address, &function);
        for (const auto callee : function.direct_callees)
            callers_by_callee[callee].push_back(function.entry_address);
    }
    std::deque<std::uint32_t> pending;
    std::unordered_set<std::uint32_t> queued;
    for (const auto& function : functions) {
        pending.push_back(function.entry_address);
        queued.insert(function.entry_address);
    }
    while (!pending.empty()) {
        const auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        const auto function = function_by_address.find(address);
        if (function == function_by_address.end()) continue;
        ++result.fixpoint_iterations;
        auto evaluation =
            evaluate_function(image, *function->second, block_index, summaries, false).summary;
        auto& previous = summaries[address];
        if (previous == evaluation) continue;
        previous = std::move(evaluation);
        const auto callers = callers_by_callee.find(address);
        if (callers == callers_by_callee.end()) continue;
        for (const auto caller : callers->second) {
            if (queued.insert(caller).second) pending.push_back(caller);
        }
    }

    for (const auto& [address, summary] : summaries)
        result.summaries.push_back(summary);
    for (const auto& function : functions) {
        auto evaluation = evaluate_function(image, function, block_index, summaries, true);
        result.resolutions.insert(result.resolutions.end(),
                                  std::make_move_iterator(evaluation.resolutions.begin()),
                                  std::make_move_iterator(evaluation.resolutions.end()));
    }
    std::sort(result.resolutions.begin(),
              result.resolutions.end(),
              [](const auto& left, const auto& right) {
                  return left.instruction_address < right.instruction_address;
              });
    result.resolutions.erase(std::unique(result.resolutions.begin(),
                                         result.resolutions.end(),
                                         [](const auto& left, const auto& right) {
                                             return left.instruction_address ==
                                                    right.instruction_address;
                                         }),
                             result.resolutions.end());
    return result;
}

} // namespace katana::analysis
