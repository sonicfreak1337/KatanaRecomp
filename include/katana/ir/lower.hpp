#pragma once

#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <span>
#include <vector>

namespace katana::ir {

[[nodiscard]] Function lower_function(
    std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::analysis::FunctionInfo& function
);

[[nodiscard]] std::vector<Function> lower_program(
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const katana::analysis::FunctionInfo> functions
);

}
