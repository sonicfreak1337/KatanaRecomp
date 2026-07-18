#include "katana/codegen/partition.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/ir/serialize.hpp"

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
    std::vector<std::uint32_t> function_entries;
    function_entries.reserve(functions.size());
    for (const auto function_index : order)
        function_entries.push_back(functions[function_index].entry_address);
    for (auto& partition : result) {
        std::vector<katana::ir::Function> partition_functions;
        partition_functions.reserve(partition.function_indices.size());
        for (const auto function_index : partition.function_indices) {
            partition_functions.push_back(functions[function_index]);
        }
        partition.content_sha256 = katana::io::sha256_bytes(
            katana::ir::emit_ir_fragment_json(partition_functions, function_entries));
    }
    return result;
}

std::vector<std::size_t>
changed_translation_unit_partitions(const std::span<const TranslationUnitPartition> current,
                                    const std::span<const TranslationUnitPartition> previous) {
    std::vector<std::size_t> changed;
    for (std::size_t index = 0u; index < current.size(); ++index) {
        if (index >= previous.size() ||
            current[index].content_sha256 != previous[index].content_sha256 ||
            current[index].first_entry_address != previous[index].first_entry_address ||
            current[index].last_entry_address != previous[index].last_entry_address) {
            changed.push_back(index);
        }
    }
    return changed;
}

} // namespace katana::codegen
