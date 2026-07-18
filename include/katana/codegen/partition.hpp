#pragma once

#include "katana/ir/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::codegen {

struct PartitionOptions {
    std::size_t maximum_functions = 128u;
    std::size_t maximum_instructions = 4096u;
};

struct TranslationUnitPartition {
    std::size_t index = 0u;
    std::uint32_t first_entry_address = 0u;
    std::uint32_t last_entry_address = 0u;
    std::size_t instruction_count = 0u;
    std::vector<std::size_t> function_indices;
    std::string content_sha256;

    [[nodiscard]] bool operator==(const TranslationUnitPartition&) const = default;
};

[[nodiscard]] std::vector<TranslationUnitPartition>
partition_translation_units(std::span<const katana::ir::Function> functions,
                            const PartitionOptions& options = {});

[[nodiscard]] std::vector<std::size_t>
changed_translation_unit_partitions(std::span<const TranslationUnitPartition> current,
                                    std::span<const TranslationUnitPartition> previous);

} // namespace katana::codegen
