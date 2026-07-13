#include "katana/ir/lower.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::ir {
namespace {

Operation lower_operation(
    const katana::sh4::InstructionKind kind
) {
    using Source = katana::sh4::InstructionKind;

    switch (kind) {
        case Source::Nop:
            return Operation::Nop;

        case Source::MovImmediate:
            return Operation::MovImmediate;

        case Source::AddImmediate:
            return Operation::AddImmediate;

        case Source::MovRegister:
            return Operation::MovRegister;

        case Source::AddRegister:
            return Operation::AddRegister;

        case Source::Bra:
            return Operation::Branch;

        case Source::Bsr:
            return Operation::Call;

        case Source::Bt:
        case Source::BtS:
            return Operation::BranchIfTrue;

        case Source::Bf:
        case Source::BfS:
            return Operation::BranchIfFalse;

        case Source::Jmp:
            return Operation::JumpRegister;

        case Source::Jsr:
            return Operation::CallRegister;

        case Source::Rts:
            return Operation::Return;

        case Source::Unknown:
            return Operation::Unknown;
    }

    return Operation::Unknown;
}

Instruction lower_instruction(
    const katana::sh4::DisassemblyLine& source
) {
    Instruction result;

    result.source_address = source.address;
    result.original_opcode = source.opcode;
    result.operation = lower_operation(source.instruction.kind);

    result.destination_register =
        source.instruction.destination_register;
    result.source_register =
        source.instruction.source_register;
    result.branch_register =
        source.instruction.branch_register;

    result.immediate = source.instruction.immediate;
    result.target_address = source.target_address;

    result.has_delay_slot =
        source.instruction.has_delay_slot;
    result.is_delay_slot =
        source.is_delay_slot;

    return result;
}

}

std::string_view operation_name(
    const Operation operation
) noexcept {
    switch (operation) {
        case Operation::Unknown:
            return "unknown";

        case Operation::Nop:
            return "nop";

        case Operation::MovImmediate:
            return "mov_imm";

        case Operation::AddImmediate:
            return "add_imm";

        case Operation::MovRegister:
            return "mov_reg";

        case Operation::AddRegister:
            return "add_reg";

        case Operation::Branch:
            return "branch";

        case Operation::Call:
            return "call";

        case Operation::BranchIfTrue:
            return "branch_if_true";

        case Operation::BranchIfFalse:
            return "branch_if_false";

        case Operation::JumpRegister:
            return "jump_register";

        case Operation::CallRegister:
            return "call_register";

        case Operation::Return:
            return "return";
    }

    return "unknown";
}

Function lower_function(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const katana::analysis::FunctionInfo& function
) {
    const auto source_blocks =
        katana::analysis::build_basic_blocks(lines);

    std::unordered_map<std::uint32_t, const katana::analysis::BasicBlock*>
        block_by_address;

    block_by_address.reserve(source_blocks.size());

    for (const auto& block : source_blocks) {
        block_by_address.emplace(
            block.start_address,
            &block
        );
    }

    Function result;
    result.entry_address = function.entry_address;
    result.direct_callees = function.direct_callees;
    result.indirect_call_sites = function.indirect_call_sites;

    result.blocks.reserve(function.block_addresses.size());

    for (const auto block_address : function.block_addresses) {
        const auto block_iterator =
            block_by_address.find(block_address);

        if (block_iterator == block_by_address.end()) {
            throw std::runtime_error(
                "Ein Funktionsblock wurde in der CFG nicht gefunden."
            );
        }

        const auto& source_block =
            *block_iterator->second;

        BasicBlock target_block;
        target_block.start_address =
            source_block.start_address;
        target_block.successors =
            source_block.successors;
        target_block.has_indirect_successor =
            source_block.has_indirect_successor;

        target_block.instructions.reserve(
            source_block.lines.size()
        );

        for (const auto& source_line : source_block.lines) {
            target_block.instructions.push_back(
                lower_instruction(source_line)
            );
        }

        result.blocks.push_back(
            std::move(target_block)
        );
    }

    std::sort(
        result.blocks.begin(),
        result.blocks.end(),
        [](const BasicBlock& left, const BasicBlock& right) {
            return left.start_address < right.start_address;
        }
    );

    return result;
}

std::vector<Function> lower_program(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const katana::analysis::FunctionInfo> functions
) {
    std::vector<Function> result;
    result.reserve(functions.size());

    for (const auto& function : functions) {
        result.push_back(
            lower_function(lines, function)
        );
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const Function& left, const Function& right) {
            return left.entry_address < right.entry_address;
        }
    );

    return result;
}

}
