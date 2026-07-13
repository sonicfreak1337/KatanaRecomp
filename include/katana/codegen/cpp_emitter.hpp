#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace katana::codegen {

[[nodiscard]] std::string emit_cpp_program(
    std::span<const katana::ir::Function> functions,
    std::uint32_t entry_address
);

}
