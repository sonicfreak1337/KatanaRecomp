#pragma once

#include "katana/ir/ir.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace katana::ir {

struct OptimizationResult {
    std::size_t changes = 0u;
};

struct OptimizationOptions {
    bool enabled = true;
    bool constant_folding = true;
    bool copy_propagation = true;
    bool dead_code_elimination = true;
    bool cfg_simplification = true;
    bool load_store_simplification = false;
    bool capture_dumps = false;
};

struct OptimizationPassReport {
    std::string name;
    std::size_t changes = 0u;
    std::string before;
    std::string after;
};

struct OptimizationPipelineReport {
    std::vector<OptimizationPassReport> passes;
    std::size_t total_changes = 0u;
};

[[nodiscard]] OptimizationResult fold_constants(Function& function);
[[nodiscard]] OptimizationResult propagate_copies(Function& function);
[[nodiscard]] OptimizationResult eliminate_dead_code(Function& function);
[[nodiscard]] OptimizationResult simplify_cfg(Function& function);
[[nodiscard]] OptimizationResult simplify_load_store(Function& function);
[[nodiscard]] OptimizationPipelineReport optimize_program(
    std::vector<Function>& program,
    const OptimizationOptions& options = {}
);

}
