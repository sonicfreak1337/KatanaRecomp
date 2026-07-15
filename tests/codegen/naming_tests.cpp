#include "katana/codegen/naming.hpp"

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

katana::ir::Function function(const std::uint32_t entry, const std::uint16_t opcode) {
    katana::ir::Function value;
    value.entry_address = entry;
    katana::ir::BasicBlock block;
    block.start_address = entry;
    katana::ir::Instruction instruction;
    instruction.source_address = entry;
    instruction.original_opcode = opcode;
    instruction.original_operation = katana::ir::Operation::Nop;
    instruction.operation = katana::ir::Operation::Nop;
    block.instructions.push_back(instruction);
    value.blocks.push_back(std::move(block));
    return value;
}

std::vector<std::string> names(const std::vector<katana::ir::Function>& functions) {
    const auto partitions = katana::codegen::partition_translation_units(functions, {2u, 8u});
    std::vector<std::string> result;
    for (const auto& partition : partitions) {
        result.push_back(katana::codegen::deterministic_translation_unit_name(
            partition, functions
        ));
    }
    return result;
}

} // namespace

int main() {
    std::vector functions = {
        function(0x8C002000u, 0x0009u),
        function(0x8C001000u, 0x0009u),
        function(0x8C003000u, 0x0009u)
    };
    const auto first = names(functions);
    std::mt19937 random(3302u);
    std::shuffle(functions.begin(), functions.end(), random);
    const auto second = names(functions);
    require(
        first == second && first.size() == 2u &&
            first.front().starts_with("unit-00000-v8C001000-8C002000-") &&
            first.front().ends_with(".cpp") &&
            first.front().find(':') == std::string::npos &&
            first.front().find('\\') == std::string::npos,
        "Dateinamen haengen von Eingabereihenfolge oder Hostpfaden ab."
    );

    functions.front().blocks.front().instructions.front().original_opcode = 0xFFFFu;
    require(names(functions) != second, "IR-Inhaltsaenderung behaelt denselben Dateinamen.");

    const auto partition = katana::codegen::partition_translation_units(functions).front();
    bool rejected = false;
    try {
        static_cast<void>(katana::codegen::deterministic_translation_unit_name(
            partition, functions, "../cpp"
        ));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Pfadtragende Dateiendung wird akzeptiert.");

    std::cout << "KR-3302 deterministische Dateinamen erfolgreich.\n";
    return 0;
}
