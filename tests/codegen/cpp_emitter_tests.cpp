#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/sh4/disassembler.hpp"

#include "../../src/codegen/cpp_lexical_replace.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::size_t count_occurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0u;
    std::size_t offset = 0u;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::string_view emitted_instruction(const std::string_view source,
                                     const std::string_view source_address) {
    const auto marker = std::string{"// katana-guest "} + std::string{source_address};
    const auto begin = source.find(marker);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = source.find("// katana-guest ", begin + marker.size());
    return source.substr(begin, end == std::string_view::npos ? source.size() - begin : end - begin);
}

std::string read_cpp_emitter_implementation() {
    const auto repository_root =
        std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
    std::ifstream input{repository_root / "src" / "codegen" / "cpp_emitter.cpp",
                        std::ios::binary};
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

int main() {
    require(katana::codegen::cpp_function_name(0x1000018Eu) == "fn_1000018E" &&
                katana::codegen::cpp_service_function_name(0x1000018Eu) ==
                    "fn_1000018E_with_services",
            "Der oeffentliche C++-Funktionssymbolvertrag verliert hexadezimale Grossbuchstaben.");

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

    const auto emitter_implementation = read_cpp_emitter_implementation();
    require(!emitter_implementation.empty(),
            "Die Codegen-Regressions koennen cpp_emitter.cpp nicht lesen.");
    constexpr std::array<std::string_view, 12u> legacy_guest_accesses = {
        "guest_read_u8(cpu",
        "guest_read_s8(cpu",
        "guest_read_u16(cpu",
        "guest_read_s16(cpu",
        "guest_read_u32(cpu",
        "guest_read_s32(cpu",
        "guest_write_u8(cpu",
        "guest_write_s8(cpu",
        "guest_write_u16(cpu",
        "guest_write_s16(cpu",
        "guest_write_u32(cpu",
        "guest_write_s32(cpu",
    };
    for (const auto legacy_access : legacy_guest_accesses) {
        require(emitter_implementation.find(legacy_access) == std::string::npos,
                "Der C++-Emitter besitzt noch einen Gastzugriff ohne Instruktionsprovenienz: " +
                    std::string{legacy_access});
    }
    require(
        emitter_implementation.find(
                "\"(cpu, guest_origin, \" + address + \")\"") !=
                std::string::npos &&
            emitter_implementation.find(
                "\"(cpu, katana_origin, katana_ram_address));\\n\"") !=
                std::string::npos &&
            emitter_implementation.find(
                "direct_ram_write_suffix(kind) + \"(guest_origin, \" + address +") !=
                std::string::npos &&
            emitter_implementation.find(
                "\"(cpu, katana_origin, katana_ram_address,\\n\"") !=
                std::string::npos,
        "Zentrale RAM-Read-/Write-Callsites oder ihre Guard-Fallbacks verlieren "
        "ihre Instruktionsprovenienz.");
    require(emitter_implementation.find(
                "const katana::runtime::GuestInstructionOrigin guest_origin{") !=
                std::string::npos &&
                emitter_implementation.find(
                    "services->prefetch(cpu, guest_origin,") != std::string::npos &&
                emitter_implementation.find("services->prefetch(cpu, cpu.r[") ==
                    std::string::npos,
            "PREF oder zentrale Gastzugriffe verlieren ihre exakte "
            "Instruktionsprovenienz.");

    require(source.find("#include \"katana/runtime/aot_runtime_abi.hpp\"") !=
                std::string::npos,
            "Der generierte Code bindet den schmalen AOT-Runtimevertrag nicht ein.");
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
    const auto call_owner = emitted_instruction(source, "0x8C010000");
    const auto call_delay_slot = emitted_instruction(source, "0x8C010002");
    require(call_owner.find(
                "katana::runtime::ExplicitGuestInstructionAttempt "
                "terminal_instruction_attempt(") !=
                    std::string_view::npos &&
                call_owner.find(
                    "katana::runtime::relocate_code_address(0x8C010000u), 2u);") !=
                    std::string_view::npos &&
                call_delay_slot.find(
                    "katana::runtime::ExplicitGuestInstructionAttempt "
                    "guest_instruction_attempt(") !=
                    std::string_view::npos &&
                call_delay_slot.find(
                    "katana::runtime::relocate_code_address(0x8C010002u), 1u);") !=
                    std::string_view::npos &&
                source.find("terminal_instruction_attempt.complete();") != std::string::npos &&
                source.find("katana::runtime::finalize_guest_block(") != std::string::npos,
            "Owner und Delay Slot besitzen keine getrennten Attempts, Kosten oder Abschlusskante.");

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
    local_chain_request.conservative_register_localization = true;
    const katana::codegen::CppBackend local_chain_backend;
    const auto local_chain_source = local_chain_backend.emit(local_chain_request).joined_text();
    constexpr std::string_view block_note = "                note_block_entry(";
    const auto first_block_note = local_chain_source.find(block_note);
    const auto local_chain_transition =
        local_chain_source.find("services->can_chain_executable_block(cpu.pc)) "
                                "goto katana_block_8C010002;");
    const auto next_block_note = local_chain_source.find(block_note, first_block_note + 1u);
    const auto function_entry_fastpath = local_chain_source.find(
        "if (katana::runtime::unrelocate_code_address(cpu.pc) == 0x8C010000u)\n"
        "        goto katana_block_8C010000;");
    const auto conservative_resume_dispatch = local_chain_source.find(
        "switch (katana::runtime::unrelocate_code_address(cpu.pc))");
    const auto native_entry_label =
        local_chain_source.find("katana_block_8C010000:", conservative_resume_dispatch);
    require(function_entry_fastpath != std::string::npos &&
                conservative_resume_dispatch != std::string::npos &&
                native_entry_label != std::string::npos &&
                function_entry_fastpath < conservative_resume_dispatch &&
                conservative_resume_dispatch < native_entry_label &&
                first_block_note != std::string::npos &&
                local_chain_transition != std::string::npos &&
                next_block_note != std::string::npos && first_block_note < local_chain_transition &&
                local_chain_transition < next_block_note &&
                local_chain_source.find(
                    "note_block_entry(katana::runtime::relocate_code_address(0x8C010000u));") !=
                    std::string::npos &&
                local_chain_source.find(
                    "katana::runtime::ExplicitGuestInstructionAttempt "
                    "guest_instruction_attempt(") !=
                    std::string::npos &&
                local_chain_source.find("++cpu.retired_guest_instructions;") ==
                    std::string::npos &&
                local_chain_source.find("services->consume_guest_cycles(") == std::string::npos &&
                local_chain_source.find(
                    "const bool take_branch = !katana_registers.t();") !=
                    std::string::npos &&
                local_chain_source.find(
                    "katana::runtime::NativeAotRegisterFile<"
                    "0x00000001u, 0x00000001u> "
                    "katana_registers(cpu);") != std::string::npos &&
                local_chain_source.find(
                    "katana_registers[0] = static_cast<std::uint32_t>(1);") !=
                    std::string::npos &&
                local_chain_source.find("cpu.r[0]") == std::string::npos &&
                local_chain_source.find("cpu.pc = take_branch ? "
                                        "katana::runtime::relocate_code_address(0x8C010002u) : "
                                        "katana::runtime::relocate_code_address(0x8C010006u);") !=
                    std::string::npos,
            "Function-Level-AOT besitzt keinen direkten Funktionseinstieg, kein lokales "
            "Mehrblock-Chaining, keinen Attempt/Retire-Guard oder keine konservative "
            "Registerlokalisierung.");

    katana::codegen::BackendRequest native_call_request{program, 0x8C010000u};
    native_call_request.single_block_execution = true;
    native_call_request.guarded_local_block_chaining = true;
    native_call_request.external_dynamic_dispatch = true;
    const auto native_call_source =
        local_chain_backend.emit(native_call_request).joined_text();
    const auto native_call = native_call_source.find(
        "fn_8C010008_with_services(cpu, services);");
    const auto native_return_guard = native_call_source.find(
        "katana::runtime::unrelocate_code_address(cpu.pc) == 0x8C010004u",
        native_call);
    const auto native_return_label =
        native_call_source.find("goto katana_block_8C010004;", native_return_guard);
    require(native_call != std::string::npos &&
                native_call_source.rfind(
                    "katana::runtime::NativeAotCallDepthGuard native_call_depth;",
                    native_call) != std::string::npos &&
                native_call_source.rfind(
                    "services->can_chain_executable_block(cpu.pc)", native_call) !=
                    std::string::npos &&
                native_return_guard != std::string::npos &&
                native_return_label != std::string::npos,
            "Function-Level-AOT fuehrt einen bekannten Call oder belegten Return nicht "
            "direkt unter dem Chainingguard aus.");
    const std::array architectural_call_boundary{0x8C010008u};
    auto boundary_call_request = native_call_request;
    boundary_call_request.architectural_boundary_entries =
        architectural_call_boundary;
    const auto boundary_call_source =
        local_chain_backend.emit(boundary_call_request).joined_text();
    require(boundary_call_source.find(
                "fn_8C010008_with_services(cpu, services);") ==
                std::string::npos &&
                boundary_call_source.find(
                    "cpu.pc = katana::runtime::relocate_code_address("
                    "0x8C010008u);") != std::string::npos,
            "Architekturgrenze wird von einem direkten nativen Function-Level-Call "
            "umgangen.");

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
    constexpr std::string_view jump_target_latch_text =
        "const std::uint32_t jump_target = cpu.r[1];";
    constexpr std::string_view jump_switch_text =
        "switch (katana::runtime::unrelocate_code_address(jump_target))";
    constexpr std::string_view jump_case_text = "case 0x00000008u:";
    const auto jump_target_latch = indirect_jump_source.find(jump_target_latch_text);
    const auto jump_delay_slot =
        indirect_jump_source.find("// katana-guest 0x00000004", jump_target_latch);
    const auto jump_first_completion =
        indirect_jump_source.find("katana::runtime::finalize_guest_block(", jump_target_latch);
    const auto jump_first_flush =
        indirect_jump_source.find(
            "katana::runtime::flush_pending_guest_cycles(cpu, *services)", jump_target_latch);
    const auto jump_switch = indirect_jump_source.find(jump_switch_text, jump_delay_slot);
    const auto jump_case = indirect_jump_source.find(jump_case_text, jump_switch);
    const auto jump_case_pc = indirect_jump_source.find("cpu.pc = jump_target;", jump_case);
    const auto jump_case_completion =
        indirect_jump_source.find("katana::runtime::finalize_guest_block(", jump_case_pc);
    const auto jump_case_continue = indirect_jump_source.find("continue;", jump_case_completion);
    const auto jump_default = indirect_jump_source.find("default:", jump_case_continue);
    const auto jump_fallback_completion =
        indirect_jump_source.find("katana::runtime::finalize_guest_block(", jump_default);
    const auto jump_fallback =
        indirect_jump_source.find("(cpu, jump_target);", jump_fallback_completion);
    const auto jump_fallback_return = indirect_jump_source.find("return;", jump_fallback);
    require(jump_target_latch != std::string::npos &&
                count_occurrences(indirect_jump_source, jump_target_latch_text) == 1u &&
                jump_delay_slot != std::string::npos && jump_target_latch < jump_delay_slot &&
                jump_first_completion != std::string::npos &&
                jump_delay_slot < jump_first_completion &&
                (jump_first_flush == std::string::npos || jump_delay_slot < jump_first_flush) &&
                jump_switch != std::string::npos && jump_delay_slot < jump_switch &&
                jump_case != std::string::npos && jump_switch < jump_case &&
                jump_case_pc != std::string::npos && jump_case < jump_case_pc &&
                jump_case_completion != std::string::npos &&
                jump_case_pc < jump_case_completion &&
                jump_case_continue != std::string::npos &&
                jump_case_completion < jump_case_continue &&
                jump_default != std::string::npos && jump_case_continue < jump_default &&
                jump_fallback_completion != std::string::npos &&
                jump_default < jump_fallback_completion &&
                jump_fallback != std::string::npos &&
                jump_fallback_completion < jump_fallback &&
                count_occurrences(indirect_jump_source, "(cpu, jump_target);") == 1u &&
                jump_fallback_return != std::string::npos &&
                jump_fallback < jump_fallback_return &&
                indirect_jump_source.find("services->consume_guest_cycles(") ==
                    std::string::npos &&
                indirect_jump_source.find(
                    "jump_target = katana::runtime::relocate_code_address(cpu.r[1])") ==
                    std::string::npos,
            "Aufgeloestes JMP veraendert sein Registerziel, fallthrought oder verbucht Zeit vor "
            "dem Block.");

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

    // A selected closure probe must observe the common successful return of
    // both the static candidate-chain and RuntimeOnly fallback paths exactly
    // once.  Every unselected partition remains free of probe state and
    // branches. Runtime evidence is retained only after the exact SH-4 call
    // continuation was restored.
    auto runtime_candidate_program = indirect_call_program;
    katana::ir::Instruction* runtime_candidate_call = nullptr;
    for (auto& function : runtime_candidate_program)
        for (auto& block : function.blocks)
            for (auto& instruction : block.instructions)
                if (instruction.source_address == 2u) {
                    instruction.dynamic_target_class =
                        katana::ir::DynamicTargetClass::RuntimeOnly;
                    block.has_indirect_successor = true;
                    runtime_candidate_call = &instruction;
                }
    require(runtime_candidate_call != nullptr &&
                runtime_candidate_call->resolved_targets ==
                    std::vector<std::uint32_t>{12u},
            "Closure-Probe-Test verlor seinen statischen RuntimeOnly-Kandidaten.");
    constexpr std::array<std::uint32_t, 1u> closure_probe_callsites{2u};
    katana::codegen::BackendRequest closure_probe_request{
        runtime_candidate_program, 0u};
    closure_probe_request.external_function_linkage = true;
    closure_probe_request.external_dynamic_dispatch = true;
    closure_probe_request.closure_probe_callsites = closure_probe_callsites;
    const auto closure_probe_emission =
        katana::codegen::CppBackend{}.emit(closure_probe_request);
    require(
        closure_probe_emission.functions.find("case 0x0000000Cu:") !=
                std::string::npos &&
            closure_probe_emission.functions.find(
                "runtime_only_call(cpu, call_target)") != std::string::npos &&
            closure_probe_emission.functions.find(
                "if (closure_probe_dispatch_pending &&") != std::string::npos &&
            closure_probe_emission.functions.find(
                "katana_exact_guarded_target_matches(cpu.pc, "
                "katana::runtime::relocate_code_address(0x00000006u))") !=
                std::string::npos &&
            count_occurrences(
                closure_probe_emission.functions,
                "note_successful_closure_probe_dispatch(") == 1u,
        "RuntimeOnly-Kandidat und Fallback teilen keinen exakt einmaligen, "
        "fortsetzungsgebundenen Closure-Probe-Erfolgsweg.");
    auto unselected_probe_request = closure_probe_request;
    unselected_probe_request.closure_probe_callsites = {};
    const auto unselected_probe_emission =
        katana::codegen::CppBackend{}.emit(unselected_probe_request);
    require(
        unselected_probe_emission.declarations.find(
            "closure_probe_dispatch_pending") == std::string::npos &&
            unselected_probe_emission.functions.find(
                "closure_probe_dispatch_pending") == std::string::npos &&
            unselected_probe_emission.functions.find(
                "note_successful_closure_probe_dispatch(") ==
                std::string::npos,
        "Nicht armierte CallRegister-Sites tragen Closure-Probe-Hotpathkosten.");

    const std::array guarded_native_call_targets{
        katana::codegen::GuardedNativeCallTarget{2u, 12u}};
    katana::codegen::BackendRequest guarded_native_request{dynamic_program, 0u};
    guarded_native_request.external_function_linkage = true;
    guarded_native_request.single_block_execution = true;
    guarded_native_request.external_dynamic_dispatch = true;
    guarded_native_request.guarded_local_block_chaining = true;
    guarded_native_request.guarded_native_call_targets =
        guarded_native_call_targets;
    const auto guarded_native_source =
        katana::codegen::CppBackend{}.emit(guarded_native_request).joined_text();
    require(
        guarded_native_source.find(
            "katana::runtime::unrelocate_code_address(call_target) == 0x0000000Cu") !=
                std::string::npos &&
            guarded_native_source.find(
                "services->can_chain_executable_block(cpu.pc)") != std::string::npos &&
            guarded_native_source.find(
                "katana::runtime::NativeAotCallDepthGuard native_call_depth;") !=
                std::string::npos &&
            guarded_native_source.find(
                "fn_0000000C_with_services(cpu, services);") != std::string::npos &&
            guarded_native_source.find("goto katana_block_00000006;") !=
                std::string::npos &&
            guarded_native_source.find("}\n                return;") !=
                std::string::npos,
        "Guardierter nativer Registercall verliert Live-Zielvergleich, Runtime-/Depthguard, "
        "direkten AOT-Call, Fortsetzung oder Dispatcher-Fallback.");
    const std::array guarded_architectural_boundary{12u};
    auto guarded_boundary_request = guarded_native_request;
    guarded_boundary_request.architectural_boundary_entries =
        guarded_architectural_boundary;
    const auto guarded_boundary_source =
        katana::codegen::CppBackend{}
            .emit(guarded_boundary_request)
            .joined_text();
    require(
        guarded_boundary_source.find(
            "katana::runtime::unrelocate_code_address(call_target) == "
            "0x0000000Cu") == std::string::npos &&
            guarded_boundary_source.find(
                "cpu.pc = call_target;") !=
                std::string::npos,
        "Architekturgrenze wird vom guardierten nativen Singleton-Call "
        "umgangen.");

    auto guarded_candidate_program = indirect_call_program;
    auto* guarded_candidate_call =
        make_dynamic(guarded_candidate_program, katana::ir::DynamicTargetClass::GuardedPartial);
    if (guarded_candidate_call == nullptr) {
        std::cerr << "TEST FEHLGESCHLAGEN: Guarded-IR-Testcallsite fehlt.\n";
        return EXIT_FAILURE;
    }
    guarded_candidate_call->resolved_targets = {0x00001000u};
    for (auto& function : guarded_candidate_program) {
        if (std::find(function.indirect_call_sites.begin(),
                      function.indirect_call_sites.end(),
                      guarded_candidate_call->source_address) !=
            function.indirect_call_sites.end()) {
            function.direct_callees.insert(function.direct_callees.end(),
                                           guarded_candidate_call->resolved_targets.begin(),
                                           guarded_candidate_call->resolved_targets.end());
            std::sort(function.direct_callees.begin(), function.direct_callees.end());
            function.direct_callees.erase(std::unique(function.direct_callees.begin(),
                                                      function.direct_callees.end()),
                                          function.direct_callees.end());
        }
    }
    require(katana::ir::verify_program(guarded_candidate_program).empty(),
            "Guarded-IR-Testcallsite besitzt inkonsistente Callee-Metadaten.");
    const auto guarded_candidate_source =
        katana::codegen::emit_cpp_program(guarded_candidate_program, 0u);
    const auto guarded_candidate_dispatch =
        guarded_candidate_source.find("guarded_call(cpu, call_target)");
    const auto guarded_candidate_completion =
        guarded_candidate_source.rfind("katana::runtime::finalize_guest_block(",
                                       guarded_candidate_dispatch);
    require(guarded_candidate_dispatch != std::string::npos &&
                guarded_candidate_completion != std::string::npos &&
                guarded_candidate_completion < guarded_candidate_dispatch,
            "Ein aufgeloester Guarded-Call betritt sein Ziel vor dem zentralen Blockabschluss.");

    auto guarded_complete_program = indirect_call_program;
    auto* guarded_complete_call =
        make_dynamic(guarded_complete_program,
                     katana::ir::DynamicTargetClass::GuardedComplete);
    if (guarded_complete_call == nullptr) {
        std::cerr << "TEST FEHLGESCHLAGEN: Guarded-Complete-Testcallsite fehlt.\n";
        return EXIT_FAILURE;
    }
    guarded_complete_call->resolved_targets = {0x00001000u, 0x00002000u};
    for (auto& function : guarded_complete_program) {
        if (std::find(function.indirect_call_sites.begin(),
                      function.indirect_call_sites.end(),
                      guarded_complete_call->source_address) ==
            function.indirect_call_sites.end())
            continue;
        function.direct_callees.insert(function.direct_callees.end(),
                                       guarded_complete_call->resolved_targets.begin(),
                                       guarded_complete_call->resolved_targets.end());
        std::sort(function.direct_callees.begin(), function.direct_callees.end());
        function.direct_callees.erase(
            std::unique(function.direct_callees.begin(),
                        function.direct_callees.end()),
            function.direct_callees.end());
    }
    require(katana::ir::verify_program(guarded_complete_program).empty(),
            "Guarded-Complete-Testcallsite besitzt inkonsistente Callee-Metadaten.");
    const auto guarded_complete_source =
        katana::codegen::emit_cpp_program(guarded_complete_program, 0u);
    require(
        guarded_complete_source.find(
            "katana_exact_guarded_target_matches(call_target, 0x00001000u)") !=
                std::string::npos &&
            guarded_complete_source.find(
                "katana_exact_guarded_target_matches(call_target, 0x00002000u)") !=
                std::string::npos &&
            guarded_complete_source.find(
                "exact_guarded_call(cpu, call_target, 0x00001000u)") !=
                std::string::npos,
        "Guarded-Complete-Mehrzielcall bindet Aliasvergleich oder fail-closed "
        "Defaultpfad nicht an die vollstaendige Zielmenge.");

    auto empty_guarded_complete = dynamic_program;
    static_cast<void>(make_dynamic(empty_guarded_complete,
                                   katana::ir::DynamicTargetClass::GuardedComplete));
    require(!katana::ir::verify_program(empty_guarded_complete).empty(),
            "Guarded-Complete-IR akzeptiert eine leere erlaubte Zielmenge.");

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

    constexpr std::array<std::uint8_t, 2u> unknown_bytes = {0xFFu, 0xFFu};
    const auto unknown_lines = katana::sh4::disassemble(unknown_bytes, 0x8C020000u);
    constexpr std::array<std::uint32_t, 1u> unknown_seeds = {0x8C020000u};
    const auto unknown_functions =
        katana::analysis::discover_functions(unknown_lines, unknown_seeds);
    const auto unknown_program = katana::ir::lower_program(unknown_lines, unknown_functions);
    const auto unknown_source = katana::codegen::emit_cpp_program(unknown_program, 0x8C020000u);
    const auto unknown_instruction = emitted_instruction(unknown_source, "0x8C020000u");
    const auto unknown_raise = unknown_instruction.find("raise_illegal_instruction");
    const auto unknown_completion =
        unknown_instruction.find("katana::runtime::finalize_guest_block(", unknown_raise);
    const auto unknown_return = unknown_instruction.find("return;", unknown_completion);
    require(unknown_raise != std::string_view::npos &&
                unknown_completion != std::string_view::npos &&
                unknown_return != std::string_view::npos &&
                unknown_raise < unknown_completion && unknown_completion < unknown_return,
            "Eine unbekannte Instruktion umgeht Attempt-Zeit und zentralen Blockabschluss.");

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
    const auto delay_load =
        emitted_instruction(delay_memory_source, "0x8C020002");
    const auto delay_load_flush =
        delay_load.find("katana::runtime::flush_pending_guest_cycles(cpu, *services)");
    const auto delay_load_attempt =
        delay_load.find(
            "katana::runtime::ExplicitGuestInstructionAttempt guest_instruction_attempt");
    require(delay_memory_source.find("catch (const katana::runtime::MemoryAccessError& error)") !=
                    std::string::npos &&
                delay_load.find(
                    "const katana::runtime::GuestInstructionOrigin "
                    "guest_origin{0x8C020002u, "
                    "katana::runtime::relocate_code_address(0x8C020002u), true}") !=
                    std::string_view::npos &&
                delay_load.find("katana_direct_ram_read_u32(guest_origin, cpu.r[1],") !=
                    std::string_view::npos &&
                delay_load_flush != std::string_view::npos &&
                delay_load_attempt != std::string_view::npos &&
                delay_load_flush < delay_load_attempt &&
                delay_memory_source.find(
                    "enter_memory_exception_with_provenance(cpu, error, "
                                         "katana::runtime::relocate_code_address(0x8C020002u), "
                                         "0x00006212u, "
                                         "katana::runtime::relocate_code_address(0x8C020000u));") !=
                    std::string::npos &&
                delay_memory_source.find(
                    "cpu.pc = katana::runtime::relocate_code_address(0x8C020006u);") !=
                    std::string::npos,
            "BRA oder Load im Delay Slot verlieren eigene Provenienz, relokiertes Ziel, "
            "Fehler-PC oder Owner-PC.");

    auto proven_delay_memory_program = delay_memory_program;
    for (auto& function : proven_delay_memory_program) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.source_address == 0x8C020002u)
                    instruction.memory_effects.region =
                        katana::ir::MemoryRegionKind::NormalRam;
            }
        }
    }
    const auto proven_delay_memory_source =
        katana::codegen::emit_cpp_program(proven_delay_memory_program, 0x8C020000u);
    const auto proven_delay_load =
        emitted_instruction(proven_delay_memory_source, "0x8C020002");
    require(
        proven_delay_memory_source.find(
            "auto katana_direct_ram = "
            "cpu.memory.direct_linear_memory_guard(false);") != std::string::npos &&
            proven_delay_memory_source.find(
                "const auto katana_direct_ram_read_u32 =") !=
                std::string::npos &&
            proven_delay_memory_source.find(
                "cpu.privileged_mode_inline()") != std::string::npos &&
            proven_delay_memory_source.find(
                "cpu.privileged_mode()") == std::string::npos &&
            proven_delay_memory_source.find(
                "if (katana_allow_direct_read &&\n"
                "            katana_direct_ram_translate(") !=
                std::string::npos &&
            proven_delay_memory_source.find(
                "const auto katana_direct_ram_resolve =\n"
                "        [&](const std::uint32_t katana_virtual_address,") !=
                std::string::npos &&
            proven_delay_memory_source.find(
                "katana::runtime::direct_linear_guard_read_u32("
                "katana_direct_ram, katana_direct_ram_address, "
                "katana_ram_value)") !=
                std::string::npos &&
            proven_delay_memory_source.find(
                "katana::runtime::guest_read_u32_at("
                "cpu, katana_origin, katana_ram_address)") != std::string::npos &&
            proven_delay_load.find(
                "katana_direct_ram_read_u32(guest_origin, cpu.r[1], true)") !=
                std::string_view::npos &&
            proven_delay_load.find(
                "katana::runtime::flush_pending_guest_cycles(cpu, *services)") ==
                std::string_view::npos &&
            delay_load.find("direct_linear_guard_read_u32") == std::string_view::npos,
        "Ein allgemeiner als NormalRam bewiesener Register-Load nutzt nicht den "
        "funktionsweiten direkten RAM-Guard mit korrektem Fallback.");

    katana::codegen::BackendRequest deferred_mmio_request{
        delay_memory_program, 0x8C020000u};
    deferred_mmio_request.single_block_execution = true;
    deferred_mmio_request.guarded_local_block_chaining = true;
    deferred_mmio_request.external_dynamic_dispatch = true;
    const auto deferred_mmio_source =
        katana::codegen::CppBackend{}.emit(deferred_mmio_request).joined_text();
    const auto deferred_flag =
        deferred_mmio_source.find("bool katana_deferred_safepoint_8C020002 = false;");
    const auto deferred_epoch = deferred_mmio_source.find(
        "const auto katana_mmio_boundary_epoch_before = "
        "cpu.memory.mmio_boundary_epoch();",
        deferred_flag);
    const auto deferred_load =
        deferred_mmio_source.find(
            "katana_direct_ram_read_u32(guest_origin, cpu.r[1], "
            "katana_guarded_unknown_ram_reads)",
                                  deferred_epoch);
    const auto deferred_assignment = deferred_mmio_source.find(
        "katana_deferred_safepoint_8C020002 = "
        "cpu.memory.mmio_boundary_epoch() != katana_mmio_boundary_epoch_before;",
        deferred_load);
    const auto deferred_target = deferred_mmio_source.find(
        "cpu.pc = katana::runtime::relocate_code_address(0x8C020006u);",
        deferred_assignment);
    const auto deferred_terminal_completion =
        deferred_mmio_source.find("terminal_instruction_attempt.complete();", deferred_target);
    const auto deferred_finalize = deferred_mmio_source.find(
        "katana_commit_post_instruction_safepoint(", deferred_terminal_completion);
    const auto deferred_chain =
        deferred_mmio_source.find("services->can_chain_executable_block(cpu.pc)",
                                  deferred_finalize);
    require(
        deferred_flag != std::string::npos &&
            deferred_epoch != std::string::npos &&
            deferred_load != std::string::npos &&
            deferred_assignment != std::string::npos &&
            deferred_target != std::string::npos &&
            deferred_terminal_completion != std::string::npos &&
            deferred_finalize != std::string::npos &&
            deferred_chain != std::string::npos &&
            deferred_flag < deferred_epoch && deferred_epoch < deferred_load &&
            deferred_load < deferred_assignment &&
            deferred_assignment < deferred_target &&
            deferred_target < deferred_terminal_completion &&
            deferred_terminal_completion < deferred_finalize &&
            deferred_finalize < deferred_chain &&
            count_occurrences(deferred_mmio_source,
                              "if (katana_commit_post_instruction_safepoint(") == 1u,
        "Ein tatsaechlicher MMIO-Kandidat im Delay Slot wird nicht erst nach Ziel-PC "
        "und Terminal-Retirement genau einmal am Runtime-Safepoint abgeschlossen.");

    katana::codegen::BackendRequest proven_delay_request{
        proven_delay_memory_program, 0x8C020000u};
    proven_delay_request.single_block_execution = true;
    proven_delay_request.guarded_local_block_chaining = true;
    proven_delay_request.external_dynamic_dispatch = true;
    const auto proven_delay_product_source =
        katana::codegen::CppBackend{}.emit(proven_delay_request).joined_text();
    require(
        proven_delay_product_source.find("katana_deferred_safepoint_8C020002") ==
                std::string::npos &&
            proven_delay_product_source.find("mmio_boundary_epoch()") ==
                std::string::npos,
        "Bewiesenes lineares Delay-Slot-RAM erhaelt eine unnoetige MMIO-Safepointkante.");

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
    const auto pc_relative_word =
        emitted_instruction(pc_relative_source, "0x8C030000");
    const auto pc_relative_long =
        emitted_instruction(pc_relative_source, "0x8C030002");
    require(pc_relative_source.find(
                "const katana::runtime::GuestInstructionOrigin "
                "guest_origin{0x8C030000u, "
                "katana::runtime::relocate_code_address(0x8C030000u), true}") !=
                    std::string::npos &&
                pc_relative_source.find(
                    "katana_direct_ram_read_s16(guest_origin, "
                    "katana::runtime::relocate_code_address(0x8C030004u), "
                    "katana_guarded_unknown_ram_reads)") != std::string::npos &&
                pc_relative_source.find(
                    "katana_direct_ram_read_u32(guest_origin, "
                    "katana::runtime::relocate_code_address(0x8C030004u), "
                    "katana_guarded_unknown_ram_reads)") != std::string::npos &&
                pc_relative_source.find(
                    "cpu.r[0] = katana::runtime::relocate_code_address(0x8C030008u);") !=
                    std::string::npos &&
                pc_relative_source.find(
                    "relocate_code_address(katana_direct_ram_read_u32") ==
                    std::string::npos &&
                count_occurrences(
                    pc_relative_word,
                    "GuestInstructionOrigin guest_origin{0x8C030000u") == 1u &&
                count_occurrences(pc_relative_word,
                                  "katana_direct_ram_read_s16(guest_origin") == 1u &&
                pc_relative_word.find(
                    "GuestInstructionOrigin guest_origin{0x8C030002u") ==
                    std::string_view::npos &&
                count_occurrences(
                    pc_relative_long,
                    "GuestInstructionOrigin guest_origin{0x8C030002u") == 1u &&
                count_occurrences(pc_relative_long,
                                  "katana_direct_ram_read_u32(guest_origin") == 1u &&
                pc_relative_long.find(
                    "GuestInstructionOrigin guest_origin{0x8C030000u") ==
                    std::string_view::npos,
            "PC-relative MOV.W/MOV.L besitzen keine getrennten Origins, folgen nicht dem "
            "Codetemplate oder veraendern den geladenen Literalwert.");

    auto proven_linear_pc_relative_program = pc_relative_program;
    for (auto& function : proven_linear_pc_relative_program) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.source_address == 0x8C030000u ||
                    instruction.source_address == 0x8C030002u)
                    instruction.memory_effects.region =
                        katana::ir::MemoryRegionKind::NormalRam;
            }
        }
    }
    const auto proven_linear_pc_relative_source = katana::codegen::emit_cpp_program(
        proven_linear_pc_relative_program, 0x8C030000u);
    const auto proven_linear_word =
        emitted_instruction(proven_linear_pc_relative_source, "0x8C030000");
    const auto proven_linear_long =
        emitted_instruction(proven_linear_pc_relative_source, "0x8C030002");
    require(pc_relative_word.find(
                "katana::runtime::flush_pending_guest_cycles(cpu, *services)") !=
                    std::string_view::npos &&
                pc_relative_long.find(
                    "katana::runtime::flush_pending_guest_cycles(cpu, *services)") !=
                    std::string_view::npos &&
                proven_linear_word.find(
                    "katana::runtime::flush_pending_guest_cycles(cpu, *services)") ==
                    std::string_view::npos &&
                proven_linear_long.find(
                    "katana::runtime::flush_pending_guest_cycles(cpu, *services)") ==
                    std::string_view::npos,
            "Unbewiesene Speicherzugriffe verlieren ihren Flush oder bewiesenes lineares RAM "
            "wird weiterhin vor jeder Instruktion geflusht.");

    auto guarded_linear_program = proven_linear_pc_relative_program;
    auto& guarded_linear_blocks = guarded_linear_program.front().blocks;
    const auto original_linear_block = guarded_linear_blocks.front();
    katana::ir::BasicBlock guarded_linear_first;
    guarded_linear_first.start_address = original_linear_block.start_address;
    guarded_linear_first.instructions = {original_linear_block.instructions.front()};
    guarded_linear_first.successors = {
        original_linear_block.instructions[1u].source_address};
    katana::ir::BasicBlock guarded_linear_second;
    guarded_linear_second.start_address =
        original_linear_block.instructions[1u].source_address;
    guarded_linear_second.instructions.assign(
        original_linear_block.instructions.begin() + 1,
        original_linear_block.instructions.end());
    guarded_linear_blocks = {
        std::move(guarded_linear_first), std::move(guarded_linear_second)};
    katana::codegen::BackendRequest guarded_linear_request{
        guarded_linear_program, 0x8C030000u};
    guarded_linear_request.single_block_execution = true;
    guarded_linear_request.guarded_local_block_chaining = true;
    guarded_linear_request.external_dynamic_dispatch = true;
    const auto guarded_linear_source =
        katana::codegen::CppBackend{}.emit(guarded_linear_request).joined_text();
    require(
        guarded_linear_source.find(
            "auto katana_direct_ram = "
            "cpu.memory.direct_linear_memory_guard(false);") != std::string::npos &&
            guarded_linear_source.find(
                "services->can_chain_executable_block(cpu.pc)) "
                "goto katana_block_8C030002;") != std::string::npos &&
            guarded_linear_source.find(
                "direct_linear_memory_guard_current(katana_direct_ram, false)") ==
                std::string::npos,
        "Der funktionsgebundene direkte RAM-Guard wird an einer reinen nativen "
        "Labelgrenze unerwartet erneut validiert.");

    constexpr std::array<std::uint8_t, 6> read_modify_write_bytes = {
        0x0Fu,
        0xCDu, // AND.B #15,@(R0,GBR)
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto read_modify_write_lines =
        katana::sh4::disassemble(read_modify_write_bytes, 0x8C050000u);
    constexpr std::array<std::uint32_t, 1> read_modify_write_seeds = {0x8C050000u};
    const auto read_modify_write_functions =
        katana::analysis::discover_functions(read_modify_write_lines, read_modify_write_seeds);
    const auto read_modify_write_program =
        katana::ir::lower_program(read_modify_write_lines, read_modify_write_functions);
    const auto read_modify_write_source =
        katana::codegen::emit_cpp_program(read_modify_write_program, 0x8C050000u);
    const auto read_modify_write =
        emitted_instruction(read_modify_write_source, "0x8C050000");
    require(count_occurrences(
                read_modify_write,
                "GuestInstructionOrigin guest_origin{0x8C050000u, "
                "katana::runtime::relocate_code_address(0x8C050000u), true}") == 1u &&
                count_occurrences(
                    read_modify_write, "guest_read_u8_at(cpu, guest_origin, address)") == 1u &&
                count_occurrences(
                    read_modify_write, "katana_guest_ram_write_u8(guest_origin, address") == 1u &&
                count_occurrences(read_modify_write,
                                  "GuestInstructionOrigin guest_origin{0x") == 1u,
            "AND.B Read-Modify-Write verwendet nicht fuer Read und Write denselben Origin.");

    auto proven_read_modify_write_program = read_modify_write_program;
    proven_read_modify_write_program.front()
        .blocks.front()
        .instructions.front()
        .memory_effects.region = katana::ir::MemoryRegionKind::NormalRam;
    const auto proven_read_modify_write_source = katana::codegen::emit_cpp_program(
        proven_read_modify_write_program, 0x8C050000u);
    const auto proven_read_modify_write =
        emitted_instruction(proven_read_modify_write_source, "0x8C050000");
    require(
        proven_read_modify_write_source.find(
            "auto katana_direct_ram = "
            "cpu.memory.direct_linear_memory_guard(false);") != std::string::npos &&
            proven_read_modify_write_source.find(
                "const auto katana_direct_ram_write_u8 =") !=
                std::string::npos &&
            proven_read_modify_write_source.find(
                "Memory::DirectLinearWriteBatch* const "
                "katana_direct_ram_writes = nullptr;") != std::string::npos &&
            proven_read_modify_write_source.find(
                "native_aot_code_tracker_tracks_address(") !=
                std::string::npos &&
            proven_read_modify_write_source.find(
                "katana_batch->try_stage_u8(") !=
                std::string::npos &&
            proven_read_modify_write_source.find(
                "cpu.memory.try_write_direct_linear_u8(") !=
                std::string::npos &&
            proven_read_modify_write_source.find(
                "guest_write_u8_at(cpu, katana_origin, katana_ram_address,") !=
                std::string::npos &&
            proven_read_modify_write.find(
                "katana_direct_ram_read_u8(guest_origin, address, true)") !=
                std::string_view::npos &&
            proven_read_modify_write.find(
                "katana_direct_ram_write_u8(katana_direct_ram_writes, "
                "guest_origin, address,") !=
                std::string_view::npos,
        "Ein bewiesener RAM-Read-Modify-Write nutzt keinen invalidierungs- und "
        "watchpointsicheren direkten Read-/Write-Vertrag mit allgemeinem Fallback.");

    constexpr std::array<std::uint8_t, 8> direct_store_batch_bytes = {
        0x12u,
        0x22u, // MOV.L R1,@R2
        0x12u,
        0x24u, // MOV.L R1,@R4
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto direct_store_batch_lines =
        katana::sh4::disassemble(direct_store_batch_bytes, 0x8C051000u);
    constexpr std::array<std::uint32_t, 1> direct_store_batch_seeds = {
        0x8C051000u};
    const auto direct_store_batch_functions =
        katana::analysis::discover_functions(
            direct_store_batch_lines, direct_store_batch_seeds);
    auto direct_store_batch_program = katana::ir::lower_program(
        direct_store_batch_lines, direct_store_batch_functions);
    auto& direct_store_batch_blocks =
        direct_store_batch_program.front().blocks;
    const auto direct_store_original = direct_store_batch_blocks.front();
    katana::ir::BasicBlock direct_store_prefix;
    direct_store_prefix.start_address = 0x8C051000u;
    direct_store_prefix.instructions.assign(
        direct_store_original.instructions.begin(),
        direct_store_original.instructions.begin() + 2);
    for (auto& instruction : direct_store_prefix.instructions)
        instruction.memory_effects.region =
            katana::ir::MemoryRegionKind::NormalRam;
    direct_store_prefix.successors = {0x8C051004u};
    katana::ir::BasicBlock direct_store_return;
    direct_store_return.start_address = 0x8C051004u;
    direct_store_return.instructions.assign(
        direct_store_original.instructions.begin() + 2,
        direct_store_original.instructions.end());
    direct_store_batch_blocks = {
        std::move(direct_store_prefix), std::move(direct_store_return)};
    katana::codegen::BackendRequest direct_store_batch_request{
        direct_store_batch_program, 0x8C051000u};
    direct_store_batch_request.single_block_execution = true;
    direct_store_batch_request.guarded_local_block_chaining = true;
    direct_store_batch_request.external_dynamic_dispatch = true;
    direct_store_batch_request.conservative_register_localization = true;
    const auto direct_store_batch_source =
        katana::codegen::CppBackend{}
            .emit(direct_store_batch_request)
            .joined_text();
    const auto direct_store_batch_begin =
        direct_store_batch_source.find(
            "cpu.memory.begin_direct_linear_write_batch();");
    const auto direct_store_batch_stage =
        direct_store_batch_source.find(
            "katana_batch->try_stage_u32(");
    const auto direct_store_batch_flush =
        direct_store_batch_source.find(
            "katana_direct_ram_writes->flush();",
            direct_store_batch_begin);
    const auto direct_store_batch_successor =
        direct_store_batch_source.find(
            "cpu.pc = katana::runtime::relocate_code_address("
            "0x8C051004u);",
            direct_store_batch_flush);
    const auto direct_store_batch_exit =
        direct_store_batch_source.find(
            "if (katana_guest_write_exit_requested) {",
            direct_store_batch_successor);
    const auto direct_store_batch_chain =
        direct_store_batch_source.find(
            "services->can_chain_executable_block(cpu.pc)",
            direct_store_batch_exit);
    require(
        direct_store_batch_begin != std::string::npos &&
            direct_store_batch_stage != std::string::npos &&
            count_occurrences(
                direct_store_batch_source,
                "katana_direct_ram_write_u32("
                "katana_direct_ram_writes, guest_origin,") == 2u &&
            direct_store_batch_flush != std::string::npos &&
            direct_store_batch_successor != std::string::npos &&
            direct_store_batch_exit != std::string::npos &&
            direct_store_batch_chain != std::string::npos &&
            direct_store_batch_flush < direct_store_batch_successor &&
            direct_store_batch_successor < direct_store_batch_exit &&
            direct_store_batch_exit < direct_store_batch_chain &&
            direct_store_batch_source.find(
                "katana_direct_ram_code_tracker != nullptr &&") !=
                std::string::npos &&
            direct_store_batch_source.find(
                "katana_direct_ram_code_tracker == nullptr ||") !=
                std::string::npos,
        "Direkte RAM-Store-Region verliert Tracker-Fail-Closed, geordnetes "
        "Batchflush oder den Exit vor dem naechsten nativen Block.");

    auto direct_store_port_request = direct_store_batch_request;
    direct_store_port_request.emit_run_functions = false;
    const auto direct_store_port_source =
        katana::codegen::emit_cpp_port_translation_unit(
            direct_store_port_request)
            .joined_text();
    const auto direct_store_instruction =
        emitted_instruction(direct_store_port_source, "0x8C051000");
    require(
        direct_store_port_source.find(
            "katana::runtime::canonical_physical_address_inline(") !=
                std::string::npos &&
            direct_store_port_source.find(
                "katana::runtime::canonical_physical_address(") ==
                std::string::npos,
        "Generierter AOT-Hotpath benutzt den externen Canonicalization-"
        "ABI-Wrapper statt des ABI-neutralen Inline-Helpers.");
    const auto direct_store_smc_exit =
        direct_store_instruction.find(
            "if (katana_guest_write_exit_requested) {");
    const auto direct_store_exit_source =
        direct_store_instruction.find(
            "runtime_dispatch_detail::active_exit_source = {",
            direct_store_smc_exit);
    const auto direct_store_exit_kind =
        direct_store_instruction.find(
            "katana::runtime::BlockEndKind::Fallthrough;",
            direct_store_exit_source);
    const auto direct_store_exit_pc =
        direct_store_instruction.find(
            "cpu.pc = katana::runtime::relocate_code_address("
            "0x8C051002u);",
            direct_store_exit_kind);
    require(
        direct_store_smc_exit != std::string_view::npos &&
            direct_store_exit_source != std::string_view::npos &&
            direct_store_exit_kind != std::string_view::npos &&
            direct_store_exit_pc != std::string_view::npos &&
            direct_store_smc_exit < direct_store_exit_source &&
            direct_store_exit_source < direct_store_exit_kind &&
            direct_store_exit_kind < direct_store_exit_pc,
        "Skalarer SMC-Exit verliert Store-PC, Fallthrough-Kante oder "
        "Direct-Blockmetadaten.");

    constexpr std::array<std::uint8_t, 8> fmov_memory_bytes = {
        0x28u,
        0xF0u, // FMOV.S @R2,FR0
        0x0Au,
        0xF4u, // FMOV.S FR0,@R4
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto fmov_memory_lines = katana::sh4::disassemble(fmov_memory_bytes, 0x8C060000u);
    constexpr std::array<std::uint32_t, 1> fmov_memory_seeds = {0x8C060000u};
    const auto fmov_memory_functions =
        katana::analysis::discover_functions(fmov_memory_lines, fmov_memory_seeds);
    const auto fmov_memory_program =
        katana::ir::lower_program(fmov_memory_lines, fmov_memory_functions);
    const auto fmov_memory_source =
        katana::codegen::emit_cpp_program(fmov_memory_program, 0x8C060000u);
    const auto fmov_load = emitted_instruction(fmov_memory_source, "0x8C060000");
    const auto fmov_store = emitted_instruction(fmov_memory_source, "0x8C060002");
    require(count_occurrences(
                fmov_load,
                "GuestInstructionOrigin guest_origin{0x8C060000u") == 1u &&
                count_occurrences(fmov_load,
                                  "katana_direct_ram_read_u32(guest_origin") == 3u &&
                count_occurrences(
                    fmov_store,
                    "GuestInstructionOrigin guest_origin{0x8C060002u") == 1u &&
                count_occurrences(fmov_store,
                                  "katana_direct_ram_write_u32("
                                  "katana_direct_ram_writes, guest_origin") == 3u &&
                count_occurrences(fmov_store, "CodeWriteSource::Fpu") == 3u,
            "FMOV.S erzeugt fuer 32-/64-Bit-Pfade nicht 1/2 Events mit FPU-Writequelle.");

    constexpr std::array<std::uint8_t, 8> mac_memory_bytes = {
        0x1Fu,
        0x42u, // MAC.W @R1+,@R2+
        0x3Fu,
        0x04u, // MAC.L @R3+,@R4+
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto mac_memory_lines = katana::sh4::disassemble(mac_memory_bytes, 0x8C070000u);
    constexpr std::array<std::uint32_t, 1> mac_memory_seeds = {0x8C070000u};
    const auto mac_memory_functions =
        katana::analysis::discover_functions(mac_memory_lines, mac_memory_seeds);
    const auto mac_memory_program =
        katana::ir::lower_program(mac_memory_lines, mac_memory_functions);
    const auto mac_memory_source =
        katana::codegen::emit_cpp_program(mac_memory_program, 0x8C070000u);
    const auto mac_word = emitted_instruction(mac_memory_source, "0x8C070000");
    const auto mac_long = emitted_instruction(mac_memory_source, "0x8C070002");
    require(count_occurrences(
                mac_word,
                "GuestInstructionOrigin guest_origin{0x8C070000u") == 1u &&
                count_occurrences(mac_word,
                                  "katana_direct_ram_read_u16(guest_origin") == 2u &&
                count_occurrences(
                    mac_long,
                    "GuestInstructionOrigin guest_origin{0x8C070002u") == 1u &&
                count_occurrences(mac_long,
                                  "katana_direct_ram_read_u32(guest_origin") == 2u,
            "MAC.W/MAC.L erzeugen nicht je zwei Reads mit ihrem gemeinsamen Instruktionsorigin.");

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
    katana::ir::Instruction faulting_timing = first_timing;
    faulting_timing.source_address = 0x3004u;
    faulting_timing.original_opcode = 0x6012u; // MOV.L @R1,R0
    faulting_timing.original_operation = katana::ir::Operation::LoadLong;
    faulting_timing.operation = katana::ir::Operation::LoadLong;
    faulting_timing.source_register = 1u;
    faulting_timing.widths =
        katana::ir::operation_operand_widths(faulting_timing.operation);
    faulting_timing.status_effects =
        katana::ir::instruction_status_effects(faulting_timing.operation);
    faulting_timing.memory_effects = katana::ir::instruction_memory_effects(
        faulting_timing.operation,
        faulting_timing.destination_register,
        faulting_timing.source_register);
    faulting_timing.accumulator_effects =
        katana::ir::operation_accumulator_effects(faulting_timing.operation);
    katana::ir::BasicBlock timing_block;
    timing_block.start_address = 0x3000u;
    timing_block.instructions = {first_timing, second_timing, faulting_timing};
    katana::ir::Function timing_function;
    timing_function.entry_address = 0x3000u;
    timing_function.blocks = {std::move(timing_block)};
    const auto timing_source =
        katana::codegen::emit_cpp_program(std::vector{timing_function}, 0x3000u);
    const auto first_timing_source = emitted_instruction(timing_source, "0x00003000");
    const auto second_timing_source = emitted_instruction(timing_source, "0x00003002");
    const auto faulting_timing_source = emitted_instruction(timing_source, "0x00003004");
    const auto batched_attempts =
        timing_source.find("cpu.attempted_guest_instructions += 2u;");
    const auto batched_retirements =
        timing_source.find("cpu.retired_guest_instructions += 2u;", batched_attempts);
    const auto batched_cycles =
        timing_source.find("cpu.pending_guest_cycles += 2u;", batched_retirements);
    require(first_timing_source.find("ExplicitGuestInstructionAttempt") ==
                    std::string_view::npos &&
                second_timing_source.find("ExplicitGuestInstructionAttempt") ==
                    std::string_view::npos &&
                batched_attempts != std::string::npos &&
                batched_retirements != std::string::npos &&
                batched_cycles != std::string::npos &&
                count_occurrences(timing_source,
                                  "cpu.attempted_guest_instructions += 2u;") == 1u &&
                count_occurrences(timing_source,
                                  "cpu.retired_guest_instructions += 2u;") == 1u &&
                count_occurrences(timing_source, "cpu.pending_guest_cycles += 2u;") == 1u &&
                faulting_timing_source.find(
                    "ExplicitGuestInstructionAttempt guest_instruction_attempt") !=
                    std::string_view::npos &&
                faulting_timing_source.find(
                    "katana::runtime::relocate_code_address(0x00003004u), 2u);") !=
                    std::string_view::npos &&
                batched_cycles < timing_source.find("// katana-guest 0x00003004u") &&
                timing_source.find("katana::runtime::finalize_guest_block(") !=
                    std::string::npos &&
                timing_source.find("base_guest_cycles_per_instruction") == std::string::npos,
            "Eine reine NOP-Praefixregion wird nicht gebatcht vor der exakten "
            "Memory-Fault-Grenze verbucht.");

    constexpr std::array<std::uint8_t, 8> sr_boundary_bytes = {
        0x0Eu,
        0x41u, // LDC R1,SR
        0x13u,
        0x62u, // MOV R1,R2
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto sr_boundary_lines =
        katana::sh4::disassemble(sr_boundary_bytes, 0x8C060000u);
    constexpr std::array<std::uint32_t, 1> sr_boundary_seeds = {0x8C060000u};
    const auto sr_boundary_functions =
        katana::analysis::discover_functions(sr_boundary_lines, sr_boundary_seeds);
    const auto sr_boundary_program =
        katana::ir::lower_program(sr_boundary_lines, sr_boundary_functions);
    katana::codegen::BackendRequest sr_boundary_request{
        sr_boundary_program, 0x8C060000u};
    sr_boundary_request.single_block_execution = true;
    sr_boundary_request.guarded_local_block_chaining = true;
    sr_boundary_request.external_dynamic_dispatch = true;
    sr_boundary_request.conservative_register_localization = true;
    const auto sr_boundary_source =
        katana::codegen::CppBackend{}
            .emit(sr_boundary_request)
            .joined_text();
    const auto sr_instruction =
        emitted_instruction(sr_boundary_source, "0x8C060000");
    const auto sr_write = sr_instruction.find("cpu.write_sr(value);");
    const auto sr_retirement =
        sr_instruction.find("guest_instruction_attempt.complete();", sr_write);
    const auto sr_continuation = sr_instruction.find(
        "cpu.pc = katana::runtime::relocate_code_address(0x8C060002u);",
        sr_retirement);
    const auto sr_finalize = sr_instruction.find(
        "katana_commit_post_instruction_safepoint(", sr_continuation);
    const auto sr_flush =
        sr_instruction.find("katana_registers.flush_release();");
    const auto sr_reload =
        sr_instruction.find("katana_registers.reload_acquire();", sr_finalize);
    const auto sr_next_instruction =
        emitted_instruction(sr_boundary_source, "0x8C060002");
    require(
        sr_write != std::string_view::npos &&
            sr_retirement != std::string_view::npos &&
            sr_continuation != std::string_view::npos &&
            sr_finalize != std::string_view::npos &&
            sr_flush != std::string_view::npos &&
            sr_reload != std::string_view::npos &&
            sr_write < sr_retirement && sr_retirement < sr_continuation &&
            sr_continuation < sr_finalize &&
            sr_flush < sr_write && sr_finalize < sr_reload &&
            sr_next_instruction.find(
                "cpu.r[2] = katana_registers[1];") !=
                std::string_view::npos,
        "LDC SR laesst nach IMASK/BL/RB/MD-Aenderung weitere native "
        "Gastinstruktionen vor dem Runtime-Safepoint laufen oder bindet die "
        "neue Registerbank nicht erneut.");

    {
        std::string fragment =
            "cpu.r[1] = cpu.r[10]; cpu.t = cpu.pr;\n"
            "// cpu.r[1] cpu.t \\\n"
            "cpu.pr remains in the spliced comment\n"
            "/* cpu.r[10] cpu.t cpu.pr */\n"
            "const auto normal = \"cpu.r[1] \\\" cpu.t cpu.pr\";\n"
            "const auto character = 'cpu.t';\n"
            "const auto raw = u8R\"tag(cpu.r[10] cpu.t cpu.pr)tag\";\n"
            "some_cpu.r[1] = cpu.toggle_fpu_register_bank() + cpu.prior;\n"
            "cpu.r[1] = cpu.pr;\n";
        katana::codegen::detail::replace_cpp_code_token(
            fragment, "cpu.r[1]", "katana_registers[1]");
        katana::codegen::detail::replace_cpp_code_token(
            fragment, "cpu.r[10]", "katana_registers[10]");
        katana::codegen::detail::replace_cpp_code_token(
            fragment, "cpu.t", "katana_registers.t()");
        katana::codegen::detail::replace_cpp_code_token(
            fragment, "cpu.pr", "katana_registers.pr()");
        require(
            fragment.find(
                "katana_registers[1] = katana_registers[10]; "
                "katana_registers.t() = katana_registers.pr();") !=
                    std::string::npos &&
                fragment.find("// cpu.r[1] cpu.t \\\n"
                              "cpu.pr remains in the spliced comment") !=
                    std::string::npos &&
                fragment.find("/* cpu.r[10] cpu.t cpu.pr */") !=
                    std::string::npos &&
                fragment.find(
                    "\"cpu.r[1] \\\" cpu.t cpu.pr\"") !=
                    std::string::npos &&
                fragment.find("'cpu.t'") != std::string::npos &&
                fragment.find(
                    "u8R\"tag(cpu.r[10] cpu.t cpu.pr)tag\"") !=
                    std::string::npos &&
                fragment.find(
                    "some_cpu.r[1] = cpu.toggle_fpu_register_bank() + "
                    "cpu.prior;") != std::string::npos &&
                fragment.find(
                    "katana_registers[1] = katana_registers.pr();") !=
                    std::string::npos,
            "Registerlokalisierung veraenderte Kommentare, Literale, "
            "Raw-Strings oder groessere C++-Tokens.");
    }

    constexpr std::array<std::uint8_t, 10> token_boundary_bytes = {
        0x18u,
        0x00u, // SETT
        0xFDu,
        0xFBu, // FRCHG
        0x29u,
        0x00u, // MOVT R0
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto token_boundary_lines =
        katana::sh4::disassemble(token_boundary_bytes, 0x8C060100u);
    constexpr std::array<std::uint32_t, 1> token_boundary_seeds = {
        0x8C060100u};
    const auto token_boundary_functions =
        katana::analysis::discover_functions(
            token_boundary_lines, token_boundary_seeds);
    const auto token_boundary_program = katana::ir::lower_program(
        token_boundary_lines, token_boundary_functions);
    katana::codegen::BackendRequest token_boundary_request{
        token_boundary_program, 0x8C060100u};
    token_boundary_request.single_block_execution = true;
    token_boundary_request.guarded_local_block_chaining = true;
    token_boundary_request.external_dynamic_dispatch = true;
    token_boundary_request.conservative_register_localization = true;
    const auto token_boundary_source =
        katana::codegen::CppBackend{}
            .emit(token_boundary_request)
            .joined_text();
    require(
        token_boundary_source.find("katana_registers.t() = true;") !=
                std::string::npos &&
            token_boundary_source.find(
                "cpu.toggle_fpu_register_bank();") !=
                std::string::npos &&
            token_boundary_source.find(
                "katana_registers.t()oggle_fpu_register_bank") ==
                std::string::npos,
        "T-Lokalisierung ersetzt ein laengeres CpuState-Accessor-Praefix.");

    constexpr std::array<std::uint8_t, 8> privileged_token_bytes = {
        0x2Au,
        0x00u, // STS PR,R0
        0x2Au,
        0x01u, // STS PR,R1
        0x2Bu,
        0x00u, // RTE
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto privileged_token_lines =
        katana::sh4::disassemble(privileged_token_bytes, 0x8C060200u);
    constexpr std::array<std::uint32_t, 1> privileged_token_seeds = {
        0x8C060200u};
    const auto privileged_token_functions =
        katana::analysis::discover_functions(
            privileged_token_lines, privileged_token_seeds);
    const auto privileged_token_program = katana::ir::lower_program(
        privileged_token_lines, privileged_token_functions);
    katana::codegen::BackendRequest privileged_token_request{
        privileged_token_program, 0x8C060200u};
    privileged_token_request.single_block_execution = true;
    privileged_token_request.guarded_local_block_chaining = true;
    privileged_token_request.external_dynamic_dispatch = true;
    privileged_token_request.conservative_register_localization = true;
    const auto privileged_token_source =
        katana::codegen::CppBackend{}
            .emit(privileged_token_request)
            .joined_text();
    const auto rte_release = privileged_token_source.find(
        "katana_registers.flush_release();");
    const auto rte_privilege =
        privileged_token_source.find("cpu.privileged_mode_inline()", rte_release);
    const auto rte_apply =
        privileged_token_source.find("return_from_exception(cpu);", rte_privilege);
    const auto rte_reload = privileged_token_source.find(
        "katana_registers.reload_acquire();", rte_apply);
    require(
        privileged_token_source.find("katana_registers.pr()") !=
                std::string::npos &&
            rte_release != std::string::npos &&
            rte_privilege != std::string::npos &&
            rte_apply != std::string::npos &&
            rte_reload != std::string::npos &&
            rte_release < rte_privilege && rte_privilege < rte_apply &&
            rte_apply < rte_reload &&
            privileged_token_source.find(
                "katana_registers.pr()ivileged_mode") ==
                std::string::npos,
        "PR-Lokalisierung korrumpiert den Privilegguard oder haelt die alte "
        "Registerbank ueber RTE.");

    constexpr std::array<std::uint8_t, 8> ldtlb_boundary_bytes = {
        0x38u,
        0x00u, // LDTLB
        0x09u,
        0x00u, // NOP
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP (Delay Slot)
    };
    const auto ldtlb_boundary_lines =
        katana::sh4::disassemble(ldtlb_boundary_bytes, 0x8C061000u);
    constexpr std::array<std::uint32_t, 1> ldtlb_boundary_seeds = {0x8C061000u};
    const auto ldtlb_boundary_functions =
        katana::analysis::discover_functions(ldtlb_boundary_lines, ldtlb_boundary_seeds);
    const auto ldtlb_boundary_program =
        katana::ir::lower_program(ldtlb_boundary_lines, ldtlb_boundary_functions);
    const auto ldtlb_boundary_source =
        katana::codegen::emit_cpp_program(ldtlb_boundary_program, 0x8C061000u);
    const auto ldtlb_instruction =
        emitted_instruction(ldtlb_boundary_source, "0x8C061000");
    const auto ldtlb_write =
        ldtlb_instruction.find("katana::runtime::load_tlb(cpu);");
    const auto ldtlb_retirement =
        ldtlb_instruction.find("guest_instruction_attempt.complete();", ldtlb_write);
    const auto ldtlb_finalize = ldtlb_instruction.find(
        "katana_commit_post_instruction_safepoint(", ldtlb_retirement);
    require(
        ldtlb_write != std::string_view::npos &&
            ldtlb_retirement != std::string_view::npos &&
            ldtlb_finalize != std::string_view::npos &&
            ldtlb_write < ldtlb_retirement && ldtlb_retirement < ldtlb_finalize,
        "LDTLB aktualisiert die MMU ohne unmittelbaren Runtime-Safepoint.");

    constexpr std::array<std::uint8_t, 4> return_sr_delay_bytes = {
        0x0Bu,
        0x00u, // RTS
        0x0Eu,
        0x41u // LDC R1,SR (Delay Slot)
    };
    const auto return_sr_delay_lines =
        katana::sh4::disassemble(return_sr_delay_bytes, 0x8C062000u);
    constexpr std::array<std::uint32_t, 1> return_sr_delay_seeds = {0x8C062000u};
    const auto return_sr_delay_functions =
        katana::analysis::discover_functions(return_sr_delay_lines, return_sr_delay_seeds);
    const auto return_sr_delay_program =
        katana::ir::lower_program(return_sr_delay_lines, return_sr_delay_functions);
    katana::codegen::BackendRequest return_sr_delay_request{
        return_sr_delay_program, 0x8C062000u};
    return_sr_delay_request.single_block_execution = true;
    return_sr_delay_request.guarded_local_block_chaining = true;
    return_sr_delay_request.external_dynamic_dispatch = true;
    const auto return_sr_delay_source =
        katana::codegen::CppBackend{}.emit(return_sr_delay_request).joined_text();
    const auto return_sr_write =
        return_sr_delay_source.find("cpu.write_sr(value);");
    const auto return_sr_deferred = return_sr_delay_source.find(
        "katana_deferred_safepoint_8C062002 = true;", return_sr_write);
    const auto return_sr_target =
        return_sr_delay_source.find("cpu.pc = return_target;", return_sr_deferred);
    const auto return_sr_terminal = return_sr_delay_source.find(
        "terminal_instruction_attempt.complete();", return_sr_target);
    const auto return_sr_finalize = return_sr_delay_source.find(
        "katana_commit_post_instruction_safepoint(", return_sr_terminal);
    const auto return_sr_return =
        return_sr_delay_source.find("return;", return_sr_finalize);
    require(
        return_sr_write != std::string::npos &&
            return_sr_deferred != std::string::npos &&
            return_sr_target != std::string::npos &&
            return_sr_terminal != std::string::npos &&
            return_sr_finalize != std::string::npos &&
            return_sr_return != std::string::npos &&
            return_sr_write < return_sr_deferred &&
            return_sr_deferred < return_sr_target &&
            return_sr_target < return_sr_terminal &&
            return_sr_terminal < return_sr_finalize &&
            return_sr_finalize < return_sr_return &&
            count_occurrences(return_sr_delay_source,
                              "if (katana_commit_post_instruction_safepoint(") == 1u,
        "Ein SR-Write im RTS-Delay-Slot kehrt ohne post-Return-Safepoint "
        "in den nativen Caller zurueck.");

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
            cache_source.find("katana_direct_ram_write_u32("
                              "katana_direct_ram_writes, guest_origin, "
                              "cpu.r[9], cpu.r[0], "
                              "katana::runtime::CodeWriteSource::StoreQueue, "
                              "katana_guarded_unknown_ram_writes)") !=
                std::string::npos &&
            cache_source.find("services->prefetch(cpu, guest_origin, cpu.r[3])") !=
                std::string::npos &&
            cache_source.find("enter_memory_exception_with_provenance(cpu, error, "
                              "katana::runtime::relocate_code_address(0x0000400Au), ") !=
                std::string::npos,
        "Der C++-Emitter laesst LDTLB/cache instructions aus, verwechselt Register oder "
        "faengt PREF-MMU-Fehler nicht am SH-4-Exceptionpfad.");

    std::cout << "Alle C++-Codegenerator-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
