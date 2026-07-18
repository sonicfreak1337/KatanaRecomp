#include "katana/analysis/function_analysis.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace katana::analysis {
namespace {

const katana::sh4::DisassemblyLine& controlling_line(const BasicBlock& block) {
    const auto last_index = block.lines.size() - 1u;

    if (block.lines[last_index].is_delay_slot && last_index > 0u &&
        block.lines[last_index - 1u].instruction.has_delay_slot &&
        block.lines[last_index].address == block.lines[last_index - 1u].address + 2u) {
        return block.lines[last_index - 1u];
    }

    return block.lines[last_index];
}

void add_sorted_unique(std::vector<std::uint32_t>& values, const std::uint32_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
        std::sort(values.begin(), values.end());
    }
}

} // namespace

std::vector<FunctionInfo>
discover_functions_from_blocks(const std::span<const BasicBlock> blocks,
                               const std::span<const std::uint32_t> seed_entries,
                               const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    if (blocks.empty()) {
        return {};
    }

    std::unordered_map<std::uint32_t, std::size_t> block_by_start;
    block_by_start.reserve(blocks.size());

    for (std::size_t index = 0; index < blocks.size(); ++index) {
        block_by_start.emplace(blocks[index].start_address, index);
    }
    std::unordered_multimap<std::uint32_t, const ResolvedControlFlowEdge*> edges_by_instruction;
    edges_by_instruction.reserve(resolved_edges.size());
    for (const auto& edge : resolved_edges)
        edges_by_instruction.emplace(edge.instruction_address, &edge);

    std::set<std::uint32_t> known_entries;

    for (const auto entry : seed_entries) {
        if (block_by_start.contains(entry)) {
            known_entries.insert(entry);
        }
    }

    for (const auto& block : blocks) {
        if (block.lines.empty()) {
            continue;
        }

        const auto& control = controlling_line(block);

        if (control.instruction.control_flow == katana::sh4::ControlFlowKind::Call &&
            control.target_address.has_value() &&
            block_by_start.contains(*control.target_address)) {
            known_entries.insert(*control.target_address);
        }
    }
    for (const auto& edge : resolved_edges) {
        if (edge.kind == ResolvedControlFlowKind::Call &&
            resolved_edge_evidence(edge) == ControlFlowEvidence::ProvenComplete &&
            block_by_start.contains(edge.target_address)) {
            known_entries.insert(edge.target_address);
        }
    }

    std::deque<std::uint32_t> pending_entries(known_entries.begin(), known_entries.end());

    std::unordered_set<std::uint32_t> processed_entries;
    std::vector<FunctionInfo> functions;

    while (!pending_entries.empty()) {
        const auto entry = pending_entries.front();
        pending_entries.pop_front();

        if (processed_entries.contains(entry)) {
            continue;
        }

        const auto entry_block = block_by_start.find(entry);

        if (entry_block == block_by_start.end()) {
            continue;
        }

        processed_entries.insert(entry);

        FunctionInfo function;
        function.id = functions.size();
        function.entry_address = entry;

        std::deque<std::uint32_t> pending_blocks;
        std::unordered_set<std::uint32_t> visited_blocks;

        pending_blocks.push_back(entry);

        while (!pending_blocks.empty()) {
            const auto block_address = pending_blocks.front();
            pending_blocks.pop_front();

            if (visited_blocks.contains(block_address)) {
                continue;
            }

            if (block_address != entry && known_entries.contains(block_address)) {
                continue;
            }

            const auto block_iterator = block_by_start.find(block_address);

            if (block_iterator == block_by_start.end()) {
                continue;
            }

            visited_blocks.insert(block_address);
            add_sorted_unique(function.block_addresses, block_address);

            const auto& block = blocks[block_iterator->second];

            if (block.lines.empty()) {
                continue;
            }

            const auto& control = controlling_line(block);
            const auto flow = control.instruction.control_flow;

            if (flow == katana::sh4::ControlFlowKind::Call && control.target_address.has_value()) {
                add_sorted_unique(function.direct_callees, *control.target_address);

                if (block_by_start.contains(*control.target_address) &&
                    !processed_entries.contains(*control.target_address)) {
                    pending_entries.push_back(*control.target_address);
                }
            }

            if (flow == katana::sh4::ControlFlowKind::IndirectCall) {
                add_sorted_unique(function.indirect_call_sites, control.address);
            }

            const auto [edge_begin, edge_end] = edges_by_instruction.equal_range(control.address);
            for (auto edge = edge_begin; edge != edge_end; ++edge) {
                if (edge->second->kind == ResolvedControlFlowKind::Call) {
                    add_sorted_unique(function.direct_callees, edge->second->target_address);
                    add_sorted_unique(function.indirect_call_sites, control.address);
                    if (resolved_edge_evidence(*edge->second) ==
                            ControlFlowEvidence::ProvenComplete &&
                        block_by_start.contains(edge->second->target_address) &&
                        !processed_entries.contains(edge->second->target_address)) {
                        pending_entries.push_back(edge->second->target_address);
                    }
                }
            }

            for (const auto successor : block.successors) {
                if (flow == katana::sh4::ControlFlowKind::Call &&
                    control.target_address.has_value() && successor == *control.target_address) {
                    continue;
                }

                if (successor != entry && known_entries.contains(successor)) {
                    if (flow == katana::sh4::ControlFlowKind::UnconditionalBranch ||
                        flow == katana::sh4::ControlFlowKind::IndirectBranch)
                        add_sorted_unique(function.tail_jump_targets, successor);
                    continue;
                }

                pending_blocks.push_back(successor);
            }
        }

        functions.push_back(std::move(function));
    }

    std::sort(functions.begin(),
              functions.end(),
              [](const FunctionInfo& left, const FunctionInfo& right) {
                  return left.entry_address < right.entry_address;
              });

    for (std::size_t index = 0; index < functions.size(); ++index) {
        functions[index].id = index;
    }
    std::unordered_map<std::uint32_t, std::size_t> owners;
    for (const auto& function : functions) {
        for (const auto block : function.block_addresses) ++owners[block];
    }
    for (auto& function : functions) {
        for (const auto block : function.block_addresses) {
            if (owners[block] > 1u) function.shared_block_addresses.push_back(block);
        }
    }

    return functions;
}

std::vector<FunctionInfo>
discover_functions(const std::span<const katana::sh4::DisassemblyLine> lines,
                   const std::span<const std::uint32_t> seed_entries,
                   const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    const auto blocks = build_basic_blocks(lines, resolved_edges, seed_entries);
    return discover_functions_from_blocks(blocks, seed_entries, resolved_edges);
}

} // namespace katana::analysis
