#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace katana::analysis {

struct BasicBlock {
    std::size_t id = 0;
    std::uint32_t start_address = 0;
    std::uint32_t end_address = 0;

    std::vector<katana::sh4::DisassemblyLine> lines;
    std::vector<std::uint32_t> successors;

    bool has_indirect_successor = false;
};

enum class ResolvedControlFlowKind { Jump, Call };

struct ResolvedControlFlowEdge {
    std::uint32_t instruction_address = 0u;
    std::uint32_t target_address = 0u;
    ResolvedControlFlowKind kind = ResolvedControlFlowKind::Jump;
    bool guarded = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::vector<AnalysisEvidenceOrigin> evidence_origins;

    bool operator==(const ResolvedControlFlowEdge&) const = default;
};

[[nodiscard]] constexpr ControlFlowEvidence
resolved_edge_evidence(const ResolvedControlFlowEdge& edge) noexcept {
    return edge.evidence == ControlFlowEvidence::Unresolved
               ? (edge.guarded ? ControlFlowEvidence::GuardedPartial
                               : ControlFlowEvidence::ProvenComplete)
               : edge.evidence;
}

[[nodiscard]] std::vector<BasicBlock>
build_basic_blocks(std::span<const katana::sh4::DisassemblyLine> lines,
                   std::span<const ResolvedControlFlowEdge> resolved_edges = {},
                   std::span<const std::uint32_t> additional_leaders = {});

} // namespace katana::analysis
