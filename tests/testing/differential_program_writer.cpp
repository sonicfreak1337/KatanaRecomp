#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/testing/differential_execution.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> program_bytes(const katana::testing::DifferentialProgram& program) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve((program.opcodes.size() + 2u) * 2u);
    for (const auto opcode : program.opcodes) {
        bytes.push_back(static_cast<std::uint8_t>(opcode));
        bytes.push_back(static_cast<std::uint8_t>(opcode >> 8u));
    }
    bytes.insert(bytes.end(), {0x0Bu, 0x00u, 0x09u, 0x00u}); // rts; nop
    return bytes;
}

std::string emit_program(const katana::testing::DifferentialProgram& program,
                         const std::size_t index) {
    katana::io::ExecutableImage image("synthetic-differential-program");
    auto bytes = program_bytes(program);
    image.add_segment({".text",
                       program.entry_pc,
                       0u,
                       bytes.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::move(bytes)});
    image.add_entry_point(program.entry_pc);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    if (!analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Differentialprogramm enthaelt unbekannte Instruktionen.");
    }
    const auto ir = katana::ir::lower_program(analysis);
    katana::ir::require_valid_program(ir);
    const auto name = "katana_differential_generated_" + std::to_string(index);
    const katana::codegen::CppBackend backend;
    return backend.emit({ir, program.entry_pc, {}, {}, name, true, false}).joined_text();
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc != 2) return EXIT_FAILURE;
    try {
        const auto programs = katana::testing::differential_regression_programs();
        std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
        if (!output) return EXIT_FAILURE;
        for (std::size_t index = 0u; index < programs.size(); ++index) {
            output << emit_program(programs[index], index) << '\n';
        }
        output << "namespace katana::testing {\n"
                  "void run_generated_differential_program(const std::size_t index, "
                  "katana::runtime::CpuState& cpu) {\n"
                  "  switch (index) {\n";
        for (std::size_t index = 0u; index < programs.size(); ++index) {
            output << "  case " << index << "u: katana_differential_generated_" << index
                   << "::run(cpu); return;\n";
        }
        output << "  default: throw std::out_of_range(\"Differentialprogrammindex\");\n"
                  "  }\n}\n} // namespace katana::testing\n";
        return output ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
