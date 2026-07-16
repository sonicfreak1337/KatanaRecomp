#pragma once

#include "katana/analysis/control_flow_analysis.hpp"

#include <span>
#include <string>

namespace katana::analysis {

[[nodiscard]] std::string
format_indirect_control_flow_report(std::span<const IndirectControlFlowResolution> resolutions,
                                    std::span<const JumpTableAnalysis> jump_tables = {},
                                    std::span<const SymbolicAddress> symbols = {});

[[nodiscard]] std::string
format_control_flow_analysis_json(const ControlFlowAnalysisResult& analysis);

} // namespace katana::analysis
