#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>

namespace katana::ir {

[[nodiscard]] std::string emit_ir_text(std::span<const Function> functions);

[[nodiscard]] std::string emit_ir_json(std::span<const Function> functions);

[[nodiscard]] std::string
emit_ir_fragment_json(std::span<const Function> functions,
                      std::span<const std::uint32_t> external_function_entries);

// Streams the canonical IR fragment directly into SHA-256 without copying
// Function bodies or materialising the JSON document. Every pointer must
// refer to a function in one already verified program; this narrow contract
// lets partitioning validate the complete program once and hash many views.
[[nodiscard]] std::string emit_validated_ir_fragment_sha256(
    std::span<const Function* const> functions,
    std::span<const std::uint32_t> external_function_entries);

} // namespace katana::ir
