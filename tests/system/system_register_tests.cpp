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

using IrRegister = katana::ir::SpecialRegister;
using Operation = katana::ir::Operation;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::ir::Instruction transfer(const std::uint32_t address,
                                 const Operation operation,
                                 const IrRegister special_register,
                                 const std::uint8_t general_register) {
    katana::ir::Instruction instruction;
    instruction.source_address = address;
    instruction.operation = operation;
    instruction.special_register = special_register;
    instruction.widths = katana::ir::operation_operand_widths(operation);
    instruction.memory_effects = katana::ir::instruction_memory_effects(operation);
    instruction.status_effects =
        katana::ir::instruction_status_effects(operation, special_register);
    instruction.accumulator_effects =
        katana::ir::operation_accumulator_effects(operation, special_register);
    instruction.is_privileged =
        special_register == IrRegister::Sr || special_register == IrRegister::Vbr ||
        special_register == IrRegister::Ssr || special_register == IrRegister::Spc ||
        special_register == IrRegister::Sgr || special_register == IrRegister::Dbr ||
        (special_register >= IrRegister::Bank0 && special_register <= IrRegister::Bank7);
    if (operation == Operation::LoadSpecialRegister ||
        operation == Operation::LoadSpecialRegisterPostIncrement) {
        instruction.source_register = general_register;
    } else {
        instruction.destination_register = general_register;
    }
    return instruction;
}

void append_function(std::vector<katana::ir::Function>& program,
                     const std::uint32_t entry,
                     std::vector<katana::ir::Instruction> instructions) {
    katana::ir::Instruction return_instruction;
    return_instruction.source_address =
        entry + static_cast<std::uint32_t>(instructions.size() * 2u);
    return_instruction.original_opcode = 0x000Bu;
    return_instruction.operation = Operation::Return;
    return_instruction.widths = katana::ir::operation_operand_widths(Operation::Return);
    return_instruction.memory_effects = katana::ir::instruction_memory_effects(Operation::Return);
    return_instruction.status_effects = katana::ir::instruction_status_effects(Operation::Return);
    return_instruction.delay_slot = {katana::ir::DelaySlotRole::Owner,
                                     return_instruction.source_address + 2u};
    instructions.push_back(return_instruction);

    katana::ir::Instruction delay_slot;
    delay_slot.source_address = return_instruction.source_address + 2u;
    delay_slot.original_opcode = 0x0009u;
    delay_slot.operation = Operation::Nop;
    delay_slot.widths = katana::ir::operation_operand_widths(Operation::Nop);
    delay_slot.memory_effects = katana::ir::instruction_memory_effects(Operation::Nop);
    delay_slot.status_effects = katana::ir::instruction_status_effects(Operation::Nop);
    delay_slot.delay_slot = {katana::ir::DelaySlotRole::Slot, return_instruction.source_address};
    instructions.push_back(delay_slot);

    katana::ir::BasicBlock block;
    block.start_address = entry;
    block.instructions = std::move(instructions);

    katana::ir::Function function;
    function.entry_address = entry;
    function.blocks = {std::move(block)};
    program.push_back(std::move(function));
}

std::vector<katana::ir::Function> build_program() {
    std::vector<katana::ir::Function> program;
    constexpr std::array registers = {IrRegister::Mach,
                                      IrRegister::Macl,
                                      IrRegister::Pr,
                                      IrRegister::Fpul,
                                      IrRegister::Fpscr,
                                      IrRegister::Sr,
                                      IrRegister::Gbr,
                                      IrRegister::Vbr,
                                      IrRegister::Ssr,
                                      IrRegister::Spc,
                                      IrRegister::Dbr,
                                      IrRegister::Bank0,
                                      IrRegister::Bank1,
                                      IrRegister::Bank2,
                                      IrRegister::Bank3,
                                      IrRegister::Bank4,
                                      IrRegister::Bank5,
                                      IrRegister::Bank6,
                                      IrRegister::Bank7};

    for (std::size_t index = 0; index < registers.size(); ++index) {
        const auto entry = 0x100u + static_cast<std::uint32_t>(index) * 0x10u;
        append_function(
            program,
            entry,
            {transfer(entry, Operation::LoadSpecialRegister, registers[index], 0u),
             transfer(entry + 2u, Operation::StoreSpecialRegister, registers[index], 1u)});
    }

    append_function(
        program, 0x230u, {transfer(0x230u, Operation::StoreSpecialRegister, IrRegister::Sgr, 1u)});
    append_function(
        program,
        0x240u,
        {transfer(0x240u, Operation::StoreSpecialRegisterPreDecrement, IrRegister::Pr, 2u),
         transfer(0x242u, Operation::LoadSpecialRegisterPostIncrement, IrRegister::Mach, 2u)});
    append_function(
        program,
        0x250u,
        {transfer(0x250u, Operation::StoreSpecialRegisterPreDecrement, IrRegister::Sr, 8u),
         transfer(0x252u, Operation::LoadSpecialRegisterPostIncrement, IrRegister::Sr, 8u)});
    append_function(
        program,
        0x260u,
        {transfer(0x260u, Operation::StoreSpecialRegisterPreDecrement, IrRegister::Pr, 2u)});
    append_function(
        program,
        0x270u,
        {transfer(0x270u, Operation::LoadSpecialRegisterPostIncrement, IrRegister::Pr, 2u)});
    return program;
}

void test_decoder_matrix() {
    using Kind = katana::sh4::InstructionKind;
    struct Family {
        std::uint16_t suffix;
        Kind kind;
        bool privileged;
    };

    constexpr std::array system_families = {
        Family{0x000Au, Kind::StoreSpecialRegister, false},
        Family{0x4002u, Kind::StoreSpecialRegisterPreDecrement, false},
        Family{0x400Au, Kind::LoadSpecialRegister, false},
        Family{0x4006u, Kind::LoadSpecialRegisterPostIncrement, false}};
    constexpr std::array system_offsets = {0x00u, 0x10u, 0x20u, 0x50u, 0x60u};

    for (const auto& family : system_families) {
        for (const auto offset : system_offsets) {
            const auto decoded =
                katana::sh4::decode(static_cast<std::uint16_t>(family.suffix + offset + 0x0F00u));
            require(decoded.kind == family.kind && !decoded.is_privileged,
                    "Eine STS/LDS-Codierung fehlt oder ist falsch markiert.");
        }
    }

    constexpr std::array control_direct_store = {0x0002u,
                                                 0x0012u,
                                                 0x0022u,
                                                 0x0032u,
                                                 0x0042u,
                                                 0x003Au,
                                                 0x00FAu,
                                                 0x0082u,
                                                 0x0092u,
                                                 0x00A2u,
                                                 0x00B2u,
                                                 0x00C2u,
                                                 0x00D2u,
                                                 0x00E2u,
                                                 0x00F2u};
    constexpr std::array control_memory_store = {0x4003u,
                                                 0x4013u,
                                                 0x4023u,
                                                 0x4033u,
                                                 0x4043u,
                                                 0x4032u,
                                                 0x40F2u,
                                                 0x4083u,
                                                 0x4093u,
                                                 0x40A3u,
                                                 0x40B3u,
                                                 0x40C3u,
                                                 0x40D3u,
                                                 0x40E3u,
                                                 0x40F3u};
    constexpr std::array control_direct_load = {0x400Eu,
                                                0x401Eu,
                                                0x402Eu,
                                                0x403Eu,
                                                0x404Eu,
                                                0x40FAu,
                                                0x408Eu,
                                                0x409Eu,
                                                0x40AEu,
                                                0x40BEu,
                                                0x40CEu,
                                                0x40DEu,
                                                0x40EEu,
                                                0x40FEu};
    constexpr std::array control_memory_load = {0x4007u,
                                                0x4017u,
                                                0x4027u,
                                                0x4037u,
                                                0x4047u,
                                                0x40F6u,
                                                0x4087u,
                                                0x4097u,
                                                0x40A7u,
                                                0x40B7u,
                                                0x40C7u,
                                                0x40D7u,
                                                0x40E7u,
                                                0x40F7u};

    const auto check_control = [](const auto& opcodes, const Kind kind) {
        for (const auto opcode : opcodes) {
            const auto decoded = katana::sh4::decode(static_cast<std::uint16_t>(opcode | 0x0700u));
            const bool is_gbr = (opcode & 0x00FFu) == 0x12u || (opcode & 0x00FFu) == 0x13u ||
                                (opcode & 0x00FFu) == 0x1Eu || (opcode & 0x00FFu) == 0x17u;
            require(decoded.kind == kind && decoded.is_privileged != is_gbr,
                    "Eine STC/LDC-Codierung fehlt oder ist falsch markiert.");
        }
    };

    check_control(control_direct_store, Kind::StoreSpecialRegister);
    check_control(control_memory_store, Kind::StoreSpecialRegisterPreDecrement);
    check_control(control_direct_load, Kind::LoadSpecialRegister);
    check_control(control_memory_load, Kind::LoadSpecialRegisterPostIncrement);

    require(katana::sh4::decode(0x075Au).text == "sts fpul, r7" &&
                katana::sh4::decode(0x4832u).text == "stc.l sgr, @-r8" &&
                katana::sh4::decode(0x49F6u).text == "ldc.l @r9+, dbr",
            "Disassemblytexte fuer Spezialregistertransfers sind falsch.");
}

void test_lowering() {
    constexpr std::array<std::uint8_t, 20> bytes = {0x0A, 0x40, 0x0A, 0x01, 0x22, 0x42, 0x26,
                                                    0x43, 0x0E, 0x44, 0x02, 0x05, 0x13, 0x46,
                                                    0x17, 0x47, 0x0B, 0x00, 0x09, 0x00};
    const auto lines = katana::sh4::disassemble(bytes, 0x8000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8000u};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    const auto program = katana::ir::lower_program(lines, functions);

    std::array<std::size_t, 4> counts{};
    std::size_t privileged = 0;
    for (const auto& block : program.front().blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.operation == Operation::StoreSpecialRegister) ++counts[0];
            if (instruction.operation == Operation::StoreSpecialRegisterPreDecrement) ++counts[1];
            if (instruction.operation == Operation::LoadSpecialRegister) ++counts[2];
            if (instruction.operation == Operation::LoadSpecialRegisterPostIncrement) ++counts[3];
            if (instruction.is_privileged) ++privileged;
        }
    }
    require(counts == std::array<std::size_t, 4>{2u, 2u, 2u, 2u} && privileged == 2u,
            "Spezialregistertransfers wurden falsch in die IR abgesenkt.");
}

int emit_fixture(const std::string& output_path) {
    const auto source = katana::codegen::emit_cpp_program(build_program(), 0x100u);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return EXIT_FAILURE;
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den KR-1406-Test.");
    test_decoder_matrix();
    test_lowering();

    const auto source = katana::codegen::emit_cpp_program(build_program(), 0x100u);
    require(source.find("cpu.write_sr(value);") != std::string::npos &&
                source.find("cpu.r_bank[7] = value;") != std::string::npos &&
                source.find("katana::runtime::guest_write_u32(cpu, address, value);") !=
                    std::string::npos &&
                source.find("if (!cpu.privileged_mode())") != std::string::npos,
            "Der C++-Emitter bildet Spezialregistertransfers nicht vollstaendig ab.");

    std::cout << "Alle KR-1406 Decoder-, IR- und Codegen-Tests erfolgreich.\n";
    return EXIT_SUCCESS;
}
