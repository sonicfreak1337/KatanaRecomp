#pragma once

#include "katana/ir/ir.hpp"

#include <cstddef>

namespace katana::ir {

struct OptimizationResult {
    std::size_t changes = 0u;
};

[[nodiscard]] OptimizationResult fold_constants(Function& function);
[[nodiscard]] OptimizationResult propagate_copies(Function& function);

}
