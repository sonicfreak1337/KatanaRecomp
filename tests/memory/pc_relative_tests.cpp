#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t entry_address = 0x00000100u;

constexpr std::array<std::uint8_t, 12> fixture_bytes = {
    0x02,
    0x91, // mov.w @(4, pc), r1 -> 0x108
    0x03,
    0xD2, // mov.l @(12, pc), r2 -> 0x110 (PC aligned)
    0x09,
    0x00, // nop
    0x04,
    0xC7, // mova @(16, pc), r0 -> 0x118 (PC aligned)
    0x0B,
    0x00, // rts
    0x09,
    0x00 // nop (delay slot)
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void append_synthetic_function(std::vector<katana::ir::Function>& program,
                               const std::uint32_t entry,
                               const katana::ir::Operation operation,
                               const std::uint8_t destination,
                               const std::uint32_t effective_address) {
    katana::ir::Instruction access;
    access.source_address = entry;
    access.operation = operation;
    access.widths = katana::ir::operation_operand_widths(operation);
    access.memory_effects = katana::ir::instruction_memory_effects(operation);
    access.status_effects = katana::ir::instruction_status_effects(operation);
    access.destination_register = destination;
    access.effective_address = effective_address;

    katana::ir::Instruction return_instruction;
    return_instruction.source_address = entry + 2u;
    return_instruction.original_opcode = 0x000Bu;
    return_instruction.operation = katana::ir::Operation::Return;
    return_instruction.widths = katana::ir::operation_operand_widths(katana::ir::Operation::Return);
    return_instruction.memory_effects =
        katana::ir::instruction_memory_effects(katana::ir::Operation::Return);
    return_instruction.status_effects =
        katana::ir::instruction_status_effects(katana::ir::Operation::Return);
    return_instruction.delay_slot = {katana::ir::DelaySlotRole::Owner,
                                     return_instruction.source_address + 2u};

    katana::ir::Instruction delay_slot;
    delay_slot.source_address = return_instruction.source_address + 2u;
    delay_slot.original_opcode = 0x0009u;
    delay_slot.operation = katana::ir::Operation::Nop;
    delay_slot.widths = katana::ir::operation_operand_widths(katana::ir::Operation::Nop);
    delay_slot.memory_effects = katana::ir::instruction_memory_effects(katana::ir::Operation::Nop);
    delay_slot.status_effects = katana::ir::instruction_status_effects(katana::ir::Operation::Nop);
    delay_slot.delay_slot = {katana::ir::DelaySlotRole::Slot, return_instruction.source_address};

    katana::ir::BasicBlock block;
    block.start_address = entry;
    block.instructions = {access, return_instruction, delay_slot};

    katana::ir::Function function;
    function.entry_address = entry;
    function.blocks = {block};
    program.push_back(function);
}

std::vector<katana::ir::Function> build_program() {
    const auto lines = katana::sh4::disassemble(fixture_bytes, entry_address);
    constexpr std::array<std::uint32_t, 1> seeds = {entry_address};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, functions);

    append_synthetic_function(
        program, 0x00000200u, katana::ir::Operation::LoadWordSignedPcRelative, 3u, 1024u * 1024u);
    append_synthetic_function(
        program, 0x00000210u, katana::ir::Operation::LoadLongPcRelative, 4u, 0u);

    return program;
}

const katana::ir::Instruction*
find_source_instruction(const std::vector<katana::ir::Function>& program,
                        const std::uint32_t source_address) {
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address == source_address) {
                    return &instruction;
                }
            }
        }
    }
    return nullptr;
}

int emit_fixture(const std::string& output_path) {
    const auto source = katana::codegen::emit_cpp_program(build_program(), entry_address);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return EXIT_FAILURE;
    }
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den KR-1405-Test.");

    const auto word = katana::sh4::decode(0x93FFu);
    const auto longword = katana::sh4::decode(0xD4FFu);
    const auto mova = katana::sh4::decode(0xC7FFu);

    require(word.kind == InstructionKind::MovWordLoadPcRelative &&
                word.destination_register == 3u && word.displacement == 510 &&
                word.text == "mov.w @(510, pc), r3",
            "PC-relativer MOV.W-Load wurde falsch dekodiert.");
    require(longword.kind == InstructionKind::MovLongLoadPcRelative &&
                longword.destination_register == 4u && longword.displacement == 1020 &&
                longword.text == "mov.l @(1020, pc), r4",
            "PC-relativer MOV.L-Load wurde falsch dekodiert.");
    require(mova.kind == InstructionKind::MoveAddressPcRelative &&
                mova.destination_register == 0u && mova.displacement == 1020 &&
                mova.text == "mova @(1020, pc), r0",
            "MOVA wurde falsch dekodiert.");

    const auto program = build_program();
    const auto* lowered_word = find_source_instruction(program, 0x100u);
    const auto* lowered_long = find_source_instruction(program, 0x102u);
    const auto* lowered_mova = find_source_instruction(program, 0x106u);

    require(lowered_word != nullptr &&
                lowered_word->operation == Operation::LoadWordSignedPcRelative &&
                lowered_word->effective_address == 0x108u,
            "MOV.W verwendet nicht PC + 4 + disp mal 2.");
    require(lowered_long != nullptr && lowered_long->operation == Operation::LoadLongPcRelative &&
                lowered_long->effective_address == 0x110u,
            "MOV.L richtet den Instruktions-PC nicht auf vier Byte aus.");
    require(lowered_mova != nullptr &&
                lowered_mova->operation == Operation::MoveAddressPcRelative &&
                lowered_mova->effective_address == 0x118u,
            "MOVA richtet den Instruktions-PC nicht auf vier Byte aus.");

    const auto source = katana::codegen::emit_cpp_program(program, entry_address);
    require(source.find("guest_read_s16(cpu, 0x00000108u)") != std::string::npos &&
                source.find("guest_read_u32(cpu, 0x00000110u)") != std::string::npos &&
                source.find("cpu.r[0] = 0x00000118u;") != std::string::npos,
            "Der Codegenerator verwendet falsche PC-relative Adressen.");

    std::cout << "Alle KR-1405 Decoder-, IR- und Codegen-Tests erfolgreich.\n";
    return EXIT_SUCCESS;
}
