#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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
    constexpr std::array<std::uint8_t, 18> bytes = {0x02,
                                                    0xB0,
                                                    0x09,
                                                    0x00,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00,
                                                    0x05,
                                                    0xE1,
                                                    0xFF,
                                                    0x71,
                                                    0x08,
                                                    0x00,
                                                    0x0B,
                                                    0x00,
                                                    0x09,
                                                    0x00};

    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);

    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};

    const auto discovered = katana::analysis::discover_functions(lines, seeds);

    const auto program = katana::ir::lower_program(lines, discovered);

    require(program.size() == 2, "Das IR-Programm muss zwei Funktionen enthalten.");

    const auto& main_function = program[0];
    const auto& sub_function = program[1];

    require(main_function.entry_address == 0x8C010000u,
            "Die erste IR-Funktion besitzt die falsche Adresse.");
    require(main_function.blocks.size() == 2, "Die erste IR-Funktion muss zwei Bloecke besitzen.");

    const auto& call = main_function.blocks[0].instructions[0];

    require(call.operation == katana::ir::Operation::Call,
            "BSR wurde nicht als IR-Aufruf abgesenkt.");
    require(call.target_address == 0x8C010008u, "Der IR-Aufruf besitzt das falsche Ziel.");
    require(call.delay_slot.role == katana::ir::DelaySlotRole::Owner &&
                call.delay_slot.counterpart_address == 0x8C010002u,
            "Der IR-Aufruf verlor seine Delay-Slot-Eigenschaft.");

    const auto& call_delay = main_function.blocks[0].instructions[1];

    require(call_delay.operation == katana::ir::Operation::Nop,
            "Der Delay Slot des Aufrufs ist kein NOP.");
    require(call_delay.delay_slot.role == katana::ir::DelaySlotRole::Slot &&
                call_delay.delay_slot.counterpart_address == 0x8C010000u,
            "Der IR-Delay-Slot wurde nicht markiert.");

    require(main_function.blocks[1].instructions[0].operation == katana::ir::Operation::Return,
            "RTS wurde nicht als IR-Ruecksprung abgesenkt.");

    require(sub_function.entry_address == 0x8C010008u,
            "Die zweite IR-Funktion besitzt die falsche Adresse.");
    require(sub_function.blocks.size() == 1, "Die zweite IR-Funktion muss einen Block besitzen.");

    const auto& sub_block = sub_function.blocks[0];

    require(sub_block.instructions.size() == 5,
            "Der Unterfunktionsblock besitzt die falsche Groesse.");

    require(sub_block.instructions[0].operation == katana::ir::Operation::MovImmediate,
            "MOV Immediate wurde falsch abgesenkt.");
    require(sub_block.instructions[0].destination_register == 1,
            "MOV Immediate verwendet das falsche Zielregister.");
    require(sub_block.instructions[0].immediate == 5, "MOV Immediate besitzt den falschen Wert.");
    require(sub_block.instructions[0].widths.result == katana::ir::OperandWidth::Bits32 &&
                sub_block.instructions[0].widths.immediate == katana::ir::OperandWidth::Bits8,
            "MOV Immediate verlor seine expliziten Operandbreiten beim Lowering.");

    require(sub_block.instructions[1].operation == katana::ir::Operation::AddImmediate,
            "ADD Immediate wurde falsch abgesenkt.");
    require(sub_block.instructions[1].immediate == -1, "ADD Immediate besitzt den falschen Wert.");
    require(call.widths.displacement == katana::ir::OperandWidth::Bits12 &&
                call.widths.address == katana::ir::OperandWidth::Bits32,
            "BSR verlor Displacement- oder Adressbreite beim Lowering.");

    require(sub_block.instructions[2].operation == katana::ir::Operation::ClearT &&
                katana::ir::contains_status_bit(sub_block.instructions[2].status_effects.writes,
                                                katana::ir::StatusRegisterBit::T),
            "CLRT verlor seinen expliziten T-Schreibeffekt beim Lowering.");

    require(sub_block.instructions[3].operation == katana::ir::Operation::Return,
            "RTS der Unterfunktion wurde falsch abgesenkt.");
    require(sub_block.instructions[4].delay_slot.role == katana::ir::DelaySlotRole::Slot &&
                sub_block.instructions[4].delay_slot.counterpart_address ==
                    sub_block.instructions[3].source_address,
            "Der RTS-Delay-Slot wurde im IR nicht markiert.");

    require(katana::ir::operation_name(katana::ir::Operation::MovImmediate) == "mov_imm",
            "Der IR-Operationsname ist falsch.");

    constexpr std::array<std::uint8_t, 8> dual_role_bytes = {
        0x09u, 0x00u, 0xFFu, 0x8Du, 0x1Cu, 0x31u, 0x09u, 0x00u};
    constexpr std::uint32_t dual_role_base = 0x10000000u;
    auto dual_role_lines = katana::sh4::disassemble(dual_role_bytes, dual_role_base);
    // Recursive discovery retains the normal context when one address is both
    // the BT/S delay slot and its direct target.
    dual_role_lines[2].is_delay_slot = false;
    constexpr std::array<std::uint32_t, 4> dual_role_seeds = {
        dual_role_base, dual_role_base + 2u, dual_role_base + 4u, dual_role_base + 6u};
    const auto dual_role_functions =
        katana::analysis::discover_functions(dual_role_lines, dual_role_seeds);
    const auto dual_role_program = katana::ir::lower_program(dual_role_lines, dual_role_functions);
    require(katana::ir::verify_program(dual_role_program).empty(),
            "BT/S-Ziel am eigenen Delay Slot erzeugt ungueltige IR.");
    const auto dual_role_owner =
        std::find_if(dual_role_program.begin(), dual_role_program.end(), [](const auto& function) {
            return function.entry_address == dual_role_base + 2u;
        });
    const auto dual_role_target =
        std::find_if(dual_role_program.begin(), dual_role_program.end(), [](const auto& function) {
            return function.entry_address == dual_role_base + 4u;
        });
    require(dual_role_program.size() == 4u && dual_role_owner != dual_role_program.end() &&
                dual_role_owner->blocks.size() == 1u &&
                dual_role_owner->blocks.front().instructions.size() == 2u &&
                dual_role_owner->blocks.front().instructions.front().delay_slot.role ==
                    katana::ir::DelaySlotRole::Owner &&
                dual_role_owner->blocks.front().instructions.back().delay_slot.role ==
                    katana::ir::DelaySlotRole::Slot,
            "BT/S verlor den ueberlappenden Delay-Slot-Kontext.");
    require(dual_role_target != dual_role_program.end() && dual_role_target->blocks.size() == 1u &&
                dual_role_target->blocks.front().instructions.front().source_address ==
                    dual_role_base + 4u &&
                dual_role_target->blocks.front().instructions.front().delay_slot.role ==
                    katana::ir::DelaySlotRole::None,
            "BT/S-Delay-Slot wurde nicht zugleich als normaler Zieleinstieg materialisiert.");
    auto dual_role_bf_bytes = dual_role_bytes;
    dual_role_bf_bytes[3] = 0x8Fu;
    auto dual_role_bf_lines = katana::sh4::disassemble(dual_role_bf_bytes, dual_role_base);
    dual_role_bf_lines[2].is_delay_slot = false;
    const auto dual_role_bf_functions =
        katana::analysis::discover_functions(dual_role_bf_lines, dual_role_seeds);
    const auto dual_role_bf_program =
        katana::ir::lower_program(dual_role_bf_lines, dual_role_bf_functions);
    require(dual_role_bf_program.size() == 4u &&
                katana::ir::verify_program(dual_role_bf_program).empty(),
            "BF/S-Ziel am eigenen Delay Slot erzeugt ungueltige IR.");

    const auto lower_product_dual_role = [&](const std::array<std::uint8_t, 8>& image_bytes,
                                             const std::string& name) {
        katana::io::ExecutableImage image(name);
        image.add_segment({".text",
                           dual_role_base,
                           0u,
                           image_bytes.size(),
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {image_bytes.begin(), image_bytes.end()}});
        for (const auto address : dual_role_seeds)
            image.add_entry_point(address);
        const auto analyzed = katana::analysis::analyze_control_flow(image);
        return katana::ir::lower_program(analyzed);
    };
    const auto dual_role_product_program =
        lower_product_dual_role(dual_role_bytes, "dual-role-bt-product-analysis");
    const auto dual_role_bf_product_program =
        lower_product_dual_role(dual_role_bf_bytes, "dual-role-bf-product-analysis");
    require(dual_role_product_program.size() == 4u &&
                katana::ir::verify_program(dual_role_product_program).empty() &&
                dual_role_bf_product_program.size() == 4u &&
                katana::ir::verify_program(dual_role_bf_product_program).empty(),
            "Produktanalyse trennt dual-role BT/S- oder BF/S-Einstiege nicht in gueltige "
            "IR-Funktionen.");

    constexpr std::array<std::uint8_t, 10> guarded_bytes = {
        0x0Bu, 0x00u, 0x09u, 0x00u, 0x01u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    katana::analysis::ControlFlowAnalysisResult guarded_analysis;
    guarded_analysis.recursive.instructions = katana::sh4::disassemble(guarded_bytes, 0x8C020000u);
    guarded_analysis.recursive.functions.push_back(
        {0x8C020000u,
         katana::analysis::AnalysisConfidence::Certain,
         katana::analysis::ControlFlowEvidence::ProvenComplete,
         {katana::analysis::FunctionOrigin::EntryPoint}});
    guarded_analysis.recursive.proven_instruction_addresses = {0x8C020000u, 0x8C020002u};
    guarded_analysis.recursive.guarded_candidate_instruction_addresses = {
        0x8C020004u, 0x8C020006u, 0x8C020008u};
    const auto guarded_program = katana::ir::lower_program(guarded_analysis);
    const auto guarded_function =
        std::find_if(guarded_program.begin(), guarded_program.end(), [](const auto& function) {
            return function.entry_address == 0x8C020004u;
        });
    require(guarded_function != guarded_program.end() && guarded_function->blocks.size() == 1u,
            "Bekannter dynamischer Codeblock wurde nicht fuer AOT-Rekompilierung materialisiert.");

    constexpr std::size_t scaling_function_count = 4096u;
    constexpr std::uint32_t scaling_base = 0x8C100000u;
    std::vector<std::uint8_t> scaling_bytes;
    scaling_bytes.reserve(scaling_function_count * 4u);
    std::vector<katana::analysis::FunctionInfo> scaling_functions;
    scaling_functions.reserve(scaling_function_count);
    for (std::size_t index = 0u; index < scaling_function_count; ++index) {
        scaling_bytes.insert(scaling_bytes.end(), {0x0Bu, 0x00u, 0x09u, 0x00u});
        katana::analysis::FunctionInfo function;
        function.id = index;
        function.entry_address = scaling_base + static_cast<std::uint32_t>(index * 4u);
        function.block_addresses.push_back(function.entry_address);
        scaling_functions.push_back(std::move(function));
    }
    const auto scaling_lines = katana::sh4::disassemble(scaling_bytes, scaling_base);
    const auto scaling_start = std::chrono::steady_clock::now();
    const auto scaling_program = katana::ir::lower_program(scaling_lines, scaling_functions);
    const auto scaling_elapsed = std::chrono::steady_clock::now() - scaling_start;
    require(scaling_program.size() == scaling_function_count,
            "Der gebuendelte Loweringpfad verlor unabhaengige Funktionen.");
    require(scaling_elapsed < std::chrono::seconds(5),
            "Der gebuendelte Loweringpfad baut die globale CFG offenbar pro Funktion neu.");

    std::cout << "Alle IR-Lowering-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
