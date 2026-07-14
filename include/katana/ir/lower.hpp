#pragma once

#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <span>
#include <vector>

namespace katana::ir {

[[nodiscard]] Function lower_function(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::analysis::FunctionInfo& function,
    std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges = {},
    std::span<const std::uint32_t> function_entries = {}
);

[[nodiscard]] std::vector<Function> lower_program(
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const katana::analysis::FunctionInfo> functions,
    std::span<const katana::analysis::ResolvedControlFlowEdge> resolved_edges = {}
);

[[nodiscard]] std::vector<Function> lower_program(
    const katana::analysis::ControlFlowAnalysisResult& analysis
);

}
