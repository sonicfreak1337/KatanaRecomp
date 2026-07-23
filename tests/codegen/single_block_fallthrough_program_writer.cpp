#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

katana::ir::Instruction simple_instruction(const std::uint32_t address,
                                           const katana::ir::Operation operation) {
    katana::ir::Instruction result;
    result.source_address = address;
    result.original_opcode = operation == katana::ir::Operation::AddImmediate ? 0x7001u : 0xE001u;
    result.original_operation = operation;
    result.operation = operation;
    result.widths = katana::ir::operation_operand_widths(operation);
    result.status_effects = katana::ir::instruction_status_effects(operation);
    result.memory_effects = katana::ir::instruction_memory_effects(operation);
    result.accumulator_effects = katana::ir::operation_accumulator_effects(operation);
    result.destination_register = 0u;
    result.immediate = 1;
    return result;
}

katana::ir::Function local_chain_function() {
    katana::ir::BasicBlock first;
    first.start_address = 0x2000u;
    first.instructions = {simple_instruction(0x2000u, katana::ir::Operation::MovImmediate)};
    first.successors = {0x2002u};

    katana::ir::BasicBlock second;
    second.start_address = 0x2002u;
    second.instructions = {simple_instruction(0x2002u, katana::ir::Operation::AddImmediate)};

    katana::ir::Function function;
    function.entry_address = first.start_address;
    function.blocks = {std::move(first), std::move(second)};
    return function;
}

std::vector<katana::ir::Function> build_program() {
    // Both addresses are function entries. The CFG therefore contains the ordinary 0x1000 ->
    // 0x1002 fallthrough, while lowering correctly omits it from the first function's
    // intraprocedural successor list. This is the shape used by external port partitions.
    constexpr std::array<std::uint8_t, 4u> bytes = {
        0x07u, 0xE3u, // mov #7,r3
        0x09u, 0x00u  // nop
    };
    constexpr std::array<std::uint32_t, 2u> seeds = {0x1000u, 0x1002u};
    const auto lines = katana::sh4::disassemble(bytes, 0x1000u);
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, discovered);
    if (program.size() != 2u || program.front().entry_address != 0x1000u ||
        program.front().blocks.size() != 1u ||
        program.front().blocks.front().instructions.size() != 1u ||
        !program.front().blocks.front().successors.empty()) {
        throw std::runtime_error(
            "Testfixture besitzt keinen extern abgeschnittenen Eininstruktions-Fallthroughblock.");
    }
    program.push_back(local_chain_function());
    return program;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc != 2) return EXIT_FAILURE;
    try {
        const auto program = build_program();
        const katana::codegen::CppBackend backend;
        katana::codegen::BackendRequest request{program, 0x1000u};
        request.symbol_namespace = "single_block_fallthrough_fixture";
        request.external_function_linkage = true;
        request.single_block_execution = true;
        request.external_dynamic_dispatch = true;
        request.guarded_local_block_chaining = true;
        const auto single_block_source =
            katana::codegen::generate_program(backend, request).joined_text();

        katana::codegen::BackendRequest normal_request{program, 0x1000u};
        normal_request.symbol_namespace = "normal_fallthrough_fixture";
        const auto normal_source =
            katana::codegen::generate_program(backend, normal_request).joined_text();

        constexpr std::string_view reset_dispatch_macros =
            "\n#undef static_call\n"
            "#undef resolved_call\n"
            "#undef guarded_call\n"
            "#undef guarded_jump\n"
            "#undef runtime_only_call\n"
            "#undef runtime_only_jump\n"
            "#undef unresolved_call\n"
            "#undef unresolved_jump\n\n";
        std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
        output.write(single_block_source.data(),
                     static_cast<std::streamsize>(single_block_source.size()));
        output.write(reset_dispatch_macros.data(),
                     static_cast<std::streamsize>(reset_dispatch_macros.size()));
        output.write(normal_source.data(), static_cast<std::streamsize>(normal_source.size()));
        return output ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
