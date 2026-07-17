#pragma once

#include "katana/analysis/control_flow_analysis.hpp"

#include <span>
#include <string>

namespace katana::analysis {

struct ControlFlowReportSummary {
    std::size_t indirect_total = 0u;
    std::size_t resolved = 0u;
    std::size_t guarded = 0u;
    std::size_t unresolved = 0u;
    std::size_t proven_instructions = 0u;
    std::size_t guarded_candidate_instructions = 0u;
    std::size_t unresolved_frontier = 0u;
};

[[nodiscard]] ControlFlowReportSummary
summarize_control_flow_analysis(const ControlFlowAnalysisResult& analysis) noexcept;

[[nodiscard]] std::string
format_indirect_control_flow_report(std::span<const IndirectControlFlowResolution> resolutions,
                                    std::span<const JumpTableAnalysis> jump_tables = {},
                                    std::span<const SymbolicAddress> symbols = {});

[[nodiscard]] std::string
format_control_flow_analysis_json(const ControlFlowAnalysisResult& analysis);

} // namespace katana::analysis
