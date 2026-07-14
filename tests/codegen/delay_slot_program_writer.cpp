#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/ir.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using katana::ir::DelaySlotRole;
using katana::ir::Instruction;
using katana::ir::Operation;

Instruction instruction(const std::uint32_t address, const Operation operation) {
    Instruction result;
    result.source_address = address;
    result.operation = operation;
    switch (operation) {
        case Operation::Branch: result.original_opcode = 0xA000u; break;
        case Operation::Call: result.original_opcode = 0xB000u; break;
        case Operation::JumpRegister: result.original_opcode = 0x402Bu; break;
        case Operation::CallRegister: result.original_opcode = 0x400Bu; break;
        case Operation::Return: result.original_opcode = 0x000Bu; break;
        case Operation::ReturnFromException: result.original_opcode = 0x002Bu; break;
        case Operation::Nop: result.original_opcode = 0x0009u; break;
        case Operation::Unknown: result.original_opcode = 0xFFFFu; break;
        default: break;
    }
    result.widths = katana::ir::operation_operand_widths(operation);
    result.memory_effects = katana::ir::instruction_memory_effects(operation);
    result.status_effects = katana::ir::instruction_status_effects(operation);
    result.accumulator_effects = katana::ir::operation_accumulator_effects(operation);
    return result;
}

Instruction memory_slot(
    const std::uint32_t owner,
    const Operation operation = Operation::LoadLong
) {
    auto result = instruction(owner + 2u, operation);
    result.source_register = 8u;
    result.destination_register = 9u;
    switch (operation) {
        case Operation::LoadLong: result.original_opcode = 0x6982u; break;
        case Operation::LoadLongPostIncrement: result.original_opcode = 0x6986u; break;
        case Operation::StoreLongPreDecrement: result.original_opcode = 0x2896u; break;
        default: break;
    }
    result.memory_effects = katana::ir::instruction_memory_effects(
        operation,
        result.destination_register,
        result.source_register
    );
    result.delay_slot = {DelaySlotRole::Slot, owner};
    return result;
}

Instruction nop_slot(const std::uint32_t owner) {
    auto result = instruction(owner + 2u, Operation::Nop);
    result.delay_slot = {DelaySlotRole::Slot, owner};
    return result;
}

void append_function(
    std::vector<katana::ir::Function>& program,
    const std::uint32_t entry,
    Instruction owner,
    Instruction slot
) {
    owner.delay_slot = {DelaySlotRole::Owner, entry + 2u};
    katana::ir::BasicBlock block;
    block.start_address = entry;
    if (owner.operation == Operation::Branch && owner.target_address == entry) {
        block.successors.push_back(entry);
    }
    if (
        (owner.operation == Operation::JumpRegister ||
            owner.operation == Operation::CallRegister) &&
        owner.resolved_targets.empty()
    ) {
        block.has_indirect_successor = true;
    }
    block.instructions = {std::move(owner), std::move(slot)};

    katana::ir::Function function;
    function.entry_address = entry;
    const auto& control = block.instructions.front();
    if (control.operation == Operation::Call && control.target_address) {
        function.direct_callees.push_back(*control.target_address);
    }
    if (control.operation == Operation::CallRegister) {
        function.indirect_call_sites.push_back(entry);
        function.direct_callees = control.resolved_targets;
    }
    function.blocks = {std::move(block)};
    program.push_back(std::move(function));
}

std::vector<katana::ir::Function> build_program() {
    std::vector<katana::ir::Function> program;

    auto bra = instruction(0x1000u, Operation::Branch);
    bra.target_address = 0x1000u;
    append_function(program, 0x1000u, std::move(bra), memory_slot(0x1000u));

    auto bsr = instruction(0x1100u, Operation::Call);
    bsr.target_address = 0x1800u;
    append_function(program, 0x1100u, std::move(bsr), memory_slot(0x1100u));

    auto jmp = instruction(0x1200u, Operation::JumpRegister);
    jmp.branch_register = 10u;
    jmp.resolved_targets = {0x1800u};
    append_function(program, 0x1200u, std::move(jmp), memory_slot(0x1200u));

    auto jsr = instruction(0x1300u, Operation::CallRegister);
    jsr.branch_register = 10u;
    jsr.resolved_targets = {0x1800u};
    append_function(program, 0x1300u, std::move(jsr), memory_slot(0x1300u));

    append_function(
        program,
        0x1400u,
        instruction(0x1400u, Operation::Return),
        memory_slot(0x1400u, Operation::LoadLongPostIncrement)
    );

    auto rte_slot = memory_slot(0x1500u);
    rte_slot.source_register = 0u;
    rte_slot.memory_effects = katana::ir::instruction_memory_effects(
        rte_slot.operation,
        rte_slot.destination_register,
        rte_slot.source_register
    );
    append_function(
        program,
        0x1500u,
        instruction(0x1500u, Operation::ReturnFromException),
        std::move(rte_slot)
    );

    auto illegal_owner = instruction(0x1600u, Operation::Branch);
    illegal_owner.target_address = 0x1600u;
    append_function(
        program,
        0x1600u,
        std::move(illegal_owner),
        instruction(0x1602u, Operation::Unknown)
    );
    program.back().blocks.front().instructions.back().delay_slot = {
        DelaySlotRole::Slot,
        0x1600u
    };

    auto nested_call = instruction(0x1700u, Operation::Call);
    nested_call.target_address = 0x1800u;
    append_function(program, 0x1700u, std::move(nested_call), nop_slot(0x1700u));

    auto nested_fault = instruction(0x1800u, Operation::Branch);
    nested_fault.target_address = 0x1800u;
    auto nested_slot = memory_slot(0x1800u, Operation::StoreLongPreDecrement);
    nested_slot.source_register = 9u;
    nested_slot.destination_register = 8u;
    nested_slot.memory_effects = katana::ir::instruction_memory_effects(
        nested_slot.operation,
        nested_slot.destination_register,
        nested_slot.source_register
    );
    append_function(
        program,
        0x1800u,
        std::move(nested_fault),
        std::move(nested_slot)
    );

    return program;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        return EXIT_FAILURE;
    }
    const auto source = katana::codegen::emit_cpp_program(build_program(), 0x1000u);
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}
