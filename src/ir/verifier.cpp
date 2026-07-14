#include "katana/ir/verifier.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace katana::ir {
namespace {

bool is_control_flow(const Operation operation) noexcept {
    switch (operation) {
        case Operation::TrapAlways:
        case Operation::ReturnFromException:
        case Operation::Sleep:
        case Operation::Branch:
        case Operation::Call:
        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse:
        case Operation::JumpRegister:
        case Operation::CallRegister:
        case Operation::Return:
            return true;
        default:
            return false;
    }
}

bool requires_direct_target(const Operation operation) noexcept {
    return operation == Operation::Branch || operation == Operation::Call ||
        operation == Operation::BranchIfTrue ||
        operation == Operation::BranchIfFalse;
}

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
        << std::setfill('0') << address;
    return output.str();
}

void add_issue(
    std::vector<VerificationIssue>& issues,
    const std::uint32_t address,
    std::string message
) {
    issues.push_back({address, std::move(message)});
}

void verify_memory_effects(
    const Instruction& instruction,
    std::vector<VerificationIssue>& issues
) {
    const auto& effects = instruction.memory_effects;
    const bool has_access = effects.access != MemoryAccessKind::None;
    const bool has_update = effects.address_update != AddressUpdateKind::None;

    if (
        has_access != (effects.width != OperandWidth::None) ||
        has_access != (effects.access_count != 0u)
    ) {
        add_issue(issues, instruction.source_address,
            "Speicherzugriff, Breite und Zugriffszahl sind inkonsistent.");
    }
    if (
        has_update != (effects.updated_register_count != 0u) ||
        effects.width != instruction.widths.memory
    ) {
        add_issue(issues, instruction.source_address,
            "Speicher- und Adressupdate-Metadaten sind inkonsistent.");
    }
}

void verify_delay_slot(
    const BasicBlock& block,
    const std::size_t index,
    std::vector<VerificationIssue>& issues
) {
    const auto& instruction = block.instructions[index];
    const auto role = instruction.delay_slot.role;
    const auto peer = instruction.delay_slot.counterpart_address;
    const auto decoded = katana::sh4::decode(instruction.original_opcode);
    const bool opcode_has_delay_slot = decoded.has_delay_slot;

    if (role == DelaySlotRole::Owner && !opcode_has_delay_slot) {
        add_issue(issues, instruction.source_address,
            "Delay-Slot-Owner verwendet keinen verzoegerten Originalopcode.");
    }
    if (role != DelaySlotRole::Owner && opcode_has_delay_slot) {
        add_issue(issues, instruction.source_address,
            "Verzoegerter Originalopcode besitzt keine Owner-Rolle.");
    }

    if (role == DelaySlotRole::None) {
        if (peer.has_value()) {
            add_issue(issues, instruction.source_address,
                "Delay-Slot-Rolle None besitzt eine Gegenadresse.");
        }
        return;
    }
    if (!peer.has_value()) {
        add_issue(issues, instruction.source_address,
            "Delay-Slot-Beziehung besitzt keine Gegenadresse.");
        return;
    }

    if (role == DelaySlotRole::Owner) {
        if (!is_control_flow(instruction.operation)) {
            add_issue(issues, instruction.source_address,
                "Delay-Slot-Owner ist keine Kontrollflussinstruktion.");
        }
        if (index + 1u >= block.instructions.size()) {
            add_issue(issues, instruction.source_address,
                "Delay-Slot-Owner besitzt keinen folgenden Slot.");
            return;
        }
        const auto& slot = block.instructions[index + 1u];
        if (
            *peer != instruction.source_address + 2u ||
            slot.source_address != instruction.source_address + 2u ||
            *peer != slot.source_address ||
            slot.delay_slot.role != DelaySlotRole::Slot ||
            slot.delay_slot.counterpart_address != instruction.source_address
        ) {
            add_issue(issues, instruction.source_address,
                "Delay-Slot-Owner und folgender Slot sind nicht gegenseitig verknuepft.");
        }
        if (index + 2u != block.instructions.size()) {
            add_issue(issues, instruction.source_address,
                "Nach einem Delay-Slot-Paar stehen weitere Blockinstruktionen.");
        }
        return;
    }

    if (index == 0u) {
        add_issue(issues, instruction.source_address,
            "Delay Slot besitzt keinen vorhergehenden Owner.");
        return;
    }
    const auto& owner = block.instructions[index - 1u];
    if (
        instruction.source_address != owner.source_address + 2u ||
        *peer != instruction.source_address - 2u ||
        *peer != owner.source_address ||
        owner.delay_slot.role != DelaySlotRole::Owner ||
        owner.delay_slot.counterpart_address != instruction.source_address
    ) {
        add_issue(issues, instruction.source_address,
            "Delay Slot und vorhergehender Owner sind nicht gegenseitig verknuepft.");
    }
    if (is_control_flow(instruction.operation) || decoded.changes_control_flow()) {
        add_issue(issues, instruction.source_address,
            "Kontrollflussinstruktion ist als Delay Slot ungueltig.");
    }
}

}

std::vector<VerificationIssue> verify_function(const Function& function) {
    std::vector<VerificationIssue> issues;
    std::unordered_set<std::uint32_t> block_addresses;
    std::unordered_set<std::uint32_t> instruction_addresses;

    if (function.blocks.empty()) {
        add_issue(issues, function.entry_address, "Funktion besitzt keine Bloecke.");
        return issues;
    }

    for (const auto& block : function.blocks) {
        if (!block_addresses.insert(block.start_address).second) {
            add_issue(issues, block.start_address, "Blockadresse ist doppelt vorhanden.");
        }
        if (block.instructions.empty()) {
            add_issue(issues, block.start_address, "Block besitzt keine Instruktionen.");
            continue;
        }
        if (block.instructions.front().source_address != block.start_address) {
            add_issue(issues, block.start_address,
                "Blockstart stimmt nicht mit der ersten Instruktion ueberein.");
        }

        bool saw_control_flow = false;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if ((instruction.source_address & 1u) != 0u) {
                add_issue(issues, instruction.source_address,
                    "Instruktionsadresse ist nicht 2-Byte-ausgerichtet.");
            }
            if (!instruction_addresses.insert(instruction.source_address).second) {
                add_issue(issues, instruction.source_address,
                    "Instruktionsadresse ist doppelt vorhanden.");
            }
            if (instruction.operation == Operation::Unknown) {
                add_issue(issues, instruction.source_address,
                    "Unbekannte Operation ist keine gueltige Katana-IR.");
            }
            if (
                instruction.destination_register >= 16u ||
                instruction.source_register >= 16u ||
                instruction.branch_register >= 16u ||
                (instruction.forwarded_value_register &&
                    *instruction.forwarded_value_register >= 16u)
            ) {
                add_issue(issues, instruction.source_address,
                    "Allgemeines Register liegt ausserhalb R0 bis R15.");
            }
            if (instruction.widths != operation_operand_widths(instruction.operation)) {
                add_issue(issues, instruction.source_address,
                    "Operandbreiten passen nicht zur Operation.");
            }
            if (
                instruction.status_effects != instruction_status_effects(
                    instruction.operation, instruction.special_register
                )
            ) {
                add_issue(issues, instruction.source_address,
                    "Statusregistereffekte passen nicht zur Operation.");
            }
            if (
                instruction.memory_effects != instruction_memory_effects(
                    instruction.operation,
                    instruction.destination_register,
                    instruction.source_register
                )
            ) {
                add_issue(issues, instruction.source_address,
                    "Speichereffekte passen nicht zur Operation.");
            }
            if (
                instruction.accumulator_effects !=
                    operation_accumulator_effects(instruction.operation)
            ) {
                add_issue(issues, instruction.source_address,
                    "Akkumulatoreffekte passen nicht zur Operation.");
            }
            verify_memory_effects(instruction, issues);
            if (instruction.forwarded_value_register) {
                const bool valid_forward =
                    instruction.operation == Operation::LoadLong &&
                    index != 0u &&
                    block.instructions[index - 1u].operation == Operation::StoreLong &&
                    block.instructions[index - 1u].destination_register ==
                        instruction.source_register &&
                    block.instructions[index - 1u].source_register ==
                        *instruction.forwarded_value_register;
                if (!valid_forward) {
                    add_issue(issues, instruction.source_address,
                        "Load-Forwarding besitzt keinen passenden direkten Store.");
                }
            }
            verify_delay_slot(block, index, issues);

            const bool control_flow = is_control_flow(instruction.operation);
            if (saw_control_flow && instruction.delay_slot.role != DelaySlotRole::Slot) {
                add_issue(issues, instruction.source_address,
                    "Normale Instruktion folgt einer Terminalinstruktion.");
            }
            saw_control_flow = saw_control_flow || control_flow;
            if (requires_direct_target(instruction.operation) &&
                !instruction.target_address.has_value()) {
                add_issue(issues, instruction.source_address,
                    "Direkter Kontrollfluss besitzt kein Ziel.");
            }
        }
    }

    if (!block_addresses.contains(function.entry_address)) {
        add_issue(issues, function.entry_address,
            "Funktionseintritt besitzt keinen gleichnamigen Block.");
    }
    for (const auto& block : function.blocks) {
        for (const auto successor : block.successors) {
            if (!block_addresses.contains(successor)) {
                add_issue(issues, block.start_address,
                    "Blocknachfolger " + hex_address(successor) +
                    " ist in der Funktion nicht vorhanden.");
            }
        }
    }

    std::sort(issues.begin(), issues.end(), [](const auto& left, const auto& right) {
        if (left.address != right.address) return left.address < right.address;
        return left.message < right.message;
    });
    return issues;
}

void require_valid_function(const Function& function) {
    const auto issues = verify_function(function);
    if (issues.empty()) return;

    std::ostringstream message;
    message << "Ungueltige Katana-IR-Funktion " << hex_address(function.entry_address)
        << ": " << hex_address(issues.front().address) << ": "
        << issues.front().message;
    throw std::invalid_argument(message.str());
}

}
