#include "katana/analysis/function_analysis.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
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

void canonicalize_addresses(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

std::vector<FunctionInfo>
discover_functions_from_blocks(const std::span<const BasicBlock> blocks,
                               const std::span<const std::uint32_t> seed_entries,
                               const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    std::vector<FunctionBoundary> boundaries;
    boundaries.reserve(seed_entries.size());
    for (const auto entry : seed_entries)
        boundaries.push_back({entry, 0u});
    return discover_functions_from_blocks(blocks, boundaries, resolved_edges);
}

std::vector<FunctionInfo>
discover_functions_from_blocks(
    const std::span<const BasicBlock> blocks,
    const std::span<const FunctionBoundary> seed_boundaries,
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
    std::unordered_map<std::uint32_t, std::uint64_t> exact_ends;
    exact_ends.reserve(seed_boundaries.size());

    for (const auto& boundary : seed_boundaries) {
        if (boundary.size != 0u) {
            const auto end = static_cast<std::uint64_t>(boundary.entry_address) +
                             boundary.size;
            if ((boundary.size & 1u) != 0u ||
                end > static_cast<std::uint64_t>(
                          std::numeric_limits<std::uint32_t>::max()) +
                          1u)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenze ist ungueltig.");
            const auto [existing, inserted] =
                exact_ends.emplace(boundary.entry_address, end);
            if (!inserted && existing->second != end)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenzen widersprechen sich.");
        }
        if (block_by_start.contains(boundary.entry_address)) {
            known_entries.insert(boundary.entry_address);
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
            control_flow_evidence_complete(resolved_edge_evidence(edge)) &&
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
        const auto exact_end = exact_ends.find(entry);
        if (exact_end != exact_ends.end())
            function.size =
                static_cast<std::uint32_t>(exact_end->second - entry);

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
            if (exact_end != exact_ends.end() &&
                (block_address < entry ||
                 static_cast<std::uint64_t>(block_address) >=
                     exact_end->second)) {
                continue;
            }

            const auto block_iterator = block_by_start.find(block_address);

            if (block_iterator == block_by_start.end()) {
                continue;
            }

            visited_blocks.insert(block_address);
            function.block_addresses.push_back(block_address);

            const auto& block = blocks[block_iterator->second];

            if (block.lines.empty()) {
                continue;
            }

            const auto& control = controlling_line(block);
            const auto flow = control.instruction.control_flow;

            if (flow == katana::sh4::ControlFlowKind::Call && control.target_address.has_value()) {
                function.direct_callees.push_back(*control.target_address);

                if (block_by_start.contains(*control.target_address) &&
                    !processed_entries.contains(*control.target_address)) {
                    pending_entries.push_back(*control.target_address);
                }
            }

            if (flow == katana::sh4::ControlFlowKind::IndirectCall) {
                function.indirect_call_sites.push_back(control.address);
            }

            const auto [edge_begin, edge_end] = edges_by_instruction.equal_range(control.address);
            for (auto edge = edge_begin; edge != edge_end; ++edge) {
                if (edge->second->kind == ResolvedControlFlowKind::Call) {
                    function.direct_callees.push_back(edge->second->target_address);
                    function.indirect_call_sites.push_back(control.address);
                    if (control_flow_evidence_complete(resolved_edge_evidence(*edge->second)) &&
                        block_by_start.contains(edge->second->target_address) &&
                        !processed_entries.contains(edge->second->target_address)) {
                        pending_entries.push_back(edge->second->target_address);
                    }
                }
            }

            for (const auto successor : block.successors) {
                const bool crosses_exact_boundary =
                    exact_end != exact_ends.end() &&
                    (successor < entry ||
                     static_cast<std::uint64_t>(successor) >=
                         exact_end->second);
                if (crosses_exact_boundary) {
                    if (flow ==
                            katana::sh4::ControlFlowKind::
                                UnconditionalBranch ||
                        flow ==
                            katana::sh4::ControlFlowKind::IndirectBranch)
                        function.tail_jump_targets.push_back(successor);
                    continue;
                }
                if (flow == katana::sh4::ControlFlowKind::Call &&
                    control.target_address.has_value() && successor == *control.target_address) {
                    continue;
                }

                if (successor != entry && known_entries.contains(successor)) {
                    if (flow == katana::sh4::ControlFlowKind::UnconditionalBranch ||
                        flow == katana::sh4::ControlFlowKind::IndirectBranch)
                        function.tail_jump_targets.push_back(successor);
                    continue;
                }

                pending_blocks.push_back(successor);
            }
        }

        canonicalize_addresses(function.block_addresses);
        canonicalize_addresses(function.direct_callees);
        canonicalize_addresses(function.indirect_call_sites);
        canonicalize_addresses(function.tail_jump_targets);
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
        for (const auto block : function.block_addresses)
            ++owners[block];
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
    std::vector<FunctionBoundary> boundaries;
    boundaries.reserve(seed_entries.size());
    for (const auto entry : seed_entries)
        boundaries.push_back({entry, 0u});
    return discover_functions(lines, boundaries, resolved_edges);
}

std::vector<FunctionInfo>
discover_functions(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const FunctionBoundary> seed_boundaries,
    const std::span<const ResolvedControlFlowEdge> resolved_edges) {
    std::vector<std::uint32_t> leaders;
    leaders.reserve(seed_boundaries.size() * 2u);
    for (const auto& boundary : seed_boundaries) {
        leaders.push_back(boundary.entry_address);
        if (boundary.size != 0u) {
            const auto end = static_cast<std::uint64_t>(
                                 boundary.entry_address) +
                             boundary.size;
            if (end <= std::numeric_limits<std::uint32_t>::max())
                leaders.push_back(static_cast<std::uint32_t>(end));
        }
    }
    const auto blocks = build_basic_blocks(lines, resolved_edges, leaders);
    return discover_functions_from_blocks(
        blocks, seed_boundaries, resolved_edges);
}

} // namespace katana::analysis
