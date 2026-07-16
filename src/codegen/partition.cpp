#include "katana/codegen/partition.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>

namespace katana::codegen {
namespace {

std::size_t instruction_count(const katana::ir::Function& function) {
    return std::accumulate(function.blocks.begin(),
                           function.blocks.end(),
                           std::size_t{0u},
                           [](const std::size_t total, const katana::ir::BasicBlock& block) {
                               return total + block.instructions.size();
                           });
}

} // namespace

std::vector<TranslationUnitPartition>
partition_translation_units(const std::span<const katana::ir::Function> functions,
                            const PartitionOptions& options) {
    if (options.maximum_functions == 0u || options.maximum_instructions == 0u) {
        throw std::invalid_argument("Codegen-Partitionsgrenzen duerfen nicht null sein.");
    }
    std::vector<std::size_t> order(functions.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
        return functions[left].entry_address < functions[right].entry_address;
    });
    for (std::size_t index = 1u; index < order.size(); ++index) {
        if (functions[order[index - 1u]].entry_address == functions[order[index]].entry_address) {
            throw std::invalid_argument(
                "Codegen-Partitionierung erhielt doppelte Funktionseinstiege.");
        }
    }

    std::vector<TranslationUnitPartition> result;
    for (const auto function_index : order) {
        const auto count = instruction_count(functions[function_index]);
        if (count > options.maximum_instructions) {
            throw std::length_error(
                "Funktion an Gastadresse " +
                std::to_string(functions[function_index].entry_address) +
                " ueberschreitet das Instruktionslimit einer Translation Unit.");
        }
        const bool needs_partition =
            result.empty() || result.back().function_indices.size() >= options.maximum_functions ||
            (result.back().instruction_count != 0u &&
             count > options.maximum_instructions -
                         std::min(result.back().instruction_count, options.maximum_instructions));
        if (needs_partition) {
            result.push_back({result.size()});
        }
        auto& partition = result.back();
        partition.function_indices.push_back(function_index);
        partition.instruction_count += count;
        const auto entry = functions[function_index].entry_address;
        if (partition.function_indices.size() == 1u) {
            partition.first_entry_address = entry;
        }
        partition.last_entry_address = entry;
    }
    return result;
}

} // namespace katana::codegen
