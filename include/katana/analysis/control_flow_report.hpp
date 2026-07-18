#pragma once

#include "katana/analysis/control_flow_analysis.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace katana::analysis {

enum class ControlFlowReportStatus : std::uint8_t {
    Resolved,
    GuardedComplete,
    GuardedPartial,
    RuntimeOnly,
    Unresolved
};

[[nodiscard]] ControlFlowReportStatus
control_flow_report_status(const IndirectControlFlowResolution& resolution) noexcept;

[[nodiscard]] const char* control_flow_report_status_name(ControlFlowReportStatus status) noexcept;

struct ControlFlowFrontierClassSummary {
    std::size_t callback = 0u;
    std::size_t parameter = 0u;
    std::size_t stack = 0u;
    std::size_t object_vtable = 0u;
    std::size_t table = 0u;
    std::size_t unbounded_memory = 0u;
    std::size_t runtime_pointer = 0u;
};

struct ControlFlowReportSummary {
    std::size_t indirect_total = 0u;
    std::size_t resolved = 0u;
    std::size_t guarded = 0u;
    std::size_t guarded_complete = 0u;
    std::size_t guarded_partial = 0u;
    std::size_t runtime_only = 0u;
    std::size_t unresolved = 0u;
    std::size_t proven_instructions = 0u;
    std::size_t guarded_candidate_instructions = 0u;
    std::size_t unresolved_frontier = 0u;
    ControlFlowFrontierClassSummary frontier_classes;
};

[[nodiscard]] ControlFlowReportSummary
summarize_control_flow_analysis(const ControlFlowAnalysisResult& analysis) noexcept;

[[nodiscard]] std::string
format_indirect_control_flow_report(std::span<const IndirectControlFlowResolution> resolutions,
                                    std::span<const JumpTableAnalysis> jump_tables = {},
                                    std::span<const SymbolicAddress> symbols = {});

[[nodiscard]] std::string
format_control_flow_analysis_json(const ControlFlowAnalysisResult& analysis);

[[nodiscard]] std::string
format_control_flow_frontier_json(const ControlFlowAnalysisResult& analysis);

} // namespace katana::analysis
