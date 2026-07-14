#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(
    const bool condition,
    const std::string& message
) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    constexpr std::array<std::uint8_t, 18> bytes = {
        0x02, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x05, 0xE1,
        0xFF, 0x71,
        0x08, 0x00,
        0x0B, 0x00,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    constexpr std::array<std::uint32_t, 1> seeds = {
        0x8C010000u
    };

    const auto discovered =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    const auto program =
        katana::ir::lower_program(
            lines,
            discovered
        );

    require(
        program.size() == 2,
        "Das IR-Programm muss zwei Funktionen enthalten."
    );

    const auto& main_function = program[0];
    const auto& sub_function = program[1];

    require(
        main_function.entry_address == 0x8C010000u,
        "Die erste IR-Funktion besitzt die falsche Adresse."
    );
    require(
        main_function.blocks.size() == 2,
        "Die erste IR-Funktion muss zwei Bloecke besitzen."
    );

    const auto& call =
        main_function.blocks[0].instructions[0];

    require(
        call.operation == katana::ir::Operation::Call,
        "BSR wurde nicht als IR-Aufruf abgesenkt."
    );
    require(
        call.target_address == 0x8C010008u,
        "Der IR-Aufruf besitzt das falsche Ziel."
    );
    require(
        call.delay_slot.role == katana::ir::DelaySlotRole::Owner
            && call.delay_slot.counterpart_address == 0x8C010002u,
        "Der IR-Aufruf verlor seine Delay-Slot-Eigenschaft."
    );

    const auto& call_delay =
        main_function.blocks[0].instructions[1];

    require(
        call_delay.operation == katana::ir::Operation::Nop,
        "Der Delay Slot des Aufrufs ist kein NOP."
    );
    require(
        call_delay.delay_slot.role == katana::ir::DelaySlotRole::Slot
            && call_delay.delay_slot.counterpart_address == 0x8C010000u,
        "Der IR-Delay-Slot wurde nicht markiert."
    );

    require(
        main_function.blocks[1].instructions[0].operation ==
            katana::ir::Operation::Return,
        "RTS wurde nicht als IR-Ruecksprung abgesenkt."
    );

    require(
        sub_function.entry_address == 0x8C010008u,
        "Die zweite IR-Funktion besitzt die falsche Adresse."
    );
    require(
        sub_function.blocks.size() == 1,
        "Die zweite IR-Funktion muss einen Block besitzen."
    );

    const auto& sub_block = sub_function.blocks[0];

    require(
        sub_block.instructions.size() == 5,
        "Der Unterfunktionsblock besitzt die falsche Groesse."
    );

    require(
        sub_block.instructions[0].operation ==
            katana::ir::Operation::MovImmediate,
        "MOV Immediate wurde falsch abgesenkt."
    );
    require(
        sub_block.instructions[0].destination_register == 1,
        "MOV Immediate verwendet das falsche Zielregister."
    );
    require(
        sub_block.instructions[0].immediate == 5,
        "MOV Immediate besitzt den falschen Wert."
    );
    require(
        sub_block.instructions[0].widths.result
                == katana::ir::OperandWidth::Bits32
            && sub_block.instructions[0].widths.immediate
                == katana::ir::OperandWidth::Bits8,
        "MOV Immediate verlor seine expliziten Operandbreiten beim Lowering."
    );

    require(
        sub_block.instructions[1].operation ==
            katana::ir::Operation::AddImmediate,
        "ADD Immediate wurde falsch abgesenkt."
    );
    require(
        sub_block.instructions[1].immediate == -1,
        "ADD Immediate besitzt den falschen Wert."
    );
    require(
        call.widths.displacement == katana::ir::OperandWidth::Bits12
            && call.widths.address == katana::ir::OperandWidth::Bits32,
        "BSR verlor Displacement- oder Adressbreite beim Lowering."
    );

    require(
        sub_block.instructions[2].operation == katana::ir::Operation::ClearT
            && katana::ir::contains_status_bit(
                sub_block.instructions[2].status_effects.writes,
                katana::ir::StatusRegisterBit::T
            ),
        "CLRT verlor seinen expliziten T-Schreibeffekt beim Lowering."
    );

    require(
        sub_block.instructions[3].operation ==
            katana::ir::Operation::Return,
        "RTS der Unterfunktion wurde falsch abgesenkt."
    );
    require(
        sub_block.instructions[4].delay_slot.role == katana::ir::DelaySlotRole::Slot
            && sub_block.instructions[4].delay_slot.counterpart_address
                == sub_block.instructions[3].source_address,
        "Der RTS-Delay-Slot wurde im IR nicht markiert."
    );

    require(
        katana::ir::operation_name(
            katana::ir::Operation::MovImmediate
        ) == "mov_imm",
        "Der IR-Operationsname ist falsch."
    );

    std::cout
        << "Alle IR-Lowering-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
