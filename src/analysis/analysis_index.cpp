#include "katana/analysis/analysis_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace katana::analysis {

EvidenceId EvidenceInterner::intern(const std::string_view text) {
    if (text.empty()) return invalid_evidence_id;
    if (const auto found = ids_.find(std::string(text)); found != ids_.end()) return found->second;
    if (strings_.size() >= std::numeric_limits<EvidenceId>::max() - 1u) {
        throw std::overflow_error("Evidence-ID-Tabelle ist uebergelaufen.");
    }
    strings_.emplace_back(text);
    const auto id = static_cast<EvidenceId>(strings_.size());
    ids_.emplace(strings_.back(), id);
    return id;
}

std::string_view EvidenceInterner::resolve(const EvidenceId id) const {
    if (id == invalid_evidence_id || id > strings_.size()) {
        throw std::out_of_range("Unbekannte Evidence-ID.");
    }
    return strings_[id - 1u];
}

std::size_t EvidenceInterner::size() const noexcept {
    return strings_.size();
}

InstructionArena::InstructionArena(std::vector<katana::sh4::DisassemblyLine> instructions) {
    std::sort(instructions.begin(), instructions.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    for (std::size_t index = 1u; index < instructions.size(); ++index) {
        if (instructions[index - 1u].address == instructions[index].address) {
            throw std::invalid_argument("Instruktionsarena enthaelt doppelte Gastadressen.");
        }
    }
    instructions_ =
        std::make_shared<const std::vector<katana::sh4::DisassemblyLine>>(std::move(instructions));
    by_address_.reserve(instructions_->size());
    for (std::size_t index = 0u; index < instructions_->size(); ++index) {
        by_address_.emplace((*instructions_)[index].address, index);
    }
}

std::span<const katana::sh4::DisassemblyLine> InstructionArena::instructions() const noexcept {
    return *instructions_;
}

const katana::sh4::DisassemblyLine*
InstructionArena::find(const std::uint32_t address) const noexcept {
    const auto found = by_address_.find(address);
    return found == by_address_.end() ? nullptr : &(*instructions_)[found->second];
}

std::size_t InstructionArena::size() const noexcept {
    return instructions_->size();
}

std::span<const katana::sh4::DisassemblyLine>
InstructionSpan::view(const InstructionArena& arena) const {
    const auto instructions = arena.instructions();
    if (first > instructions.size() || count > instructions.size() - first) {
        throw std::out_of_range("Blockspan liegt ausserhalb der Instruktionsarena.");
    }
    return instructions.subspan(first, count);
}

std::vector<InstructionSpan>
build_block_spans(const InstructionArena& arena, const std::span<const BasicBlock> blocks) {
    const auto instructions = arena.instructions();
    std::vector<InstructionSpan> spans;
    spans.reserve(blocks.size());
    for (const auto& block : blocks) {
        const auto first = std::lower_bound(instructions.begin(),
                                            instructions.end(),
                                            block.start_address,
                                            [](const auto& line, const auto address) {
                                                return line.address < address;
                                            });
        const auto after = std::upper_bound(instructions.begin(),
                                            instructions.end(),
                                            block.end_address,
                                            [](const auto address, const auto& line) {
                                                return address < line.address;
                                            });
        if (first == instructions.end() || first == after || first->address != block.start_address) {
            throw std::invalid_argument("Basic Block besitzt keinen zusammenhaengenden Arenaspan.");
        }
        spans.push_back({static_cast<std::size_t>(first - instructions.begin()),
                         static_cast<std::size_t>(after - first)});
    }
    return spans;
}

AnalysisIndex::AnalysisIndex(const InstructionArena& arena,
                             const std::span<const BasicBlock> blocks,
                             const std::span<const ResolvedControlFlowEdge> edges,
                             const std::span<const FunctionInfo> functions,
                             const std::span<const katana::io::ImageSegment> segments) {
    instructions_.reserve(arena.size());
    for (std::size_t index = 0u; index < arena.size(); ++index) {
        instructions_.emplace(arena.instructions()[index].address, index);
    }
    blocks_.reserve(blocks.size());
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        blocks_.emplace(blocks[index].start_address, index);
    }
    edges_.reserve(edges.size());
    for (std::size_t index = 0u; index < edges.size(); ++index) {
        edges_[edges[index].instruction_address].push_back(index);
    }
    functions_.reserve(functions.size());
    for (std::size_t index = 0u; index < functions.size(); ++index) {
        functions_.emplace(functions[index].entry_address, index);
    }
    segments_.reserve(segments.size());
    for (std::size_t index = 0u; index < segments.size(); ++index) {
        segments_.emplace_back(
            segments[index].virtual_address, segments[index].end_address(), index);
    }
    std::sort(segments_.begin(), segments_.end());
}

std::optional<std::size_t> AnalysisIndex::instruction(const std::uint32_t address) const noexcept {
    const auto found = instructions_.find(address);
    return found == instructions_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::size_t> AnalysisIndex::block(const std::uint32_t start) const noexcept {
    const auto found = blocks_.find(start);
    return found == blocks_.end() ? std::nullopt : std::optional(found->second);
}

std::span<const std::size_t> AnalysisIndex::edges(const std::uint32_t site) const noexcept {
    const auto found = edges_.find(site);
    return found == edges_.end()
               ? std::span<const std::size_t>{}
               : std::span<const std::size_t>(found->second.data(), found->second.size());
}

std::optional<std::size_t> AnalysisIndex::function(const std::uint32_t entry) const noexcept {
    const auto found = functions_.find(entry);
    return found == functions_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::size_t> AnalysisIndex::segment(const std::uint32_t address) const noexcept {
    const auto next = std::upper_bound(
        segments_.begin(), segments_.end(), address, [](const auto value, const auto& item) {
            return value < std::get<0>(item);
        });
    if (next == segments_.begin()) return std::nullopt;
    const auto& candidate = *std::prev(next);
    return address < std::get<1>(candidate) ? std::optional(std::get<2>(candidate)) : std::nullopt;
}

} // namespace katana::analysis
