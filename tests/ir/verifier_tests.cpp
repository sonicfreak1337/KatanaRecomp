#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool has_issue(
    const std::vector<katana::ir::VerificationIssue>& issues,
    const std::string& fragment
) {
    for (const auto& issue : issues) {
        if (issue.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

}

int main() {
    constexpr std::array<std::uint8_t, 6> bytes = {
        0x00u, 0xE1u, // MOV #0,R1
        0x0Bu, 0x00u, // RTS
        0x09u, 0x00u  // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto discovered = katana::analysis::discover_functions(lines, seeds);
    auto program = katana::ir::lower_program(lines, discovered);

    require(
        katana::ir::verify_function(program.front()).empty(),
        "Gueltige abgesenkte Funktion wird abgelehnt."
    );

    auto descending = program.front();
    std::swap(
        descending.blocks.front().instructions[0],
        descending.blocks.front().instructions[1]
    );
    require(
        has_issue(
            katana::ir::verify_function(descending),
            "nicht streng aufsteigend"
        ),
        "Nicht aufsteigende Instruktionsadressen werden akzeptiert."
    );

    auto stale_calls = program.front();
    stale_calls.direct_callees.push_back(0x8C020000u);
    require(
        has_issue(
            katana::ir::verify_function(stale_calls),
            "Callee-Metadaten"
        ),
        "Veraltete Call-Metadaten werden akzeptiert."
    );

    auto wrong_successor = program.front();
    wrong_successor.blocks.front().successors.push_back(
        wrong_successor.entry_address
    );
    require(
        has_issue(
            katana::ir::verify_function(wrong_successor),
            "Terminaloperation"
        ),
        "CFG-Nachfolger entgegen der Terminaloperation werden akzeptiert."
    );

    auto missing_effective = program.front();
    auto& effective_instruction =
        missing_effective.blocks.front().instructions.front();
    effective_instruction.operation =
        katana::ir::Operation::LoadLongPcRelative;
    effective_instruction.original_operation = effective_instruction.operation;
    effective_instruction.widths = katana::ir::operation_operand_widths(
        effective_instruction.operation
    );
    effective_instruction.memory_effects = katana::ir::instruction_memory_effects(
        effective_instruction.operation
    );
    require(
        has_issue(
            katana::ir::verify_function(missing_effective),
            "effektive Adresse"
        ),
        "Fehlende erforderliche effektive Adresse wird akzeptiert."
    );

    auto missing_special = program.front();
    auto& special_instruction = missing_special.blocks.front().instructions.front();
    special_instruction.operation = katana::ir::Operation::StoreSpecialRegister;
    special_instruction.original_operation = special_instruction.operation;
    special_instruction.widths = katana::ir::operation_operand_widths(
        special_instruction.operation
    );
    special_instruction.memory_effects = katana::ir::instruction_memory_effects(
        special_instruction.operation
    );
    require(
        has_issue(
            katana::ir::verify_function(missing_special),
            "kein Spezialregister"
        ),
        "Fehlendes erforderliches Spezialregister wird akzeptiert."
    );

    auto invalid_transform = program.front();
    auto& transformed = invalid_transform.blocks.front().instructions.front();
    transformed.operation = katana::ir::Operation::AddImmediate;
    transformed.widths = katana::ir::operation_operand_widths(transformed.operation);
    require(
        has_issue(
            katana::ir::verify_function(invalid_transform),
            "synthetische Operation"
        ),
        "Ursprungsoperation und transformierte IR werden vermischt."
    );

    const std::array<katana::ir::Function, 2> duplicate_entries = {
        program.front(), program.front()
    };
    require(
        has_issue(
            katana::ir::verify_program(duplicate_entries),
            "doppelt vorhanden"
        ),
        "Doppelte Funktionseinstiege werden akzeptiert."
    );

    auto invalid_width = program.front();
    invalid_width.blocks.front().instructions.front().widths.result =
        katana::ir::OperandWidth::Bits8;
    require(
        !katana::ir::verify_function(invalid_width).empty(),
        "Widerspruechliche Operandbreite wird nicht abgelehnt."
    );

    auto orphan_slot = program.front();
    orphan_slot.blocks.front().instructions.front().delay_slot = {
        katana::ir::DelaySlotRole::Slot,
        0x8C00FFFEu
    };
    require(
        !katana::ir::verify_function(orphan_slot).empty(),
        "Verwaister Delay Slot wird nicht abgelehnt."
    );

    auto wrong_distance = program.front();
    auto& wrong_owner = wrong_distance.blocks.front().instructions[1];
    wrong_owner.delay_slot.counterpart_address = wrong_owner.source_address + 4u;
    require(
        has_issue(
            katana::ir::verify_function(wrong_distance),
            "nicht gegenseitig verknuepft"
        ),
        "Nicht direkt folgende Delay-Slot-Gegenadresse wird akzeptiert."
    );

    auto false_owner = program.front();
    false_owner.blocks.front().instructions[1].original_opcode = 0x0009u;
    require(
        has_issue(
            katana::ir::verify_function(false_owner),
            "keinen verzoegerten Originalopcode"
        ),
        "Owner-Rolle bei nicht verzoegertem Opcode wird akzeptiert."
    );

    auto missing_owner = program.front();
    missing_owner.blocks.front().instructions[1].delay_slot = {};
    require(
        has_issue(
            katana::ir::verify_function(missing_owner),
            "keine Owner-Rolle"
        ),
        "Verzoegerter Opcode ohne Owner-Rolle wird akzeptiert."
    );

    auto control_in_slot = program.front();
    control_in_slot.blocks.front().instructions[2].original_opcode = 0x000Bu;
    require(
        has_issue(
            katana::ir::verify_function(control_in_slot),
            "Kontrollflussinstruktion ist als Delay Slot ungueltig"
        ),
        "Kontrollflussopcode im Delay Slot wird akzeptiert."
    );

    auto illegal_slot = program.front();
    auto& illegal = illegal_slot.blocks.front().instructions[2];
    illegal.original_opcode = 0xFFFFu;
    illegal.original_operation = katana::ir::Operation::Unknown;
    illegal.operation = katana::ir::Operation::Unknown;
    illegal.widths = katana::ir::operation_operand_widths(illegal.operation);
    illegal.memory_effects = katana::ir::instruction_memory_effects(illegal.operation);
    illegal.status_effects = katana::ir::instruction_status_effects(illegal.operation);
    illegal.accumulator_effects = katana::ir::operation_accumulator_effects(illegal.operation);
    require(
        katana::ir::verify_function(illegal_slot).empty(),
        "Strukturierte Illegal-Instruction-Semantik wird vom IR-Verifier blockiert."
    );

    bool codegen_rejected = false;
    try {
        const std::array<katana::ir::Function, 1> invalid_program = {invalid_width};
        static_cast<void>(katana::codegen::emit_cpp_program(
            invalid_program,
            invalid_width.entry_address
        ));
    } catch (const std::invalid_argument& error) {
        codegen_rejected = std::string(error.what()).find("Operandbreiten") !=
            std::string::npos;
    }
    require(codegen_rejected, "Codegenerator akzeptiert ungueltige Katana-IR.");

    std::cout << "KR-1905 IR-Verifier erfolgreich.\n";
    return EXIT_SUCCESS;
}
