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

struct ExactFunctionOwnershipRange final {
    std::uint32_t entry = 0u;
    std::uint64_t end = 0u;
};

template <typename Callback>
bool for_each_exact_function_owner(
    const std::span<const ExactFunctionOwnershipRange> ranges,
    const std::uint32_t address,
    Callback&& callback) {
    const auto after = std::upper_bound(
        ranges.begin(), ranges.end(), address,
        [](const auto candidate, const auto& range) {
            return candidate < range.entry;
        });
    bool found = false;
    auto current = after;
    while (current != ranges.begin()) {
        --current;
        if (static_cast<std::uint64_t>(address) >= current->end) break;
        found = true;
        callback(*current);
    }
    return found;
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

    std::vector<ExactFunctionOwnershipRange> exact_ranges;
    exact_ranges.reserve(seed_boundaries.size());

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
            if (inserted)
                exact_ranges.push_back({boundary.entry_address, end});
        }
    }
    std::sort(exact_ranges.begin(), exact_ranges.end(),
              [](const auto& left, const auto& right) {
                  return left.entry < right.entry;
              });
    for (std::size_t index = 1u; index < exact_ranges.size(); ++index) {
        if (exact_ranges[index].entry < exact_ranges[index - 1u].end &&
            exact_ranges[index].end != exact_ranges[index - 1u].end)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen ueberlappen sich.");
    }

    std::unordered_map<std::uint32_t, std::set<std::uint32_t>>
        interior_entries_by_owner;
    const auto record_interior_owners =
        [&](const std::uint32_t address) {
            return for_each_exact_function_owner(
                exact_ranges, address,
                [&](const auto& owner) {
                    interior_entries_by_owner[owner.entry].insert(address);
                });
        };
    const auto classify_entry = [&](const std::uint32_t address) {
        if (!block_by_start.contains(address)) return;
        // An explicit entry remains an independent function even when two
        // exact functions share the same suffix. Non-entry targets inside
        // such a suffix belong to every declared owner and must not create a
        // synthetic nested function.
        if (!exact_ends.contains(address) &&
            record_interior_owners(address))
            return;
        known_entries.insert(address);
    };
    for (const auto& boundary : seed_boundaries)
        classify_entry(boundary.entry_address);

    for (const auto& block : blocks) {
        if (block.lines.empty()) {
            continue;
        }

        const auto& control = controlling_line(block);

        if (control.instruction.control_flow == katana::sh4::ControlFlowKind::Call &&
            control.target_address.has_value() &&
            block_by_start.contains(*control.target_address)) {
            classify_entry(*control.target_address);
        }
    }
    for (const auto& edge : resolved_edges) {
        if (edge.kind != ResolvedControlFlowKind::Call ||
            !block_by_start.contains(edge.target_address))
            continue;
        if (!exact_ends.contains(edge.target_address) &&
            record_interior_owners(edge.target_address)) {
            // Guarded or partial indirect-call evidence still denotes a real
            // externally reachable block when it lands inside an exact
            // function. It must be emitted under every shared owner, but it
            // must never manufacture a nested function boundary.
        } else if (control_flow_evidence_complete(
                       resolved_edge_evidence(edge))) {
            classify_entry(edge.target_address);
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
        const auto is_shared_exact_entry =
            [&](const std::uint32_t candidate) {
                if (exact_end == exact_ends.end() || candidate == entry)
                    return false;
                const auto candidate_end = exact_ends.find(candidate);
                return candidate_end != exact_ends.end() &&
                       candidate_end->second == exact_end->second &&
                       candidate >= entry &&
                       static_cast<std::uint64_t>(candidate) <
                           exact_end->second;
            };

        std::deque<std::uint32_t> pending_blocks;
        std::unordered_set<std::uint32_t> visited_blocks;

        pending_blocks.push_back(entry);
        if (const auto interior = interior_entries_by_owner.find(entry);
            interior != interior_entries_by_owner.end())
            pending_blocks.insert(pending_blocks.end(),
                                  interior->second.begin(),
                                  interior->second.end());

        while (!pending_blocks.empty()) {
            const auto block_address = pending_blocks.front();
            pending_blocks.pop_front();

            if (visited_blocks.contains(block_address)) {
                continue;
            }

            if (block_address != entry &&
                known_entries.contains(block_address) &&
                !is_shared_exact_entry(block_address)) {
                continue;
            }
            const bool block_in_current_exact =
                exact_end != exact_ends.end() &&
                block_address >= entry &&
                static_cast<std::uint64_t>(block_address) <
                    exact_end->second;
            const bool block_has_exact_owner =
                for_each_exact_function_owner(
                    exact_ranges, block_address, [](const auto&) {});
            if (block_has_exact_owner && !block_in_current_exact)
                continue;
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
                const auto target = *control.target_address;
                const bool target_is_local_exact_interior =
                    exact_end != exact_ends.end() &&
                    target >= entry &&
                    static_cast<std::uint64_t>(target) <
                        exact_end->second &&
                    !exact_ends.contains(target);
                if (target == entry || target_is_local_exact_interior) {
                    if (block_by_start.contains(target))
                        pending_blocks.push_back(target);
                } else {
                    const auto append_callee = [&](const auto callee) {
                        function.direct_callees.push_back(callee);
                        if (block_by_start.contains(callee) &&
                            !processed_entries.contains(callee))
                            pending_entries.push_back(callee);
                    };
                    if (exact_ends.contains(target)) {
                        append_callee(target);
                    } else if (!for_each_exact_function_owner(
                                   exact_ranges, target,
                                   [&](const auto& owner) {
                                       append_callee(owner.entry);
                                   })) {
                        append_callee(target);
                    }
                }
            }

            if (flow == katana::sh4::ControlFlowKind::IndirectCall) {
                function.indirect_call_sites.push_back(control.address);
            }

            const auto [edge_begin, edge_end] = edges_by_instruction.equal_range(control.address);
            for (auto edge = edge_begin; edge != edge_end; ++edge) {
                if (edge->second->kind == ResolvedControlFlowKind::Call) {
                    const auto target = edge->second->target_address;
                    function.indirect_call_sites.push_back(control.address);
                    const bool complete = control_flow_evidence_complete(
                        resolved_edge_evidence(*edge->second));
                    const bool target_is_local_exact_interior =
                        exact_end != exact_ends.end() &&
                        target >= entry &&
                        static_cast<std::uint64_t>(target) <
                            exact_end->second &&
                        !exact_ends.contains(target);
                    if (target == entry || target_is_local_exact_interior) {
                        if (control_flow_evidence_complete(
                                resolved_edge_evidence(*edge->second)) &&
                            block_by_start.contains(target))
                            pending_blocks.push_back(target);
                    } else {
                        const auto append_callee = [&](const auto callee) {
                            function.direct_callees.push_back(callee);
                            if (complete && block_by_start.contains(callee) &&
                                !processed_entries.contains(callee))
                                pending_entries.push_back(callee);
                        };
                        if (exact_ends.contains(target)) {
                            append_callee(target);
                        } else if (!for_each_exact_function_owner(
                                       exact_ranges, target,
                                       [&](const auto& owner) {
                                           append_callee(owner.entry);
                                       })) {
                            append_callee(target);
                        }
                    }
                }
            }

            for (const auto successor : block.successors) {
                const bool crosses_exact_boundary =
                    exact_end != exact_ends.end() &&
                    (successor < entry ||
                     static_cast<std::uint64_t>(successor) >=
                         exact_end->second);
                const bool successor_in_current_exact =
                    exact_end != exact_ends.end() &&
                    successor >= entry &&
                    static_cast<std::uint64_t>(successor) <
                        exact_end->second;
                const bool crosses_exact_owner =
                    !successor_in_current_exact &&
                    for_each_exact_function_owner(
                        exact_ranges, successor, [](const auto&) {});
                if (crosses_exact_boundary || crosses_exact_owner) {
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

                if (successor != entry && known_entries.contains(successor) &&
                    !is_shared_exact_entry(successor)) {
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

    const auto function_for_entry =
        [&](const std::uint32_t entry_address) -> const FunctionInfo* {
            const auto found = std::lower_bound(
                functions.begin(), functions.end(), entry_address,
                [](const FunctionInfo& function, const auto candidate) {
                    return function.entry_address < candidate;
                });
            return found != functions.end() &&
                           found->entry_address == entry_address
                       ? &*found
                       : nullptr;
        };
    for (std::size_t group_begin = 0u;
         group_begin < exact_ranges.size();) {
        std::size_t group_end = group_begin + 1u;
        while (group_end < exact_ranges.size() &&
               exact_ranges[group_end].entry <
                   exact_ranges[group_begin].end &&
               exact_ranges[group_end].end ==
                   exact_ranges[group_begin].end) {
            ++group_end;
        }
        if (group_end - group_begin > 1u) {
            const FunctionInfo* smallest = nullptr;
            for (auto index = group_begin; index < group_end; ++index) {
                const auto* function =
                    function_for_entry(exact_ranges[index].entry);
                if (function == nullptr) {
                    throw std::invalid_argument(
                        "Explizite Funktionsgrenzen ueberlappen sich.");
                }
                if (smallest == nullptr ||
                    function->block_addresses.size() <
                        smallest->block_addresses.size()) {
                    smallest = function;
                }
            }

            const auto final_halfword = static_cast<std::uint32_t>(
                exact_ranges[group_begin].end - 2u);
            const bool has_proven_shared_tail = std::any_of(
                smallest->block_addresses.begin(),
                smallest->block_addresses.end(),
                [&](const auto block_address) {
                    const auto block = block_by_start.find(block_address);
                    if (block == block_by_start.end() ||
                        blocks[block->second].lines.empty() ||
                        blocks[block->second].end_address !=
                            final_halfword) {
                        return false;
                    }
                    for (auto index = group_begin;
                         index < group_end; ++index) {
                        const auto* function = function_for_entry(
                            exact_ranges[index].entry);
                        if (!std::binary_search(
                                function->block_addresses.begin(),
                                function->block_addresses.end(),
                                block_address)) {
                            return false;
                        }
                    }
                    return true;
                });
            if (!has_proven_shared_tail) {
                throw std::invalid_argument(
                    "Explizite Funktionsgrenzen ueberlappen sich.");
            }
        }
        group_begin = group_end;
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
