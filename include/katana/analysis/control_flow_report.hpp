#pragma once

#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/value_analysis.hpp"

#include <span>
#include <string>

namespace katana::analysis {

[[nodiscard]] std::string format_indirect_control_flow_report(
    std::span<const IndirectControlFlowResolution> resolutions,
    std::span<const JumpTableAnalysis> jump_tables = {}
);

}
