#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    constexpr std::array<std::uint8_t, 16> bytes = {0x02,
                                                    0xB0,
                                                    0x07,
                                                    0xE2,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00,
                                                    0x05,
                                                    0xE1,
                                                    0xFF,
                                                    0x71,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto discovered = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, discovered);

    require(program.size() == 2, "Das Codegen-Testprogramm muss zwei Funktionen enthalten.");

    require(program[0].blocks[0].successors.size() == 1,
            "Ein Call-Block darf intraprozedural nur den Rueckkehrpfad besitzen.");

    require(program[0].blocks[0].successors[0] == 0x8C010004u,
            "Der intraprozedurale Call-Nachfolger ist falsch.");

    const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);

    require(source.find("#include \"katana/runtime/exception.hpp\"") != std::string::npos &&
                source.find("#include \"katana/runtime/runtime.hpp\"") != std::string::npos,
            "Der generierte Code bindet Exception-Pfad oder Runtime nicht ein.");
    require(source.find("using CpuState = katana::runtime::CpuState;") != std::string::npos &&
                source.find("using Memory = katana::runtime::Memory;") != std::string::npos,
            "Der generierte Kompatibilitaets-Namespace fehlt.");
    require(source.find("struct CpuState") == std::string::npos &&
                source.find("class Memory") == std::string::npos,
            "Der generierte Code enthaelt weiterhin eine Runtime-Implementierung.");
    require(source.find("required_runtime_abi = 8u") != std::string::npos,
            "Der generierte Code prueft die Runtime-ABI nicht.");

    require(source.find("fn_8C010000") != std::string::npos,
            "Die generierte Einstiegsfunktion fehlt.");

    require(source.find("fn_8C010008") != std::string::npos, "Die generierte Unterfunktion fehlt.");

    const auto delay_position = source.find("cpu.r[2] = static_cast<std::uint32_t>(7);");

    const auto call_position = source.find("fn_8C010008_with_services(cpu, services);");

    require(delay_position != std::string::npos,
            "Die Delay-Slot-Instruktion fehlt im generierten Code.");

    require(call_position != std::string::npos,
            "Der direkte Funktionsaufruf fehlt im generierten Code.");

    require(delay_position < call_position,
            "Der Delay Slot muss vor dem Funktionsaufruf ausgefuehrt werden.");

    const std::vector<std::uint32_t> global_entries{0x8C010000u, 0x8C010008u};
    const katana::codegen::CppBackend partition_backend;
    const std::array caller_partition{program[0]};
    const auto caller_unit =
        partition_backend
            .emit(
                {caller_partition, 0x8C010000u, {}, global_entries, "partitioned_game", true, true})
            .joined_text();
    const std::array callee_partition{program[1]};
    const auto callee_unit = partition_backend
                                 .emit({callee_partition,
                                        0x8C010008u,
                                        {},
                                        global_entries,
                                        "partitioned_game",
                                        false,
                                        true})
                                 .joined_text();
    require(caller_unit.find("namespace partitioned_game") != std::string::npos &&
                caller_unit.find("void fn_8C010008_with_services") != std::string::npos &&
                caller_unit.find("fn_8C010008_with_services(cpu, services);") !=
                    std::string::npos &&
                caller_unit.find("unresolved_call(cpu, 0x8C010008u)") == std::string::npos &&
                callee_unit.find("void fn_8C010008_with_services") != std::string::npos,
            "Partitionsuebergreifender Call besitzt keinen externen Symbolvertrag.");

    require(source.find("cpu.r[1] += static_cast<std::uint32_t>(-1);") != std::string::npos,
            "ADD Immediate wurde nicht generiert.");

    require(source.find("const std::uint32_t return_target = cpu.pr;") != std::string::npos &&
                source.find("cpu.pc = return_target;") != std::string::npos,
            "Die Ruecksprungsemantik fehlt.");

    constexpr std::array<std::uint8_t, 12> indirect_jump_bytes = {
        0x08u,
        0xE1u, // MOV #8,R1
        0x2Bu,
        0x41u, // JMP @R1
        0x09u,
        0x00u, // NOP (Delay Slot)
        0x09u,
        0x00u, // unerreichbarer Abstand
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto indirect_jump_lines = katana::sh4::disassemble(indirect_jump_bytes, 0u);
    constexpr std::array<std::uint32_t, 1> indirect_jump_seeds = {0u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1> indirect_jump_edges = {
        katana::analysis::ResolvedControlFlowEdge{
            2u, 8u, katana::analysis::ResolvedControlFlowKind::Jump}};
    const auto indirect_jump_functions = katana::analysis::discover_functions(
        indirect_jump_lines, indirect_jump_seeds, indirect_jump_edges);
    const auto indirect_jump_program = katana::ir::lower_program(
        indirect_jump_lines, indirect_jump_functions, indirect_jump_edges);
    const auto indirect_jump_source = katana::codegen::emit_cpp_program(indirect_jump_program, 0u);
    require(indirect_jump_source.find("switch (jump_target)") != std::string::npos &&
                indirect_jump_source.find("case 0x00000008u:") != std::string::npos &&
                indirect_jump_source.find("cpu.pc = 0x00000008u;") != std::string::npos,
            "Aufgeloestes indirektes JMP wird nicht als nativer Dispatch generiert.");

    constexpr std::array<std::uint8_t, 18> relative_jump_bytes = {
        0x08u,
        0xE0u, // MOV #8,R0
        0x23u,
        0x00u, // BRAF R0: PC+4+8 = 0x0000000E
        0x09u,
        0x00u, // NOP (Delay Slot)
        0x09u,
        0x00u,
        0x09u,
        0x00u,
        0x09u,
        0x00u,
        0x09u,
        0x00u,
        0x0Bu,
        0x00u,
        0x09u,
        0x00u};
    const auto relative_jump_lines = katana::sh4::disassemble(relative_jump_bytes, 0u);
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1> relative_jump_edges = {
        katana::analysis::ResolvedControlFlowEdge{
            2u, 14u, katana::analysis::ResolvedControlFlowKind::Jump}};
    const auto relative_jump_functions = katana::analysis::discover_functions(
        relative_jump_lines, indirect_jump_seeds, relative_jump_edges);
    const auto relative_jump_program = katana::ir::lower_program(
        relative_jump_lines, relative_jump_functions, relative_jump_edges);
    const auto relative_jump_source = katana::codegen::emit_cpp_program(relative_jump_program, 0u);
    require(relative_jump_source.find("jump_target = cpu.r[0] + 0x00000006u") !=
                    std::string::npos &&
                relative_jump_source.find("case 0x0000000Eu:") != std::string::npos,
            "BRAF verliert PC+4+Rm-Zielbildung zwischen IR und C++-Backend.");

    constexpr std::array<std::uint8_t, 16> indirect_call_bytes = {
        0x0Cu,
        0xE1u, // MOV #12,R1
        0x0Bu,
        0x41u, // JSR @R1
        0x09u,
        0x00u, // NOP (Delay Slot)
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u, // NOP (Delay Slot)
        0x09u,
        0x00u, // unerreichbarer Abstand
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto indirect_call_lines = katana::sh4::disassemble(indirect_call_bytes, 0u);
    constexpr std::array<std::uint32_t, 1> indirect_call_seeds = {0u};
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1> indirect_call_edges = {
        katana::analysis::ResolvedControlFlowEdge{
            2u, 12u, katana::analysis::ResolvedControlFlowKind::Call}};
    const auto indirect_call_functions = katana::analysis::discover_functions(
        indirect_call_lines, indirect_call_seeds, indirect_call_edges);
    const auto indirect_call_program = katana::ir::lower_program(
        indirect_call_lines, indirect_call_functions, indirect_call_edges);
    const auto indirect_call_source = katana::codegen::emit_cpp_program(indirect_call_program, 0u);
    require(indirect_call_source.find("switch (call_target)") != std::string::npos &&
                indirect_call_source.find("case 0x0000000Cu:") != std::string::npos &&
                indirect_call_source.find("fn_0000000C_with_services(cpu, services);") !=
                    std::string::npos,
            "Aufgeloestes indirektes JSR wird nicht als nativer Funktionsdispatch generiert.");

    constexpr std::array<std::uint8_t, 10> delay_memory_bytes = {
        0x01u,
        0xA0u, // BRA +1
        0x12u,
        0x62u, // MOV.L @R1,R2 (Delay Slot)
        0x09u,
        0x00u, // unerreichbarer Abstand
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto delay_memory_lines = katana::sh4::disassemble(delay_memory_bytes, 0x8C020000u);
    constexpr std::array<std::uint32_t, 1> delay_memory_seeds = {0x8C020000u};
    const auto delay_memory_functions =
        katana::analysis::discover_functions(delay_memory_lines, delay_memory_seeds);
    const auto delay_memory_program =
        katana::ir::lower_program(delay_memory_lines, delay_memory_functions);
    const auto delay_memory_source =
        katana::codegen::emit_cpp_program(delay_memory_program, 0x8C020000u);
    require(delay_memory_source.find("catch (const katana::runtime::MemoryAccessError& error)") !=
                    std::string::npos &&
                delay_memory_source.find(
                    "enter_memory_exception(cpu, error, 0x8C020002u, 0x8C020000u);") !=
                    std::string::npos,
            "Speicherfehler im Delay Slot verlieren ihren Owner-PC.");

    std::cout << "Alle C++-Codegenerator-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
