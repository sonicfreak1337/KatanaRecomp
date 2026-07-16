#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace katana::ir {

[[nodiscard]] std::string emit_ir_text(std::span<const Function> functions);

[[nodiscard]] std::string emit_ir_json(std::span<const Function> functions);

[[nodiscard]] std::string
emit_ir_fragment_json(std::span<const Function> functions,
                      std::span<const std::uint32_t> external_function_entries);

} // namespace katana::ir
