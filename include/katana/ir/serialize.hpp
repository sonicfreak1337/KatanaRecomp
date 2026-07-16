#pragma once

#include "katana/ir/ir.hpp"

#include <span>
#include <string>

namespace katana::ir {

[[nodiscard]] std::string emit_ir_text(std::span<const Function> functions);

[[nodiscard]] std::string emit_ir_json(std::span<const Function> functions);

} // namespace katana::ir
