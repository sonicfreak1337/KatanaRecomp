#pragma once

#include "katana/codegen/partition.hpp"

#include <span>
#include <string>
#include <string_view>

namespace katana::codegen {

[[nodiscard]] std::string deterministic_translation_unit_name(
    const TranslationUnitPartition& partition,
    std::span<const katana::ir::Function> functions,
    std::string_view suffix = ".cpp"
);

} // namespace katana::codegen
