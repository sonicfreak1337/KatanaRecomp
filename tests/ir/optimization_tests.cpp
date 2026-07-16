#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 12> bytes = {
        0x7Fu,
        0xE1u, // MOV #127,R1
        0x7Fu,
        0x71u, // ADD #127,R1
        0x03u,
        0xE2u, // MOV #3,R2
        0x2Cu,
        0x31u, // ADD R2,R1
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, discovered);
    auto& function = program.front();

    const auto result = katana::ir::fold_constants(function);
    const auto& instructions = function.blocks.front().instructions;
    require(result.changes == 2u, "Constant Folding meldet eine falsche Aenderungszahl.");
    require(instructions[1].operation == katana::ir::Operation::Constant32 &&
                instructions[1].widths.immediate == katana::ir::OperandWidth::Bits32 &&
                static_cast<std::uint32_t>(instructions[1].immediate) == 254u &&
                instructions[3].operation == katana::ir::Operation::Constant32 &&
                instructions[3].widths.immediate == katana::ir::OperandWidth::Bits32 &&
                static_cast<std::uint32_t>(instructions[3].immediate) == 257u,
            "Konstante 32-Bit-Ausdruecke wurden nicht korrekt gefaltet.");
    require(katana::ir::verify_function(function).empty(),
            "Constant Folding erzeugt ungueltige Katana-IR.");

    constexpr std::array<std::uint8_t, 12> copy_bytes = {
        0x13u,
        0x62u, // MOV R1,R2
        0x2Cu,
        0x33u, // ADD R2,R3
        0x05u,
        0xE1u, // MOV #5,R1
        0x2Cu,
        0x34u, // ADD R2,R4
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto copy_lines = katana::sh4::disassemble(copy_bytes, 0x8C020000u);
    constexpr std::array<std::uint32_t, 1> copy_seeds = {0x8C020000u};
    const auto copy_discovered = katana::analysis::discover_functions(copy_lines, copy_seeds);
    auto copy_program = katana::ir::lower_program(copy_lines, copy_discovered);
    auto& copy_function = copy_program.front();
    const auto copy_result = katana::ir::propagate_copies(copy_function);
    const auto& copy_instructions = copy_function.blocks.front().instructions;
    require(copy_result.changes == 1u && copy_instructions[1].source_register == 1u &&
                copy_instructions[3].source_register == 2u,
            "Copy Propagation ersetzt Aliase ueber einen Quellschreibzugriff hinweg.");
    require(katana::ir::verify_function(copy_function).empty(),
            "Copy Propagation erzeugt ungueltige Katana-IR.");

    constexpr std::array<std::uint8_t, 10> dead_bytes = {
        0x01u,
        0xE1u, // MOV #1,R1 (Blockanfang bleibt erhalten)
        0x02u,
        0xE2u, // MOV #2,R2 (tot)
        0x03u,
        0xE2u, // MOV #3,R2
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto dead_lines = katana::sh4::disassemble(dead_bytes, 0x8C030000u);
    constexpr std::array<std::uint32_t, 1> dead_seeds = {0x8C030000u};
    const auto dead_discovered = katana::analysis::discover_functions(dead_lines, dead_seeds);
    auto dead_program = katana::ir::lower_program(dead_lines, dead_discovered);
    auto& dead_function = dead_program.front();
    const auto dead_result = katana::ir::eliminate_dead_code(dead_function);
    const auto& dead_instructions = dead_function.blocks.front().instructions;
    require(dead_result.changes == 1u && dead_instructions.size() == 4u &&
                dead_instructions[1].source_address == 0x8C030004u,
            "Dead-Code-Elimination entfernt keine eindeutig ueberschriebene Definition.");
    require(katana::ir::verify_function(dead_function).empty(),
            "Dead-Code-Elimination erzeugt ungueltige Katana-IR.");

    auto cfg_function = dead_function;
    katana::ir::Instruction unreachable_nop;
    unreachable_nop.source_address = 0x8C03FF00u;
    unreachable_nop.original_opcode = 0x0009u;
    unreachable_nop.operation = katana::ir::Operation::Nop;
    unreachable_nop.original_operation = katana::ir::Operation::Nop;
    unreachable_nop.widths = katana::ir::operation_operand_widths(unreachable_nop.operation);
    unreachable_nop.status_effects =
        katana::ir::instruction_status_effects(unreachable_nop.operation);
    unreachable_nop.memory_effects =
        katana::ir::instruction_memory_effects(unreachable_nop.operation);
    unreachable_nop.accumulator_effects =
        katana::ir::operation_accumulator_effects(unreachable_nop.operation);
    katana::ir::BasicBlock unreachable_block;
    unreachable_block.start_address = unreachable_nop.source_address;
    unreachable_block.instructions = {unreachable_nop};
    cfg_function.blocks.push_back(unreachable_block);
    require(katana::ir::verify_function(cfg_function).empty(),
            "CFG-Testaufbau ist keine gueltige Katana-IR.");
    const auto cfg_result = katana::ir::simplify_cfg(cfg_function);
    require(cfg_result.changes == 1u && cfg_function.blocks.size() == 1u,
            "CFG-Simplifizierung entfernt unerreichbaren Block nicht.");
    require(katana::ir::verify_function(cfg_function).empty(),
            "CFG-Simplifizierung erzeugt ungueltige Katana-IR.");

    constexpr std::array<std::uint8_t, 8> load_store_bytes = {
        0x22u,
        0x21u, // MOV.L R2,@R1
        0x12u,
        0x63u, // MOV.L @R1,R3
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto load_store_lines = katana::sh4::disassemble(load_store_bytes, 0x8C040000u);
    constexpr std::array<std::uint32_t, 1> load_store_seeds = {0x8C040000u};
    const auto load_store_discovered =
        katana::analysis::discover_functions(load_store_lines, load_store_seeds);
    auto load_store_program = katana::ir::lower_program(load_store_lines, load_store_discovered);
    auto& load_store_function = load_store_program.front();
    const auto mmio_result = katana::ir::simplify_load_store(load_store_function);
    require(mmio_result.changes == 0u &&
                !load_store_function.blocks.front().instructions[1].forwarded_value_register,
            "Unklassifizierter oder MMIO-Speicherzugriff wurde weitergeleitet.");
    load_store_function.blocks.front().instructions[0].memory_effects.region =
        katana::ir::MemoryRegionKind::NormalRam;
    load_store_function.blocks.front().instructions[1].memory_effects.region =
        katana::ir::MemoryRegionKind::NormalRam;
    const auto load_store_result = katana::ir::simplify_load_store(load_store_function);
    const auto& forwarded_load = load_store_function.blocks.front().instructions[1];
    require(load_store_result.changes == 1u && forwarded_load.forwarded_value_register == 2u &&
                forwarded_load.memory_effects.access == katana::ir::MemoryAccessKind::Read,
            "Load-Store-Pass leitet den Wert nicht weiter oder entfernt den Read-Effekt.");
    require(katana::ir::verify_function(load_store_function).empty(),
            "Load-Store-Vereinfachung erzeugt ungueltige Katana-IR.");
    const std::array<katana::ir::Function, 1> forwarded_program = {load_store_function};
    const auto forwarded_cpp =
        katana::codegen::emit_cpp_program(forwarded_program, load_store_function.entry_address);
    require(forwarded_cpp.find("static_cast<void>(cpu.memory.read_u32(cpu.r[1]));") !=
                    std::string::npos &&
                forwarded_cpp.find("cpu.r[3] = forwarded_value;") != std::string::npos,
            "Codegen erhaelt den Speicher-Read beim Load-Forwarding nicht.");

    auto pipeline_program = katana::ir::lower_program(lines, discovered);
    katana::ir::OptimizationOptions pipeline_options;
    pipeline_options.copy_propagation = false;
    pipeline_options.dead_code_elimination = false;
    pipeline_options.cfg_simplification = false;
    pipeline_options.load_store_simplification = false;
    pipeline_options.capture_dumps = true;
    const auto pipeline_report = katana::ir::optimize_program(pipeline_program, pipeline_options);
    require(pipeline_report.passes.size() == 1u &&
                pipeline_report.passes.front().name == "constant-folding" &&
                pipeline_report.passes.front().changes == 2u &&
                pipeline_report.total_changes == 2u &&
                pipeline_report.passes.front().before != pipeline_report.passes.front().after,
            "Pass-Pipeline respektiert Einzelschalter oder Dumps nicht.");

    auto disabled_program = katana::ir::lower_program(lines, discovered);
    const auto disabled_before = katana::ir::emit_ir_text(disabled_program);
    katana::ir::OptimizationOptions disabled_options;
    disabled_options.enabled = false;
    disabled_options.capture_dumps = true;
    const auto disabled_report = katana::ir::optimize_program(disabled_program, disabled_options);
    require(disabled_report.passes.empty() && disabled_report.total_changes == 0u &&
                katana::ir::emit_ir_text(disabled_program) == disabled_before,
            "Globaler Debug-Schalter deaktiviert nicht alle Optimierungen.");

    std::cout << "Katana-IR-Optimierungen erfolgreich.\n";
    return EXIT_SUCCESS;
}
