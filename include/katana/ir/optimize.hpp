#pragma once

#include "katana/ir/ir.hpp"

#include <cstddef>

namespace katana::ir {

struct OptimizationResult {
    std::size_t changes = 0u;
};

[[nodiscard]] OptimizationResult fold_constants(Function& function);
[[nodiscard]] OptimizationResult propagate_copies(Function& function);
[[nodiscard]] OptimizationResult eliminate_dead_code(Function& function);
[[nodiscard]] OptimizationResult simplify_cfg(Function& function);
[[nodiscard]] OptimizationResult simplify_load_store(Function& function);

}
