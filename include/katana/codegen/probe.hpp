#pragma once

#include "katana/ir/ir.hpp"

namespace katana::codegen {

[[nodiscard]] bool block_requires_call_dispatch(const katana::ir::BasicBlock& block) noexcept;

} // namespace katana::codegen
