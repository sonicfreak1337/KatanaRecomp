#include "katana/codegen/partition.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
        require(partition.index < first.size() && !partition.function_indices.empty() &&
                    partition.function_indices.size() <= options.maximum_functions &&
                    partition.instruction_count <= options.maximum_instructions &&
                    partition.first_entry_address <= partition.last_entry_address,
                "Partition verletzt Index-, Groessen- oder Adressvertrag.");
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
    require(first_entries == second_entries && first_entries.size() == functions.size() &&
                std::is_sorted(first_entries.begin(), first_entries.end()) &&
                std::adjacent_find(first_entries.begin(), first_entries.end()) ==
                    first_entries.end(),
            "Partitionierung ist nicht reproduzierbar oder verliert/dupliziert Funktionen.");
    require(first.size() == second.size() &&
                std::equal(first.begin(), first.end(), second.begin(), [](const auto& left,
                                                                         const auto& right) {
                    return left.content_sha256 == right.content_sha256;
                }) &&
                katana::codegen::changed_translation_unit_partitions(second, first).empty(),
            "Stabile Partitionen verlieren ihren inhaltsadressierten Bulk-Codegenstatus.");
    auto changed_functions = functions;
    changed_functions.front().blocks.front().instructions.front().original_opcode = 0x0009u;
    const auto changed =
        katana::codegen::partition_translation_units(changed_functions, options);
    require(katana::codegen::changed_translation_unit_partitions(changed, second).size() == 1u,
            "Lokale IR-Aenderung invalidiert mehr als die betroffene Codegenpartition.");

    require(
        [] {
            const std::vector oversized = {function(0x8C100000u, 4097u)};
            try {
                static_cast<void>(
                    katana::codegen::partition_translation_units(oversized, {1u, 4096u}));
            } catch (const std::length_error&) {
                return true;
            }
            return false;
        }(),
        "Einzelfunktion oberhalb des Instruktionslimits wird still akzeptiert.");
    require(
        [] {
            try {
                static_cast<void>(katana::codegen::partition_translation_units({}, {0u, 1u}));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        }(),
        "Null-Grenze wird akzeptiert.");

    for (const auto [count, budget] :
         std::array<std::pair<std::uint32_t, std::chrono::seconds>, 3u>{
             std::pair{10'000u, std::chrono::seconds(30)},
             std::pair{50'000u, std::chrono::seconds(60)},
             std::pair{100'000u, std::chrono::seconds(120)}}) {
        std::vector<katana::ir::Function> scale;
        scale.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            scale.push_back(function(0x1000u + index * 4u, 1u));
        }
        const auto started = std::chrono::steady_clock::now();
        const auto partitions =
            katana::codegen::partition_translation_units(scale, {256u, 4096u});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::size_t covered = 0u;
        for (const auto& partition : partitions) covered += partition.function_indices.size();
        require(covered == count && elapsed < budget &&
                    partitions.size() <= (count + 255u) / 256u,
                "10k/50k/100k-Bulk-Codegenfixture sprengt Zeit- oder Partitionsbudget.");
    }

    std::cout << "KR-3301 Translation-Unit-Partitionierung erfolgreich.\n";
    return 0;
}
