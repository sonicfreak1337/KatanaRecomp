#include "katana/ir/verifier.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
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
           operation == Operation::BranchIfTrue || operation == Operation::BranchIfFalse;
}

bool requires_effective_address(const Operation operation) noexcept {
    return operation == Operation::LoadWordSignedPcRelative ||
           operation == Operation::LoadLongPcRelative ||
           operation == Operation::MoveAddressPcRelative;
}

bool requires_special_register(const Operation operation) noexcept {
    return operation == Operation::StoreSpecialRegister ||
           operation == Operation::StoreSpecialRegisterPreDecrement ||
           operation == Operation::LoadSpecialRegister ||
           operation == Operation::LoadSpecialRegisterPostIncrement;
}

const Instruction& controlling_instruction(const BasicBlock& block) {
    const auto& last = block.instructions.back();
    if (last.delay_slot.role == DelaySlotRole::Slot && block.instructions.size() >= 2u) {
        return block.instructions[block.instructions.size() - 2u];
    }
    return last;
}

void sort_unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
    return output.str();
}

void add_issue(std::vector<VerificationIssue>& issues,
               const std::uint32_t address,
               std::string message) {
    issues.push_back({address, std::move(message)});
}

void verify_memory_effects(const Instruction& instruction, std::vector<VerificationIssue>& issues) {
    const auto& effects = instruction.memory_effects;
    const bool has_access = effects.access != MemoryAccessKind::None;
    const bool has_update = effects.address_update != AddressUpdateKind::None;

    if (has_access != (effects.width != OperandWidth::None) ||
        has_access != (effects.access_count != 0u)) {
        add_issue(issues,
                  instruction.source_address,
                  "Speicherzugriff, Breite und Zugriffszahl sind inkonsistent.");
    }
    if (has_update != (effects.updated_register_count != 0u) ||
        effects.width != instruction.widths.memory) {
        add_issue(issues,
                  instruction.source_address,
                  "Speicher- und Adressupdate-Metadaten sind inkonsistent.");
    }
    if (!has_access && effects.region != MemoryRegionKind::Unknown) {
        add_issue(
            issues, instruction.source_address, "Speicherregion ist ohne Speicherzugriff gesetzt.");
    }
}

void verify_delay_slot(const BasicBlock& block,
                       const std::size_t index,
                       std::vector<VerificationIssue>& issues) {
    const auto& instruction = block.instructions[index];
    const auto role = instruction.delay_slot.role;
    const auto peer = instruction.delay_slot.counterpart_address;
    const auto decoded = katana::sh4::decode(instruction.original_opcode);
    const bool opcode_has_delay_slot = decoded.has_delay_slot;

    if (role == DelaySlotRole::Owner && !opcode_has_delay_slot) {
        add_issue(issues,
                  instruction.source_address,
                  "Delay-Slot-Owner verwendet keinen verzoegerten Originalopcode.");
    }
    if (role != DelaySlotRole::Owner && opcode_has_delay_slot) {
        add_issue(issues,
                  instruction.source_address,
                  "Verzoegerter Originalopcode besitzt keine Owner-Rolle.");
    }

    if (role == DelaySlotRole::None) {
        if (peer.has_value()) {
            add_issue(issues,
                      instruction.source_address,
                      "Delay-Slot-Rolle None besitzt eine Gegenadresse.");
        }
        return;
    }
    if (!peer.has_value()) {
        add_issue(
            issues, instruction.source_address, "Delay-Slot-Beziehung besitzt keine Gegenadresse.");
        return;
    }

    if (role == DelaySlotRole::Owner) {
        if (!is_control_flow(instruction.operation)) {
            add_issue(issues,
                      instruction.source_address,
                      "Delay-Slot-Owner ist keine Kontrollflussinstruktion.");
        }
        if (index + 1u >= block.instructions.size()) {
            add_issue(issues,
                      instruction.source_address,
                      "Delay-Slot-Owner besitzt keinen folgenden Slot.");
            return;
        }
        const auto& slot = block.instructions[index + 1u];
        if (*peer != instruction.source_address + 2u ||
            slot.source_address != instruction.source_address + 2u ||
            *peer != slot.source_address || slot.delay_slot.role != DelaySlotRole::Slot ||
            slot.delay_slot.counterpart_address != instruction.source_address) {
            add_issue(issues,
                      instruction.source_address,
                      "Delay-Slot-Owner und folgender Slot sind nicht gegenseitig verknuepft.");
        }
        if (index + 2u != block.instructions.size()) {
            add_issue(issues,
                      instruction.source_address,
                      "Nach einem Delay-Slot-Paar stehen weitere Blockinstruktionen.");
        }
        return;
    }

    if (index == 0u) {
        add_issue(
            issues, instruction.source_address, "Delay Slot besitzt keinen vorhergehenden Owner.");
        return;
    }
    const auto& owner = block.instructions[index - 1u];
    if (instruction.source_address != owner.source_address + 2u ||
        *peer != instruction.source_address - 2u || *peer != owner.source_address ||
        owner.delay_slot.role != DelaySlotRole::Owner ||
        owner.delay_slot.counterpart_address != instruction.source_address) {
        add_issue(issues,
                  instruction.source_address,
                  "Delay Slot und vorhergehender Owner sind nicht gegenseitig verknuepft.");
    }
    if (is_control_flow(instruction.operation) || decoded.changes_control_flow()) {
        add_issue(issues,
                  instruction.source_address,
                  "Kontrollflussinstruktion ist als Delay Slot ungueltig.");
    }
}

} // namespace

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
            add_issue(issues,
                      block.start_address,
                      "Blockstart stimmt nicht mit der ersten Instruktion ueberein.");
        }

        bool saw_control_flow = false;
        for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (index != 0u &&
                instruction.source_address <= block.instructions[index - 1u].source_address) {
                add_issue(issues,
                          instruction.source_address,
                          "Instruktionsadressen sind nicht streng aufsteigend.");
            }
            if ((instruction.source_address & 1u) != 0u) {
                add_issue(issues,
                          instruction.source_address,
                          "Instruktionsadresse ist nicht 2-Byte-ausgerichtet.");
            }
            if (!instruction_addresses.insert(instruction.source_address).second) {
                add_issue(issues,
                          instruction.source_address,
                          "Instruktionsadresse ist doppelt vorhanden.");
            }
            if (instruction.original_operation != Operation::Unknown &&
                instruction.operation != instruction.original_operation &&
                instruction.operation != Operation::Constant32) {
                add_issue(
                    issues,
                    instruction.source_address,
                    "Transformierte IR-Operation ist nicht als synthetische Operation modelliert.");
            }
            if (instruction.destination_register >= 16u || instruction.source_register >= 16u ||
                instruction.branch_register >= 16u ||
                (instruction.forwarded_value_register &&
                 *instruction.forwarded_value_register >= 16u)) {
                add_issue(issues,
                          instruction.source_address,
                          "Allgemeines Register liegt ausserhalb R0 bis R15.");
            }
            if (instruction.widths != operation_operand_widths(instruction.operation)) {
                add_issue(issues,
                          instruction.source_address,
                          "Operandbreiten passen nicht zur Operation.");
            }
            if (instruction.status_effects !=
                instruction_status_effects(instruction.operation, instruction.special_register)) {
                add_issue(issues,
                          instruction.source_address,
                          "Statusregistereffekte passen nicht zur Operation.");
            }
            auto expected_memory_effects =
                instruction_memory_effects(instruction.operation,
                                           instruction.destination_register,
                                           instruction.source_register);
            expected_memory_effects.region = instruction.memory_effects.region;
            if (instruction.memory_effects != expected_memory_effects) {
                add_issue(issues,
                          instruction.source_address,
                          "Speichereffekte passen nicht zur Operation.");
            }
            if (instruction.accumulator_effects !=
                operation_accumulator_effects(instruction.operation,
                                              instruction.special_register)) {
                add_issue(issues,
                          instruction.source_address,
                          "Akkumulatoreffekte passen nicht zur Operation.");
            }
            verify_memory_effects(instruction, issues);
            if (instruction.forwarded_value_register) {
                const bool valid_forward =
                    instruction.operation == Operation::LoadLong && index != 0u &&
                    block.instructions[index - 1u].operation == Operation::StoreLong &&
                    block.instructions[index - 1u].destination_register ==
                        instruction.source_register &&
                    block.instructions[index - 1u].source_register ==
                        *instruction.forwarded_value_register &&
                    instruction.memory_effects.region == MemoryRegionKind::NormalRam &&
                    block.instructions[index - 1u].memory_effects.region ==
                        MemoryRegionKind::NormalRam;
                if (!valid_forward) {
                    add_issue(issues,
                              instruction.source_address,
                              "Load-Forwarding besitzt keinen passenden direkten Store.");
                }
            }
            verify_delay_slot(block, index, issues);

            const bool control_flow = is_control_flow(instruction.operation);
            if (saw_control_flow && instruction.delay_slot.role != DelaySlotRole::Slot) {
                add_issue(issues,
                          instruction.source_address,
                          "Normale Instruktion folgt einer Terminalinstruktion.");
            }
            saw_control_flow = saw_control_flow || control_flow;
            if (requires_direct_target(instruction.operation) &&
                !instruction.target_address.has_value()) {
                add_issue(issues,
                          instruction.source_address,
                          "Direkter Kontrollfluss besitzt kein Ziel.");
            }
            if (requires_effective_address(instruction.operation) &&
                !instruction.effective_address) {
                add_issue(issues,
                          instruction.source_address,
                          "Operation besitzt keine erforderliche effektive Adresse.");
            }
            if (requires_special_register(instruction.operation) &&
                instruction.special_register == SpecialRegister::None) {
                add_issue(issues,
                          instruction.source_address,
                          "Spezialregisteroperation besitzt kein Spezialregister.");
            }
            if (!instruction.resolved_targets.empty() &&
                instruction.operation != Operation::JumpRegister &&
                instruction.operation != Operation::CallRegister) {
                add_issue(issues,
                          instruction.source_address,
                          "Aufgeloeste Ziele gehoeren nicht zu indirektem Kontrollfluss.");
            }
            if (!std::is_sorted(instruction.resolved_targets.begin(),
                                instruction.resolved_targets.end()) ||
                std::adjacent_find(instruction.resolved_targets.begin(),
                                   instruction.resolved_targets.end()) !=
                    instruction.resolved_targets.end()) {
                add_issue(issues,
                          instruction.source_address,
                          "Aufgeloeste Kontrollflussziele sind nicht kanonisch.");
            }
        }
    }

    if (!block_addresses.contains(function.entry_address)) {
        add_issue(issues,
                  function.entry_address,
                  "Funktionseintritt besitzt keinen gleichnamigen Block.");
    }
    std::vector<std::uint32_t> expected_callees;
    std::vector<std::uint32_t> expected_indirect_sites;
    for (const auto& block : function.blocks) {
        if (block.instructions.empty()) {
            continue;
        }
        std::vector<std::uint32_t> expected_successors;
        const auto& control = controlling_instruction(block);
        const auto add_internal = [&](const std::uint32_t address) {
            if (block_addresses.contains(address)) {
                expected_successors.push_back(address);
            }
        };
        const auto fallthrough = block.instructions.back().source_address + 2u;
        switch (control.operation) {
        case Operation::Branch:
            if (control.target_address) add_internal(*control.target_address);
            break;
        case Operation::BranchIfTrue:
        case Operation::BranchIfFalse:
            if (control.target_address) add_internal(*control.target_address);
            add_internal(fallthrough);
            break;
        case Operation::Call:
            if (control.target_address) {
                expected_callees.push_back(*control.target_address);
            }
            add_internal(fallthrough);
            break;
        case Operation::CallRegister:
            expected_indirect_sites.push_back(control.source_address);
            expected_callees.insert(expected_callees.end(),
                                    control.resolved_targets.begin(),
                                    control.resolved_targets.end());
            add_internal(fallthrough);
            break;
        case Operation::JumpRegister:
            for (const auto target : control.resolved_targets) {
                add_internal(target);
            }
            break;
        case Operation::Return:
        case Operation::ReturnFromException:
        case Operation::TrapAlways:
        case Operation::Sleep:
            break;
        default:
            add_internal(fallthrough);
            break;
        }
        sort_unique(expected_successors);
        auto actual_successors = block.successors;
        sort_unique(actual_successors);
        if (actual_successors != block.successors) {
            add_issue(issues,
                      block.start_address,
                      "Blocknachfolger sind nicht kanonisch sortiert und eindeutig.");
        }
        if (actual_successors != expected_successors) {
            add_issue(
                issues, block.start_address, "Blocknachfolger passen nicht zur Terminaloperation.");
        }
        const bool expected_indirect = (control.operation == Operation::JumpRegister ||
                                        control.operation == Operation::CallRegister) &&
                                       control.resolved_targets.empty();
        if (block.has_indirect_successor != expected_indirect) {
            add_issue(issues,
                      block.start_address,
                      "Indirekte CFG-Markierung passt nicht zur Terminaloperation.");
        }
    }
    sort_unique(expected_callees);
    sort_unique(expected_indirect_sites);
    auto actual_callees = function.direct_callees;
    auto actual_indirect_sites = function.indirect_call_sites;
    sort_unique(actual_callees);
    sort_unique(actual_indirect_sites);
    if (actual_callees != function.direct_callees || actual_callees != expected_callees) {
        add_issue(issues,
                  function.entry_address,
                  "Direkte Callee-Metadaten sind nicht kanonisch oder veraltet.");
    }
    if (actual_indirect_sites != function.indirect_call_sites ||
        actual_indirect_sites != expected_indirect_sites) {
        add_issue(issues,
                  function.entry_address,
                  "Indirekte Callsite-Metadaten sind nicht kanonisch oder veraltet.");
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
    message << "Ungueltige Katana-IR-Funktion " << hex_address(function.entry_address) << ": "
            << hex_address(issues.front().address) << ": " << issues.front().message;
    throw std::invalid_argument(message.str());
}

std::vector<VerificationIssue> verify_program(const std::span<const Function> functions) {
    std::vector<VerificationIssue> issues;
    std::unordered_set<std::uint32_t> entries;
    for (const auto& function : functions) {
        const auto function_issues = verify_function(function);
        issues.insert(issues.end(), function_issues.begin(), function_issues.end());
        if (!entries.insert(function.entry_address).second) {
            add_issue(issues,
                      function.entry_address,
                      "Funktionseintritt ist im Programm doppelt vorhanden.");
        }
    }
    for (const auto& function : functions) {
        for (const auto callee : function.direct_callees) {
            if (!entries.contains(callee)) {
                add_issue(issues,
                          function.entry_address,
                          "Callee " + hex_address(callee) +
                              " besitzt keinen eindeutigen Funktionseintritt.");
            }
        }
    }
    std::sort(issues.begin(), issues.end(), [](const auto& left, const auto& right) {
        if (left.address != right.address) return left.address < right.address;
        return left.message < right.message;
    });
    return issues;
}

void require_valid_program(const std::span<const Function> functions) {
    const auto issues = verify_program(functions);
    if (issues.empty()) return;
    throw std::invalid_argument(
        "Ungueltiges Katana-IR-Programm: " + hex_address(issues.front().address) + ": " +
        issues.front().message);
}

} // namespace katana::ir
