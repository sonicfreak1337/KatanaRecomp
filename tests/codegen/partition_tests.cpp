#include "katana/codegen/partition.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::ir::Function function(const std::uint32_t entry, const std::size_t instructions) {
    katana::ir::Function value;
    value.entry_address = entry;
    katana::ir::BasicBlock block;
    block.start_address = entry;
    block.instructions.resize(instructions);
    value.blocks.push_back(std::move(block));
    return value;
}

} // namespace

int main() {
    std::vector<katana::ir::Function> functions;
    for (std::uint32_t index = 0u; index < 257u; ++index) {
        functions.push_back(function(0x8C000000u + index * 0x20u, index % 7u + 1u));
    }
    std::mt19937 random(3301u);
    std::shuffle(functions.begin(), functions.end(), random);

    const katana::codegen::PartitionOptions options{32u, 96u};
    const auto first = katana::codegen::partition_translation_units(functions, options);
    std::vector<std::uint32_t> first_entries;
    for (const auto& partition : first) {
        require(
            partition.index < first.size() && !partition.function_indices.empty() &&
                partition.function_indices.size() <= options.maximum_functions &&
                partition.instruction_count <= options.maximum_instructions &&
                partition.first_entry_address <= partition.last_entry_address,
            "Partition verletzt Index-, Groessen- oder Adressvertrag."
        );
        for (const auto index : partition.function_indices) {
            first_entries.push_back(functions[index].entry_address);
        }
    }

    std::shuffle(functions.begin(), functions.end(), random);
    const auto second = katana::codegen::partition_translation_units(functions, options);
    std::vector<std::uint32_t> second_entries;
    for (const auto& partition : second) {
        for (const auto index : partition.function_indices) {
            second_entries.push_back(functions[index].entry_address);
        }
    }
    require(
        first_entries == second_entries && first_entries.size() == functions.size() &&
            std::is_sorted(first_entries.begin(), first_entries.end()) &&
            std::adjacent_find(first_entries.begin(), first_entries.end()) == first_entries.end(),
        "Partitionierung ist nicht reproduzierbar oder verliert/dupliziert Funktionen."
    );

    require(
        [] {
            const std::vector oversized = {function(0x8C100000u, 4097u)};
            try {
                static_cast<void>(katana::codegen::partition_translation_units(
                    oversized, {1u, 4096u}
                ));
            } catch (const std::length_error&) {
                return true;
            }
            return false;
        }(),
        "Einzelfunktion oberhalb des Instruktionslimits wird still akzeptiert."
    );
    require(
        [] {
            try {
                static_cast<void>(katana::codegen::partition_translation_units({}, {0u, 1u}));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        }(),
        "Null-Grenze wird akzeptiert."
    );

    std::cout << "KR-3301 Translation-Unit-Partitionierung erfolgreich.\n";
    return 0;
}
