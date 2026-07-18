#include "katana/analysis/basic_blocks.hpp"

#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis {
namespace {

std::size_t terminal_index(const std::span<const katana::sh4::DisassemblyLine> lines,
                           const std::size_t control_index) {
    if (lines[control_index].instruction.has_delay_slot && control_index + 1u < lines.size() &&
        lines[control_index + 1u].address == lines[control_index].address + 2u &&
        lines[control_index + 1u].is_delay_slot) {
        return control_index + 1u;
    }

    return control_index;
}

void add_unique_successor(std::vector<std::uint32_t>& successors, const std::uint32_t address) {
    if (std::find(successors.begin(), successors.end(), address) == successors.end()) {
        successors.push_back(address);
    }
}

std::size_t find_controlling_instruction(const BasicBlock& block) {
    if (block.lines.empty()) {
        return 0;
    }

    const auto last = block.lines.size() - 1u;

    if (block.lines[last].is_delay_slot && last > 0u &&
        block.lines[last - 1u].instruction.has_delay_slot &&
        block.lines[last].address == block.lines[last - 1u].address + 2u) {
        return last - 1u;
    }

    return last;
}

} // namespace

std::vector<BasicBlock>
build_basic_blocks(const std::span<const katana::sh4::DisassemblyLine> lines,
                   const std::span<const ResolvedControlFlowEdge> resolved_edges,
                   const std::span<const std::uint32_t> additional_leaders) {
    if (lines.empty()) {
        return {};
    }

    std::unordered_map<std::uint32_t, std::size_t> address_to_index;
    address_to_index.reserve(lines.size());

    for (std::size_t index = 0; index < lines.size(); ++index) {
        address_to_index.emplace(lines[index].address, index);
    }

    std::set<std::size_t> leaders;
    leaders.insert(0u);

    for (std::size_t index = 1u; index < lines.size(); ++index) {
        if (lines[index].address != lines[index - 1u].address + 2u) {
            leaders.insert(index);
        }
    }

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];

        if (line.target_address.has_value()) {
            const auto target = address_to_index.find(*line.target_address);

            if (target != address_to_index.end()) {
                leaders.insert(target->second);
            }
        }

        if (!line.instruction.changes_control_flow()) {
            continue;
        }

        const auto end_index = terminal_index(lines, index);
        const auto next_index = end_index + 1u;

        if (next_index < lines.size()) {
            leaders.insert(next_index);
        }
    }

    for (const auto& edge : resolved_edges) {
        if (const auto target = address_to_index.find(edge.target_address);
            target != address_to_index.end()) {
            leaders.insert(target->second);
        }
    }
    for (const auto address : additional_leaders) {
        if (const auto leader = address_to_index.find(address); leader != address_to_index.end()) {
            leaders.insert(leader->second);
        }
    }

    const std::vector<std::size_t> ordered_leaders(leaders.begin(), leaders.end());

    std::vector<BasicBlock> blocks;
    blocks.reserve(ordered_leaders.size());

    for (std::size_t leader_position = 0; leader_position < ordered_leaders.size();
         ++leader_position) {
        const auto first_index = ordered_leaders[leader_position];

        const auto end_exclusive = leader_position + 1u < ordered_leaders.size()
                                       ? ordered_leaders[leader_position + 1u]
                                       : lines.size();

        BasicBlock block;
        block.id = blocks.size();
        block.start_address = lines[first_index].address;
        block.end_address = lines[end_exclusive - 1u].address;

        block.lines.insert(block.lines.end(),
                           lines.begin() + static_cast<std::ptrdiff_t>(first_index),
                           lines.begin() + static_cast<std::ptrdiff_t>(end_exclusive));

        blocks.push_back(std::move(block));
    }

    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        auto& block = blocks[block_index];

        if (block.lines.empty()) {
            continue;
        }

        const auto control_index = find_controlling_instruction(block);
        const auto& control_line = block.lines[control_index];
        const auto& instruction = control_line.instruction;
        const bool paired_delay_slot =
            !instruction.has_delay_slot ||
            (control_index + 1u < block.lines.size() &&
             block.lines[control_index + 1u].is_delay_slot &&
             block.lines[control_index + 1u].address == control_line.address + 2u);

        const auto fallthrough_address =
            control_line.address + (instruction.has_delay_slot ? 4u : 2u);
        const auto has_fallthrough = std::any_of(
            blocks.begin(), blocks.end(), [fallthrough_address](const auto& candidate) {
                return candidate.start_address == fallthrough_address;
            }) && paired_delay_slot;

        if (!paired_delay_slot) {
            block.has_indirect_successor = instruction.control_flow ==
                                               katana::sh4::ControlFlowKind::IndirectCall ||
                                           instruction.control_flow ==
                                               katana::sh4::ControlFlowKind::IndirectBranch;
            continue;
        }

        if (!instruction.changes_control_flow()) {
            if (has_fallthrough) add_unique_successor(block.successors, fallthrough_address);

            continue;
        }

        if (control_line.target_address.has_value() &&
            instruction.control_flow != katana::sh4::ControlFlowKind::Call) {
            add_unique_successor(block.successors, *control_line.target_address);
        }

        switch (instruction.control_flow) {
        case katana::sh4::ControlFlowKind::ConditionalBranch:
            if (has_fallthrough) add_unique_successor(block.successors, fallthrough_address);
            break;

        case katana::sh4::ControlFlowKind::Call:
            if (has_fallthrough) add_unique_successor(block.successors, fallthrough_address);
            break;

        case katana::sh4::ControlFlowKind::IndirectCall:
            block.has_indirect_successor = true;

            if (has_fallthrough) add_unique_successor(block.successors, fallthrough_address);
            break;

        case katana::sh4::ControlFlowKind::IndirectBranch:
            block.has_indirect_successor = true;
            break;

        case katana::sh4::ControlFlowKind::UnconditionalBranch:
        case katana::sh4::ControlFlowKind::Return:
        case katana::sh4::ControlFlowKind::Trap:
        case katana::sh4::ControlFlowKind::ExceptionReturn:
        case katana::sh4::ControlFlowKind::Halt:
        case katana::sh4::ControlFlowKind::None:
            break;
        }

        std::sort(block.successors.begin(), block.successors.end());
    }

    std::unordered_map<std::uint32_t, std::size_t> block_by_control;
    block_by_control.reserve(blocks.size());
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (!blocks[index].lines.empty()) {
            block_by_control.emplace(
                blocks[index].lines[find_controlling_instruction(blocks[index])].address, index);
        }
    }
    std::unordered_map<std::uint32_t, bool> complete_sites;
    for (const auto& edge : resolved_edges) {
        const auto found = block_by_control.find(edge.instruction_address);
        if (found == block_by_control.end()) continue;
        auto& block = blocks[found->second];
        const auto control_index = find_controlling_instruction(block);
        const auto& instruction = block.lines[control_index].instruction;
        if (instruction.has_delay_slot &&
            (control_index + 1u >= block.lines.size() ||
             !block.lines[control_index + 1u].is_delay_slot ||
             block.lines[control_index + 1u].address !=
                 block.lines[control_index].address + 2u))
            continue;
        if (edge.kind == ResolvedControlFlowKind::Jump) {
            add_unique_successor(block.successors, edge.target_address);
        }
        const auto evidence = resolved_edge_evidence(edge);
        const auto [site, inserted] =
            complete_sites.emplace(edge.instruction_address,
                                   control_flow_evidence_complete(evidence));
        if (!inserted)
            site->second = site->second && control_flow_evidence_complete(evidence);
        std::sort(block.successors.begin(), block.successors.end());
    }
    for (const auto& [instruction_address, complete] : complete_sites) {
        if (!complete) continue;
        if (const auto found = block_by_control.find(instruction_address);
            found != block_by_control.end())
            blocks[found->second].has_indirect_successor = false;
    }

    return blocks;
}

} // namespace katana::analysis
