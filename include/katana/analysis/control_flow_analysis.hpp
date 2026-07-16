#pragma once

#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

enum class AnalysisDirectiveDiagnosticStatus : std::uint8_t {
    Accepted,
    Confirmed,
    Rejected,
    Stale
};

struct AnalysisDirectiveDiagnostic {
    std::size_t line = 0u;
    std::uint32_t address = 0u;
    AnalysisDirectiveDiagnosticStatus status = AnalysisDirectiveDiagnosticStatus::Accepted;
    std::string reason;
};

struct ControlFlowAnalysisResult {
    RecursiveAnalysisResult recursive;
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<JumpTableAnalysis> jump_tables;
    std::vector<ResolvedControlFlowEdge> resolved_edges;
    std::size_t fixpoint_iterations = 0u;
    std::vector<AnalysisDirectiveDiagnostic> directive_diagnostics;
};

[[nodiscard]] const char* analysis_directive_diagnostic_status_name(
    AnalysisDirectiveDiagnosticStatus status
) noexcept;

[[nodiscard]] ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides = nullptr
);

}
