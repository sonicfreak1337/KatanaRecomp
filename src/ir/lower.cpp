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

        case Source::SubRegister:
            return Operation::SubRegister;

        case Source::NegateRegister:
            return Operation::NegateRegister;

        case Source::NotRegister:
            return Operation::NotRegister;
        case Source::AddWithCarry:
            return Operation::AddWithCarry;

        case Source::AddWithOverflow:
            return Operation::AddWithOverflow;

        case Source::SubWithCarry:
            return Operation::SubWithCarry;

        case Source::SubWithOverflow:
            return Operation::SubWithOverflow;

        case Source::NegateWithCarry:
            return Operation::NegateWithCarry;
        case Source::ExtendUnsignedByte:
            return Operation::ExtendUnsignedByte;

        case Source::ExtendUnsignedWord:
            return Operation::ExtendUnsignedWord;

        case Source::ExtendSignedByte:
            return Operation::ExtendSignedByte;

        case Source::ExtendSignedWord:
            return Operation::ExtendSignedWord;

        case Source::SwapBytes:
            return Operation::SwapBytes;

        case Source::SwapWords:
            return Operation::SwapWords;

        case Source::ExtractMiddle:
            return Operation::ExtractMiddle;
        case Source::DecrementAndTest:
            return Operation::DecrementAndTest;

        case Source::MoveT:
            return Operation::MoveT;
        case Source::ShiftLogicalLeftOne:
            return Operation::ShiftLogicalLeftOne;

        case Source::ShiftLogicalRightOne:
            return Operation::ShiftLogicalRightOne;

        case Source::ShiftArithmeticLeftOne:
            return Operation::ShiftArithmeticLeftOne;

        case Source::ShiftArithmeticRightOne:
            return Operation::ShiftArithmeticRightOne;
        case Source::ShiftLogicalLeftTwo:
            return Operation::ShiftLogicalLeftTwo;

        case Source::ShiftLogicalLeftEight:
            return Operation::ShiftLogicalLeftEight;

        case Source::ShiftLogicalLeftSixteen:
            return Operation::ShiftLogicalLeftSixteen;

        case Source::ShiftLogicalRightTwo:
            return Operation::ShiftLogicalRightTwo;

        case Source::ShiftLogicalRightEight:
            return Operation::ShiftLogicalRightEight;

        case Source::ShiftLogicalRightSixteen:
            return Operation::ShiftLogicalRightSixteen;
        case Source::RotateLeft:
            return Operation::RotateLeft;

        case Source::RotateRight:
            return Operation::RotateRight;

        case Source::RotateLeftThroughT:
            return Operation::RotateLeftThroughT;

        case Source::RotateRightThroughT:
            return Operation::RotateRightThroughT;
        case Source::ShiftArithmeticDynamic:
            return Operation::ShiftArithmeticDynamic;

        case Source::ShiftLogicalDynamic:
            return Operation::ShiftLogicalDynamic;
        case Source::MultiplyLong:
            return Operation::MultiplyLong;

        case Source::MultiplySignedWord:
            return Operation::MultiplySignedWord;

        case Source::MultiplyUnsignedWord:
            return Operation::MultiplyUnsignedWord;
        case Source::DoubleMultiplySignedLong:
            return Operation::DoubleMultiplySignedLong;

        case Source::DoubleMultiplyUnsignedLong:
            return Operation::DoubleMultiplyUnsignedLong;
        case Source::MultiplyAccumulateWord:
            return Operation::MultiplyAccumulateWord;

        case Source::MultiplyAccumulateLong:
            return Operation::MultiplyAccumulateLong;
        case Source::DivideInitializeUnsigned:
            return Operation::DivideInitializeUnsigned;

        case Source::DivideInitializeSigned:
            return Operation::DivideInitializeSigned;

        case Source::DivideStep:
            return Operation::DivideStep;
        case Source::AndRegister:
            return Operation::AndRegister;

        case Source::OrRegister:
            return Operation::OrRegister;

        case Source::XorRegister:
            return Operation::XorRegister;

        case Source::AndImmediate:
            return Operation::AndImmediate;

        case Source::OrImmediate:
            return Operation::OrImmediate;

        case Source::XorImmediate:
            return Operation::XorImmediate;
        case Source::ClearS:
            return Operation::ClearS;

        case Source::SetS:
            return Operation::SetS;
        case Source::ClearT:
            return Operation::ClearT;

        case Source::SetT:
            return Operation::SetT;

        case Source::CompareEqualImmediate:
            return Operation::CompareEqualImmediate;

        case Source::CompareEqualRegister:
            return Operation::CompareEqualRegister;

        case Source::CompareHigherOrSame:
            return Operation::CompareHigherOrSame;

        case Source::CompareGreaterOrEqual:
            return Operation::CompareGreaterOrEqual;

        case Source::CompareHigher:
            return Operation::CompareHigher;

        case Source::CompareGreaterThan:
            return Operation::CompareGreaterThan;

        case Source::ComparePositiveOrZero:
            return Operation::ComparePositiveOrZero;

        case Source::ComparePositive:
            return Operation::ComparePositive;

        case Source::CompareString:
            return Operation::CompareString;
        case Source::TestImmediate:
            return Operation::TestImmediate;

        case Source::TestRegister:
            return Operation::TestRegister;
        case Source::MovByteLoad:
            return Operation::LoadByteSigned;

        case Source::MovWordLoad:
            return Operation::LoadWordSigned;

        case Source::MovLongLoad:
            return Operation::LoadLong;

        case Source::MovByteStore:
            return Operation::StoreByte;

        case Source::MovWordStore:
            return Operation::StoreWord;

        case Source::MovLongStore:
            return Operation::StoreLong;

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

        case Operation::SubRegister:
            return "sub_reg";

        case Operation::NegateRegister:
            return "neg_reg";

        case Operation::NotRegister:
            return "not_reg";
        case Operation::AddWithCarry:
            return "add_with_carry";

        case Operation::AddWithOverflow:
            return "add_with_overflow";

        case Operation::SubWithCarry:
            return "sub_with_carry";

        case Operation::SubWithOverflow:
            return "sub_with_overflow";

        case Operation::NegateWithCarry:
            return "negate_with_carry";
        case Operation::ExtendUnsignedByte:
            return "extend_unsigned_byte";

        case Operation::ExtendUnsignedWord:
            return "extend_unsigned_word";

        case Operation::ExtendSignedByte:
            return "extend_signed_byte";

        case Operation::ExtendSignedWord:
            return "extend_signed_word";

        case Operation::SwapBytes:
            return "swap_bytes";

        case Operation::SwapWords:
            return "swap_words";

        case Operation::ExtractMiddle:
            return "extract_middle";
        case Operation::DecrementAndTest:
            return "decrement_and_test";

        case Operation::MoveT:
            return "move_t";
        case Operation::ShiftLogicalLeftOne:
            return "shift_logical_left_one";

        case Operation::ShiftLogicalRightOne:
            return "shift_logical_right_one";

        case Operation::ShiftArithmeticLeftOne:
            return "shift_arithmetic_left_one";

        case Operation::ShiftArithmeticRightOne:
            return "shift_arithmetic_right_one";
        case Operation::ShiftLogicalLeftTwo:
            return "shift_logical_left_two";

        case Operation::ShiftLogicalLeftEight:
            return "shift_logical_left_eight";

        case Operation::ShiftLogicalLeftSixteen:
            return "shift_logical_left_sixteen";

        case Operation::ShiftLogicalRightTwo:
            return "shift_logical_right_two";

        case Operation::ShiftLogicalRightEight:
            return "shift_logical_right_eight";

        case Operation::ShiftLogicalRightSixteen:
            return "shift_logical_right_sixteen";
        case Operation::RotateLeft:
            return "rotate_left";

        case Operation::RotateRight:
            return "rotate_right";

        case Operation::RotateLeftThroughT:
            return "rotate_left_through_t";

        case Operation::RotateRightThroughT:
            return "rotate_right_through_t";
        case Operation::ShiftArithmeticDynamic:
            return "shift_arithmetic_dynamic";

        case Operation::ShiftLogicalDynamic:
            return "shift_logical_dynamic";
        case Operation::MultiplyLong:
            return "multiply_long";

        case Operation::MultiplySignedWord:
            return "multiply_signed_word";

        case Operation::MultiplyUnsignedWord:
            return "multiply_unsigned_word";
        case Operation::DoubleMultiplySignedLong:
            return "double_multiply_signed_long";

        case Operation::DoubleMultiplyUnsignedLong:
            return "double_multiply_unsigned_long";
        case Operation::MultiplyAccumulateWord:
            return "multiply_accumulate_word";

        case Operation::MultiplyAccumulateLong:
            return "multiply_accumulate_long";
        case Operation::DivideInitializeUnsigned:
            return "divide_initialize_unsigned";

        case Operation::DivideInitializeSigned:
            return "divide_initialize_signed";

        case Operation::DivideStep:
            return "divide_step";
        case Operation::AndRegister:
            return "and_reg";

        case Operation::OrRegister:
            return "or_reg";

        case Operation::XorRegister:
            return "xor_reg";

        case Operation::AndImmediate:
            return "and_imm";

        case Operation::OrImmediate:
            return "or_imm";

        case Operation::XorImmediate:
            return "xor_imm";
        case Operation::ClearS:
            return "clear_s";

        case Operation::SetS:
            return "set_s";
        case Operation::ClearT:
            return "clear_t";

        case Operation::SetT:
            return "set_t";

        case Operation::CompareEqualImmediate:
            return "compare_equal_imm";

        case Operation::CompareEqualRegister:
            return "compare_equal_reg";

        case Operation::CompareHigherOrSame:
            return "compare_higher_or_same";

        case Operation::CompareGreaterOrEqual:
            return "compare_greater_or_equal";

        case Operation::CompareHigher:
            return "compare_higher";

        case Operation::CompareGreaterThan:
            return "compare_greater_than";

        case Operation::ComparePositiveOrZero:
            return "compare_positive_or_zero";

        case Operation::ComparePositive:
            return "compare_positive";

        case Operation::CompareString:
            return "compare_string";
        case Operation::TestImmediate:
            return "test_imm";

        case Operation::TestRegister:
            return "test_reg";
        case Operation::LoadByteSigned:
            return "load_s8";

        case Operation::LoadWordSigned:
            return "load_s16";

        case Operation::LoadLong:
            return "load_u32";

        case Operation::StoreByte:
            return "store_u8";

        case Operation::StoreWord:
            return "store_u16";

        case Operation::StoreLong:
            return "store_u32";

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
