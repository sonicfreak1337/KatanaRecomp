#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
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
    require(source.find("required_runtime_abi = " + std::to_string(katana::runtime::abi_version) +
                        "u") != std::string::npos,
            "Der generierte Code prueft die Runtime-ABI nicht.");
    require(source.find("switch (katana::runtime::unrelocate_code_address(cpu.pc))") !=
                std::string::npos,
            "Der Funktionsswitch normalisiert keine relokierte Runtime-PC-Adresse.");
    require(source.find("base_guest_cycles_per_instruction * 2u") != std::string::npos,
            "Owner und Delay Slot werden nicht als zwei Gastinstruktionen berechnet.");

    require(source.find("fn_8C010000") != std::string::npos,
            "Die generierte Einstiegsfunktion fehlt.");

    require(source.find("case 0xAC010000u:") != std::string::npos,
            "Der native Block akzeptiert seinen direkten SH-4-P1/P2-Alias nicht.");

    require(source.find("fn_8C010008") != std::string::npos, "Die generierte Unterfunktion fehlt.");

    const auto delay_position = source.find("cpu.r[2] = static_cast<std::uint32_t>(7);");

    const auto call_position = source.find("fn_8C010008_with_services(cpu, services);");

    require(delay_position != std::string::npos,
            "Die Delay-Slot-Instruktion fehlt im generierten Code.");

    require(call_position != std::string::npos,
            "Der direkte Funktionsaufruf fehlt im generierten Code.");

    require(delay_position < call_position,
            "Der Delay Slot muss vor dem Funktionsaufruf ausgefuehrt werden.");
    require(source.find("cpu.pr = katana::runtime::relocate_code_address(0x8C010004u);") !=
                    std::string::npos &&
                source.find("cpu.pc = katana::runtime::relocate_code_address(0x8C010008u);") !=
                    std::string::npos,
            "BSR relokiert Ziel oder Rueckkehradresse nicht mit seinem Codetemplate.");
    require(source.find("cpu.pc = 0x8C010000u;") != std::string::npos,
            "Der normale Programmeinstieg wurde unnoetig in den Relokationskontext gezogen.");

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

    constexpr std::array<std::uint8_t, 10> local_loop_bytes = {
        0x01u, 0xE0u, // MOV #1,R0
        0x10u, 0x40u, // DT R0
        0xFDu, 0x8Bu, // BF 0x8C010002
        0x0Bu, 0x00u, // RTS
        0x09u, 0x00u  // NOP (Delay Slot)
    };
    const auto local_loop_lines = katana::sh4::disassemble(local_loop_bytes, 0x8C010000u);
    const auto local_loop_functions =
        katana::analysis::discover_functions(local_loop_lines, seeds);
    const auto local_loop_program =
        katana::ir::lower_program(local_loop_lines, local_loop_functions);
    katana::codegen::BackendRequest local_chain_request{local_loop_program, 0x8C010000u};
    local_chain_request.single_block_execution = true;
    local_chain_request.guarded_local_block_chaining = true;
    local_chain_request.external_dynamic_dispatch = true;
    const katana::codegen::CppBackend local_chain_backend;
    const auto local_chain_source = local_chain_backend.emit(local_chain_request).joined_text();
    constexpr std::string_view block_note = "                note_block_entry(";
    const auto first_block_note = local_chain_source.find(block_note);
    const auto local_chain_transition =
        local_chain_source.find("services->can_chain_executable_block(cpu.pc)) continue;");
    const auto next_block_note = local_chain_source.find(block_note, first_block_note + 1u);
    require(first_block_note != std::string::npos && local_chain_transition != std::string::npos &&
                next_block_note != std::string::npos && first_block_note < local_chain_transition &&
                local_chain_transition < next_block_note &&
                local_chain_source.find(
                    "note_block_entry(katana::runtime::relocate_code_address(0x8C010000u));") !=
                    std::string::npos &&
                local_chain_source.find("++cpu.retired_guest_instructions;") != std::string::npos &&
                local_chain_source.find("services->consume_guest_cycles(") == std::string::npos &&
                local_chain_source.find("cpu.pc = !cpu.t ? "
                                        "katana::runtime::relocate_code_address(0x8C010002u) : "
                                        "katana::runtime::relocate_code_address(0x8C010006u);") !=
                    std::string::npos,
            "Lokales Mehrblock-Chaining besitzt Vorab-Timing oder keinen Retirement-Guard.");

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
    require(indirect_jump_source.find("const std::uint32_t jump_target = cpu.r[1];") !=
                    std::string::npos &&
                indirect_jump_source.find(
                    "switch (katana::runtime::unrelocate_code_address(jump_target))") !=
                    std::string::npos &&
                indirect_jump_source.find("case 0x00000008u:") != std::string::npos &&
                indirect_jump_source.find("cpu.pc = jump_target;") != std::string::npos &&
                indirect_jump_source.find(
                    "jump_target = katana::runtime::relocate_code_address(cpu.r[1])") ==
                    std::string::npos,
            "Aufgeloestes absolutes JMP veraendert sein Registerziel oder verliert nativen "
            "Dispatch.");

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
    require(relative_jump_source.find("jump_target = cpu.r[0] + "
                                      "katana::runtime::relocate_code_address(0x00000006u)") !=
                    std::string::npos &&
                relative_jump_source.find("case 0x0000000Eu:") != std::string::npos,
            "BRAF verliert die relokierbare PC+4+Rm-Zielbildung zwischen IR und C++-Backend.");

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
    require(indirect_call_source.find("const std::uint32_t call_target = cpu.r[1];") !=
                    std::string::npos &&
                indirect_call_source.find(
                    "switch (katana::runtime::unrelocate_code_address(call_target))") !=
                    std::string::npos &&
                indirect_call_source.find("case 0x0000000Cu:") != std::string::npos &&
                indirect_call_source.find("fn_0000000C_with_services(cpu, services);") !=
                    std::string::npos &&
                indirect_call_source.find(
                    "cpu.pr = katana::runtime::relocate_code_address(0x00000006u);") !=
                    std::string::npos &&
                indirect_call_source.find(
                    "call_target = katana::runtime::relocate_code_address(cpu.r[1])") ==
                    std::string::npos,
            "Aufgeloestes absolutes JSR veraendert sein Registerziel oder verliert nativen "
            "Funktionsdispatch.");

    auto relative_call_bytes = relative_jump_bytes;
    relative_call_bytes[2] = 0x03u; // BSRF R0: PC+4+8 = 0x0000000E
    const auto relative_call_lines = katana::sh4::disassemble(relative_call_bytes, 0u);
    const std::array<katana::analysis::ResolvedControlFlowEdge, 1> relative_call_edges = {
        katana::analysis::ResolvedControlFlowEdge{
            2u, 14u, katana::analysis::ResolvedControlFlowKind::Call}};
    const auto relative_call_functions = katana::analysis::discover_functions(
        relative_call_lines, indirect_jump_seeds, relative_call_edges);
    const auto relative_call_program = katana::ir::lower_program(
        relative_call_lines, relative_call_functions, relative_call_edges);
    const auto relative_call_source = katana::codegen::emit_cpp_program(relative_call_program, 0u);
    require(relative_call_source.find("call_target = cpu.r[0] + "
                                      "katana::runtime::relocate_code_address(0x00000006u)") !=
                    std::string::npos &&
                relative_call_source.find(
                    "cpu.pr = katana::runtime::relocate_code_address(0x00000006u);") !=
                    std::string::npos &&
                relative_call_source.find("case 0x0000000Eu:") != std::string::npos,
            "BSRF relokiert Zielbasis oder Rueckkehradresse nicht mit seinem Codetemplate.");

    auto dynamic_program = indirect_call_program;
    const auto make_dynamic =
        [](auto& candidate,
           const katana::ir::DynamicTargetClass target_class) -> katana::ir::Instruction* {
        for (auto& function : candidate)
            for (auto& block : function.blocks) {
                for (auto& instruction : block.instructions)
                    if (instruction.source_address == 2u) {
                        instruction.resolved_targets.clear();
                        instruction.dynamic_target_class = target_class;
                        block.has_indirect_successor = true;
                        function.direct_callees.erase(std::remove(function.direct_callees.begin(),
                                                                  function.direct_callees.end(),
                                                                  12u),
                                                      function.direct_callees.end());
                        return &instruction;
                    }
            }
        return nullptr;
    };
    auto* dynamic_call = make_dynamic(dynamic_program, katana::ir::DynamicTargetClass::RuntimeOnly);
    if (dynamic_call == nullptr) {
        std::cerr << "TEST FEHLGESCHLAGEN: Indirekte IR-Testcallsite fehlt.\n";
        return EXIT_FAILURE;
    }
    const auto runtime_only_source = katana::codegen::emit_cpp_program(dynamic_program, 0u);
    const auto runtime_only_text = katana::ir::emit_ir_text(dynamic_program);
    const auto runtime_only_json = katana::ir::emit_ir_json(dynamic_program);
    require(runtime_only_source.find("runtime_only_call(cpu, call_target)") != std::string::npos &&
                runtime_only_text.find("dynamic_target_class=runtime-only") != std::string::npos &&
                runtime_only_json.find("\"dynamic_target_class\":\"runtime-only\"") !=
                    std::string::npos,
            "Runtime-only-Klasse erreicht IR-Text, JSON oder validierenden Dispatcher nicht.");
    auto unresolved_program = dynamic_program;
    static_cast<void>(make_dynamic(unresolved_program, katana::ir::DynamicTargetClass::Unresolved));
    const auto unresolved_source = katana::codegen::emit_cpp_program(unresolved_program, 0u);
    require(unresolved_source.find("unresolved_call(cpu, call_target)") != std::string::npos &&
                unresolved_source.find("runtime_only_call(cpu, call_target)") == std::string::npos,
            "Unresolved-IR erhaelt einen Runtime-only- oder stillen Fallback.");
    auto invalid_runtime_only = dynamic_program;
    auto* invalid_call =
        make_dynamic(invalid_runtime_only, katana::ir::DynamicTargetClass::RuntimeOnly);
    if (invalid_call == nullptr) {
        std::cerr << "TEST FEHLGESCHLAGEN: Runtime-only-IR-Testcallsite fehlt.\n";
        return EXIT_FAILURE;
    }
    invalid_call->resolved_targets = {12u};
    require(!katana::ir::verify_program(invalid_runtime_only).empty(),
            "Runtime-only-IR akzeptiert geratene statische Zielkandidaten.");

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
                delay_memory_source.find("enter_memory_exception(cpu, error, "
                                         "katana::runtime::relocate_code_address(0x8C020002u), "
                                         "katana::runtime::relocate_code_address(0x8C020000u));") !=
                    std::string::npos &&
                delay_memory_source.find(
                    "cpu.pc = katana::runtime::relocate_code_address(0x8C020006u);") !=
                    std::string::npos,
            "BRA oder Speicherfehler im Delay Slot verlieren relokiertes Ziel, Fehler-PC oder "
            "Owner-PC.");

    constexpr std::array<std::uint8_t, 10> pc_relative_bytes = {
        0x00u,
        0x91u, // MOV.W @(0,PC),R1 -> 0x8C030004
        0x00u,
        0xD2u, // MOV.L @(0,PC),R2 -> 0x8C030004
        0x00u,
        0xC7u, // MOVA  @(0,PC),R0 -> 0x8C030008
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto pc_relative_lines = katana::sh4::disassemble(pc_relative_bytes, 0x8C030000u);
    constexpr std::array<std::uint32_t, 1> pc_relative_seeds = {0x8C030000u};
    const auto pc_relative_functions =
        katana::analysis::discover_functions(pc_relative_lines, pc_relative_seeds);
    const auto pc_relative_program =
        katana::ir::lower_program(pc_relative_lines, pc_relative_functions);
    const auto pc_relative_source =
        katana::codegen::emit_cpp_program(pc_relative_program, 0x8C030000u);
    require(pc_relative_source.find("guest_read_s16(cpu, "
                                    "katana::runtime::relocate_code_address(0x8C030004u))") !=
                    std::string::npos &&
                pc_relative_source.find("guest_read_u32(cpu, "
                                        "katana::runtime::relocate_code_address(0x8C030004u))") !=
                    std::string::npos &&
                pc_relative_source.find(
                    "cpu.r[0] = katana::runtime::relocate_code_address(0x8C030008u);") !=
                    std::string::npos &&
                pc_relative_source.find("relocate_code_address(katana::runtime::guest_read_u32") ==
                    std::string::npos,
            "PC-relative MOV.W/MOV.L/MOVA folgen nicht dem Codetemplate oder veraendern den "
            "geladenen Literalwert.");

    constexpr std::array<std::uint8_t, 2> sleep_bytes = {0x1Bu, 0x00u};
    const auto sleep_lines = katana::sh4::disassemble(sleep_bytes, 0x8C040000u);
    constexpr std::array<std::uint32_t, 1> sleep_seeds = {0x8C040000u};
    const auto sleep_functions = katana::analysis::discover_functions(sleep_lines, sleep_seeds);
    const auto sleep_program = katana::ir::lower_program(sleep_lines, sleep_functions);
    const auto sleep_source = katana::codegen::emit_cpp_program(sleep_program, 0x8C040000u);
    require(
        sleep_source.find("cpu.sleeping = true;") != std::string::npos &&
            sleep_source.find("cpu.pc = katana::runtime::relocate_code_address(0x8C040002u);") !=
                std::string::npos,
        "SLEEP behaelt keinen relokierten Fortsetzungs-PC.");

    katana::ir::Instruction first_timing;
    first_timing.source_address = 0x3000u;
    first_timing.operation = katana::ir::Operation::Nop;
    katana::ir::Instruction second_timing = first_timing;
    second_timing.source_address = 0x3002u;
    katana::ir::BasicBlock timing_block;
    timing_block.start_address = 0x3000u;
    timing_block.instructions = {first_timing, second_timing};
    katana::ir::Function timing_function;
    timing_function.entry_address = 0x3000u;
    timing_function.blocks = {std::move(timing_block)};
    const auto timing_source =
        katana::codegen::emit_cpp_program(std::vector{timing_function}, 0x3000u);
    require(timing_source.find("base_guest_cycles_per_instruction * 2u") != std::string::npos &&
                timing_source.find("base_guest_cycles_per_instruction * 3u") == std::string::npos,
            "Zwei Gastinstruktionen werden nicht als genau zwei Zyklen gezaehlt.");

    require(
        katana::ir::lowering_operation_for_instruction(katana::sh4::InstructionKind::LoadTlb) ==
                katana::ir::Operation::LoadTlb &&
            katana::ir::lowering_operation_for_instruction(katana::sh4::InstructionKind::Ocbi) ==
                katana::ir::Operation::Ocbi &&
            katana::ir::lowering_operation_for_instruction(katana::sh4::InstructionKind::Ocbp) ==
                katana::ir::Operation::Ocbp &&
            katana::ir::lowering_operation_for_instruction(katana::sh4::InstructionKind::Ocbwb) ==
                katana::ir::Operation::Ocbwb &&
            katana::ir::lowering_operation_for_instruction(
                katana::sh4::InstructionKind::MovcaLong) == katana::ir::Operation::MovcaLong,
        "LDTLB/OCBI/OCBP/OCBWB/MOVCA.L werden nicht in eigene IR-Operationen abgesenkt.");
    constexpr std::array<std::uint8_t, 16> cache_bytes = {0x38u,
                                                          0x00u,
                                                          0x93u,
                                                          0x07u,
                                                          0xA3u,
                                                          0x05u,
                                                          0xB3u,
                                                          0x0Cu,
                                                          0xC3u,
                                                          0x09u,
                                                          0x83u,
                                                          0x03u,
                                                          0x0Bu,
                                                          0x00u,
                                                          0x09u,
                                                          0x00u};
    const auto cache_lines = katana::sh4::disassemble(cache_bytes, 0x4000u);
    constexpr std::array<std::uint32_t, 1> cache_seeds = {0x4000u};
    const auto cache_functions = katana::analysis::discover_functions(cache_lines, cache_seeds);
    const auto cache_program = katana::ir::lower_program(cache_lines, cache_functions);
    const auto cache_source = katana::codegen::emit_cpp_program(cache_program, 0x4000u);
    require(
        cache_source.find("load_tlb(cpu)") != std::string::npos &&
            cache_source.find("OperandCacheOperation::Invalidate, cpu.r[7]") != std::string::npos &&
            cache_source.find("OperandCacheOperation::Purge, cpu.r[5]") != std::string::npos &&
            cache_source.find("OperandCacheOperation::WriteBack, cpu.r[12]") != std::string::npos &&
            cache_source.find("katana::runtime::guest_write_u32(cpu, cpu.r[9], cpu.r[0])") !=
                std::string::npos &&
            cache_source.find("services->prefetch(cpu, cpu.r[3])") != std::string::npos &&
            cache_source.find("enter_memory_exception(cpu, error, "
                              "katana::runtime::relocate_code_address(0x0000400Au));") !=
                std::string::npos,
        "Der C++-Emitter laesst LDTLB/cache instructions aus, verwechselt Register oder "
        "faengt PREF-MMU-Fehler nicht am SH-4-Exceptionpfad.");

    std::cout << "Alle C++-Codegenerator-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
