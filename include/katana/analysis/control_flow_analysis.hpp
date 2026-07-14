#pragma once

#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace katana::analysis {

struct ControlFlowAnalysisResult {
    RecursiveAnalysisResult recursive;
    std::vector<IndirectControlFlowResolution> indirect_control_flow;
    std::vector<JumpTableAnalysis> jump_tables;
    std::size_t fixpoint_iterations = 0u;
};

[[nodiscard]] ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides = nullptr
);

}
